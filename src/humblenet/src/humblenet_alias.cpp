#include "humblenet_p2p_internal.h"
#include "humblenet_utils.h"

#include <cassert>
#include <chrono>
#include <map>
#include <utility>
#include <vector>

#define VIRTUAL_PEER 0x80000000

static BidirectionalMap<PeerId, std::string> virtualPeerNames;
static BidirectionalMap<PeerId, Connection*> virtualPeerConnections;

static PeerId nextVirtualPeer = 0;

static std::string virtualName;

namespace {
	static const uint32_t kAliasLookupTimeoutMs = 10000;
	static const uint32_t kAliasHealthIntervalMs = 30000;

	void internal_alias_lookup_timeout(void*);
	void internal_alias_health_check(void*);
	void reconcile_alias_work(const std::string& alias);
	void schedule_alias_lookup_timeout();
	void schedule_alias_health_check();
	bool request_alias_lookup(const std::string& alias, AliasLookupPurpose purpose);

	bool signaling_ready()
	{
		return humbleNetState.myPeerId != 0 && humbleNetState.p2pConn && humbleNetState.p2pConn->wsi;
	}

	uint64_t now_ms()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	uint64_t bump_alias_revision(const std::string& alias)
	{
		humbleNetState.pendingAliasHealthChecks.erase(alias);
		return ++humbleNetState.aliasIntentRevision[alias];
	}

	void schedule_alias_lookup_timeout()
	{
		if (humbleNetState.aliasLookups.empty()) {
			humbleNetState.aliasLookupTimeoutScheduled = false;
			humbleNetState.aliasLookupTimerDeadlineMs = 0;
			return;
		}

		uint64_t earliest = 0;
		for (const auto& it : humbleNetState.aliasLookups) {
			if (earliest == 0 || it.second.deadlineMs < earliest) {
				earliest = it.second.deadlineMs;
			}
		}

		if (humbleNetState.aliasLookupTimeoutScheduled &&
			humbleNetState.aliasLookupTimerDeadlineMs <= earliest) {
			return;
		}

		uint64_t now = now_ms();
		uint64_t delay = earliest > now ? earliest - now : 0;
		if (delay > kAliasLookupTimeoutMs) {
			delay = kAliasLookupTimeoutMs;
		}

		humbleNetState.aliasLookupTimeoutScheduled = true;
		humbleNetState.aliasLookupTimerDeadlineMs = earliest;
		uintptr_t generation = ++humbleNetState.aliasLookupTimerGeneration;
		humblenet_timer(internal_alias_lookup_timeout, static_cast<int>(delay),
			reinterpret_cast<void*>(generation));
	}

	bool request_alias_lookup(const std::string& alias, AliasLookupPurpose purpose)
	{
		auto existing = humbleNetState.aliasLookups.find(alias);
		if (existing != humbleNetState.aliasLookups.end()) {
			return true;
		}

		if (!signaling_ready() || !humblenet::sendAliasLookup(humbleNetState.p2pConn.get(), alias)) {
			return false;
		}

		AliasLookupInFlight lookup;
		lookup.purpose = purpose;
		lookup.signalingGeneration = humbleNetState.reconnectGeneration;
		lookup.intentRevision = humbleNetState.aliasIntentRevision[alias];
		lookup.deadlineMs = now_ms() + kAliasLookupTimeoutMs;
		humbleNetState.aliasLookups.emplace(alias, lookup);
		schedule_alias_lookup_timeout();
		return true;
	}

	void remember_deferred_lookup(const std::string& alias)
	{
		humbleNetState.aliasWorkDeferred = true;
		LOG("Failed to schedule alias read-back lookup for \"%s\"\n", alias.c_str());
	}

	bool alias_needs_health(const std::string& alias)
	{
		return humbleNetState.desiredAliases.find(alias) != humbleNetState.desiredAliases.end() &&
			humbleNetState.sessionAliases.find(alias) != humbleNetState.sessionAliases.end();
	}

	bool has_alias_health_work()
	{
		for (const auto& alias : humbleNetState.desiredAliases) {
			if (alias_needs_health(alias)) {
				return true;
			}
		}
		return false;
	}

	bool send_alias_register_once(const std::string& alias)
	{
		if (!signaling_ready() || !humblenet::sendAliasRegister(humbleNetState.p2pConn.get(), alias)) {
			return false;
		}

		humbleNetState.pendingAliasRegistrations.insert(alias);
		humbleNetState.confirmedAliases.erase(alias);
		if (!request_alias_lookup(alias, AliasLookupPurpose::Acquire)) {
			remember_deferred_lookup(alias);
		}
		return true;
	}

	bool send_alias_unregister_once(const std::string& alias)
	{
		if (!signaling_ready() || !humblenet::sendAliasUnregister(humbleNetState.p2pConn.get(), alias)) {
			return false;
		}
		return true;
	}

	void finish_alias_unregister(const std::string& alias)
	{
		humbleNetState.pendingAliasUnregistrations.erase(alias);
		humbleNetState.sessionAliases.erase(alias);
		humbleNetState.confirmedAliases.erase(alias);
		humbleNetState.pendingAliasRegistrations.erase(alias);
		LOG("Alias \"%s\" unregister confirmed\n", alias.c_str());
	}

	void reconcile_alias_work(const std::string& alias)
	{
		if (!signaling_ready()) {
			return;
		}

		if (humbleNetState.aliasLookups.find(alias) != humbleNetState.aliasLookups.end()) {
			return;
		}

		if (humbleNetState.oldSessionAliasOwners.find(alias) !=
			humbleNetState.oldSessionAliasOwners.end()) {
			if (!request_alias_lookup(alias, AliasLookupPurpose::OldSessionCheck)) {
				humbleNetState.aliasWorkDeferred = true;
			}
			return;
		}

		auto pendingUnregister = humbleNetState.pendingAliasUnregistrations.find(alias);
		if (pendingUnregister != humbleNetState.pendingAliasUnregistrations.end()) {
			if (pendingUnregister->second.phase != PendingUnregisterPhase::Failed &&
				!request_alias_lookup(alias, AliasLookupPurpose::Unregister)) {
				humbleNetState.aliasWorkDeferred = true;
			}
			return;
		}

		if (humbleNetState.desiredAliases.find(alias) != humbleNetState.desiredAliases.end() &&
			humbleNetState.blockedAliasAcquisitions.find(alias) == humbleNetState.blockedAliasAcquisitions.end() &&
			humbleNetState.confirmedAliases.find(alias) == humbleNetState.confirmedAliases.end() &&
			humbleNetState.pendingAliasHealthChecks.find(alias) == humbleNetState.pendingAliasHealthChecks.end()) {
			if (humbleNetState.sessionAliases.find(alias) == humbleNetState.sessionAliases.end() &&
				humbleNetState.pendingAliasRegistrations.find(alias) == humbleNetState.pendingAliasRegistrations.end()) {
				if (!send_alias_register_once(alias)) {
					humbleNetState.aliasWorkDeferred = true;
				}
				return;
			}
			if (!request_alias_lookup(alias, AliasLookupPurpose::Acquire)) {
				humbleNetState.aliasWorkDeferred = true;
			}
			return;
		}

		if (humbleNetState.pendingAliasConnectionsOut.find(alias) !=
			humbleNetState.pendingAliasConnectionsOut.end()) {
			request_alias_lookup(alias, AliasLookupPurpose::Connect);
			return;
		}

		if (humbleNetState.pendingAliasHealthChecks.find(alias) !=
			humbleNetState.pendingAliasHealthChecks.end()) {
			if (!request_alias_lookup(alias, AliasLookupPurpose::Health)) {
				humbleNetState.pendingAliasHealthChecks.erase(alias);
				humbleNetState.confirmedAliases.erase(alias);
				humblenet_signaling_force_reconnect("alias health lookup send failed");
				return;
			}
			humbleNetState.pendingAliasHealthChecks.erase(alias);
		}
	}

	void internal_alias_lookup_timeout(void* data)
	{
		HUMBLENET_GUARD();

		uintptr_t generation = reinterpret_cast<uintptr_t>(data);
		if (generation != humbleNetState.aliasLookupTimerGeneration) {
			return;
		}

		humbleNetState.aliasLookupTimeoutScheduled = false;
		humbleNetState.aliasLookupTimerDeadlineMs = 0;
		uint64_t now = now_ms();
		for (auto it = humbleNetState.aliasLookups.begin(); it != humbleNetState.aliasLookups.end(); ++it) {
			if (it->second.deadlineMs <= now) {
				std::string alias = it->first;
				AliasLookupInFlight lookup = it->second;
				humbleNetState.aliasLookups.erase(it);
				LOG("Alias lookup for \"%s\" timed out\n", alias.c_str());
				if (lookup.purpose == AliasLookupPurpose::Health) {
					humbleNetState.confirmedAliases.erase(alias);
				}
				humblenet_signaling_force_reconnect("alias lookup timed out");
				return;
			}
		}

		schedule_alias_lookup_timeout();
	}

	void schedule_alias_health_check()
	{
#ifndef EMSCRIPTEN
		if (humbleNetState.aliasHealthCheckScheduled || !has_alias_health_work()) {
			return;
		}

		humbleNetState.aliasHealthCheckScheduled = true;
		uintptr_t generation = ++humbleNetState.aliasHealthCheckGeneration;
		humblenet_timer(internal_alias_health_check, kAliasHealthIntervalMs,
			reinterpret_cast<void*>(generation));
#endif
	}

	void internal_alias_health_check(void* data)
	{
#ifdef EMSCRIPTEN
		(void)data;
		return;
#else
		HUMBLENET_GUARD();

		uintptr_t generation = reinterpret_cast<uintptr_t>(data);
		if (generation != humbleNetState.aliasHealthCheckGeneration) {
			return;
		}

		humbleNetState.aliasHealthCheckScheduled = false;
		if (!signaling_ready()) {
			return;
		}

		std::vector<std::string> aliases;
		for (const auto& alias : humbleNetState.desiredAliases) {
			if (alias_needs_health(alias)) {
				aliases.push_back(alias);
			}
		}

		for (const auto& alias : aliases) {
			humbleNetState.pendingAliasHealthChecks.insert(alias);
			reconcile_alias_work(alias);
			if (!signaling_ready()) {
				return;
			}
		}

		schedule_alias_health_check();
#endif
	}

	std::vector<std::string> alias_snapshot()
	{
		std::unordered_set<std::string> aliases;
		aliases.insert(humbleNetState.desiredAliases.begin(), humbleNetState.desiredAliases.end());
		aliases.insert(humbleNetState.sessionAliases.begin(), humbleNetState.sessionAliases.end());
		aliases.insert(humbleNetState.pendingAliasRegistrations.begin(), humbleNetState.pendingAliasRegistrations.end());
		for (const auto& it : humbleNetState.pendingAliasUnregistrations) {
			aliases.insert(it.first);
		}
		for (const auto& it : humbleNetState.oldSessionAliasOwners) {
			aliases.insert(it.first);
		}
		return std::vector<std::string>(aliases.begin(), aliases.end());
	}

	void handle_old_session_resolution(const std::string& alias, PeerId peer)
	{
		auto oldOwner = humbleNetState.oldSessionAliasOwners.find(alias);
		if (oldOwner == humbleNetState.oldSessionAliasOwners.end()) {
			return;
		}

		PeerId previousPeerId = oldOwner->second;
		humbleNetState.oldSessionAliasOwners.erase(oldOwner);

		if (peer == 0 || peer == previousPeerId) {
			if (humbleNetState.desiredAliases.find(alias) != humbleNetState.desiredAliases.end() &&
				humbleNetState.sessionAliases.find(alias) == humbleNetState.sessionAliases.end() &&
				humbleNetState.pendingAliasRegistrations.find(alias) == humbleNetState.pendingAliasRegistrations.end()) {
				if (peer == previousPeerId) {
					LOG("Alias \"%s\" still resolves to previous peer %u; attempting takeover\n",
						alias.c_str(), previousPeerId);
				}
				if (!send_alias_register_once(alias)) {
					humbleNetState.aliasWorkDeferred = true;
				}
			}
			return;
		}

		LOG("Alias \"%s\" still resolves to peer %u after fresh session (previous peer %u); not taking over\n",
			alias.c_str(), peer, previousPeerId);
		if (humbleNetState.desiredAliases.find(alias) != humbleNetState.desiredAliases.end()) {
			humbleNetState.blockedAliasAcquisitions.insert(alias);
		}
		humbleNetState.pendingAliasRegistrations.erase(alias);
		humbleNetState.sessionAliases.erase(alias);
		humbleNetState.confirmedAliases.erase(alias);
	}

	void handle_unregister_resolution(const std::string& alias, PeerId peer)
	{
		auto it = humbleNetState.pendingAliasUnregistrations.find(alias);
		if (it == humbleNetState.pendingAliasUnregistrations.end()) {
			return;
		}

		if (peer != humbleNetState.myPeerId || peer == 0) {
			finish_alias_unregister(alias);
			return;
		}

		switch (it->second.phase) {
			case PendingUnregisterPhase::InitialReadback:
				it->second.phase = PendingUnregisterPhase::Recovering;
				LOG("Alias \"%s\" still resolves to this peer; resetting signaling before unregister retry\n",
					alias.c_str());
				humblenet_signaling_force_reconnect("alias unregister still owned");
				return;
			case PendingUnregisterPhase::Recovering:
				if (!send_alias_unregister_once(alias)) {
					LOG("Failed to retry alias unregister for \"%s\"\n", alias.c_str());
					humblenet_signaling_force_reconnect("alias unregister retry send failed");
					return;
				}
				it->second.phase = PendingUnregisterPhase::RetryReadback;
				if (!request_alias_lookup(alias, AliasLookupPurpose::Unregister)) {
					remember_deferred_lookup(alias);
				}
				return;
			case PendingUnregisterPhase::RetryReadback:
				LOG("Alias \"%s\" unregister still resolves to this peer after retry\n", alias.c_str());
				it->second.phase = PendingUnregisterPhase::Failed;
				return;
			case PendingUnregisterPhase::Failed:
				return;
		}
	}

	void handle_acquire_resolution(const std::string& alias, PeerId peer)
	{
		if (humbleNetState.desiredAliases.find(alias) == humbleNetState.desiredAliases.end()) {
			return;
		}

		if (peer == humbleNetState.myPeerId && peer != 0) {
			humbleNetState.pendingAliasRegistrations.erase(alias);
			humbleNetState.sessionAliases.insert(alias);
			humbleNetState.confirmedAliases.insert(alias);
			schedule_alias_health_check();
			LOG("Alias \"%s\" registration confirmed for peer %u\n", alias.c_str(), peer);
			return;
		}

		humbleNetState.confirmedAliases.erase(alias);

		if (humbleNetState.sessionAliases.erase(alias) > 0) {
			humbleNetState.pendingAliasRegistrations.erase(alias);
			LOG("Alias \"%s\" no longer resolves to this peer (resolved to %u)\n",
				alias.c_str(), peer);
		}

		if (peer == 0) {
			humbleNetState.pendingAliasRegistrations.erase(alias);
			if (!send_alias_register_once(alias)) {
				humbleNetState.aliasWorkDeferred = true;
			}
			return;
		}

		humbleNetState.pendingAliasRegistrations.erase(alias);
		LOG("Alias \"%s\" registration rejected or unresolved (resolved to %u)\n", alias.c_str(), peer);
	}

	bool handle_health_resolution(const std::string& alias, PeerId peer)
	{
		if (peer == humbleNetState.myPeerId && peer != 0) {
			humbleNetState.confirmedAliases.insert(alias);
			schedule_alias_health_check();
			return true;
		}

		humbleNetState.confirmedAliases.erase(alias);
		if (peer == 0) {
			LOG("Alias \"%s\" health lookup found no owner; not registering\n", alias.c_str());
		} else {
			LOG("Alias \"%s\" health lookup resolved to peer %u; not taking over\n",
				alias.c_str(), peer);
		}
		schedule_alias_health_check();
		return false;
	}

}

ha_bool internal_alias_register( const char* name ) {
	if( !name || !name[0] ) {
		humblenet_set_error("No name or empty name provided");
		return 0;
	}

	std::string alias(name);
	if (!signaling_ready()) {
		return 0;
	}

	if (humbleNetState.pendingAliasUnregistrations.find(alias) !=
		humbleNetState.pendingAliasUnregistrations.end()) {
		humblenet_set_error("Alias unregister is still pending");
		return 0;
	}

	if (humbleNetState.desiredAliases.find(alias) != humbleNetState.desiredAliases.end() &&
		(humbleNetState.confirmedAliases.find(alias) != humbleNetState.confirmedAliases.end() ||
		humbleNetState.pendingAliasRegistrations.find(alias) != humbleNetState.pendingAliasRegistrations.end() ||
		humbleNetState.oldSessionAliasOwners.find(alias) != humbleNetState.oldSessionAliasOwners.end() ||
		humbleNetState.sessionAliases.find(alias) != humbleNetState.sessionAliases.end() ||
		humbleNetState.blockedAliasAcquisitions.find(alias) != humbleNetState.blockedAliasAcquisitions.end())) {
		return 1;
	}

	if (!humblenet::sendAliasRegister(humbleNetState.p2pConn.get(), alias)) {
		return 0;
	}

	bool wasDesired = humbleNetState.desiredAliases.find(alias) != humbleNetState.desiredAliases.end();
	humbleNetState.desiredAliases.insert(alias);
	if (!wasDesired) {
		bump_alias_revision(alias);
	}
	humbleNetState.oldSessionAliasOwners.erase(alias);
	humbleNetState.pendingAliasRegistrations.insert(alias);
	humbleNetState.confirmedAliases.erase(alias);
	if (!request_alias_lookup(alias, AliasLookupPurpose::Acquire)) {
		remember_deferred_lookup(alias);
	}
	return 1;
}

ha_bool internal_alias_unregister( const char* name ) {
	if (name && !name[0] ) {
		humblenet_set_error("Empty name provided");
		return 0;
	}

	if (!signaling_ready()) {
		return 0;
	}

	if (name) {
		std::string alias(name);
		if (!humblenet::sendAliasUnregister(humbleNetState.p2pConn.get(), alias)) {
			return 0;
		}

		bump_alias_revision(alias);
		humbleNetState.blockedAliasAcquisitions.erase(alias);
		humbleNetState.desiredAliases.erase(alias);
		humbleNetState.pendingAliasRegistrations.erase(alias);
		humbleNetState.confirmedAliases.erase(alias);
		humbleNetState.oldSessionAliasOwners.erase(alias);
		PendingUnregister pending;
		humbleNetState.pendingAliasUnregistrations[alias] = pending;
		if (!request_alias_lookup(alias, AliasLookupPurpose::Unregister))
			remember_deferred_lookup(alias);
		return 1;
	}

	std::vector<std::string> aliases = alias_snapshot();
	if (!humblenet::sendAliasUnregister(humbleNetState.p2pConn.get(), "")) {
		return 0;
	}

	humbleNetState.blockedAliasAcquisitions.clear();
	humbleNetState.desiredAliases.clear();
	humbleNetState.confirmedAliases.clear();
	humbleNetState.pendingAliasRegistrations.clear();
	humbleNetState.oldSessionAliasOwners.clear();

	for (const auto& alias : aliases) {
		bump_alias_revision(alias);
		PendingUnregister pending;
		humbleNetState.pendingAliasUnregistrations[alias] = pending;
		if (!request_alias_lookup(alias, AliasLookupPurpose::Unregister))
			remember_deferred_lookup(alias);
	}

	return 1;
}

PeerId internal_alias_lookup( const char* name ) {
	auto it = virtualPeerNames.find( name );
	if( ! virtualPeerNames.is_end(it) ) {
		return it->second;
	}

	// allocate a new VPeerId
	PeerId vpeer = (++nextVirtualPeer) | VIRTUAL_PEER;
	virtualPeerNames.insert( vpeer, name );

	return vpeer;
}

bool internal_alias_query( const char* query, const std::function<void(std::vector<std::pair<std::string,PeerId>>)>& callback ) {
	if (humbleNetState.pendingAliasQueryOut.find(query) != humbleNetState.pendingAliasQueryOut.end()) {
		return false;
	}
	if (!signaling_ready() || !humblenet::sendAliasQuery(humbleNetState.p2pConn.get(), query)) {
		return false;
	}
	humbleNetState.pendingAliasQueryOut.insert( std::make_pair(query, callback) );
	return true;
}

void internal_alias_query_result( const char* query, std::vector<std::pair<std::string,PeerId>> matches) {
	auto it = humbleNetState.pendingAliasQueryOut.find(query);
	if (it != humbleNetState.pendingAliasQueryOut.end()) {
		auto callback = it->second;
		humbleNetState.pendingAliasQueryOut.erase(it);
		HUMBLENET_UNGUARD();
		callback(std::move(matches));
	}
}

std::vector<std::function<void(std::vector<std::pair<std::string,PeerId>>)>> internal_alias_cancel_queries() {
	std::vector<std::function<void(std::vector<std::pair<std::string,PeerId>>)>> callbacks;
	callbacks.reserve(humbleNetState.pendingAliasQueryOut.size());
	for (const auto& it : humbleNetState.pendingAliasQueryOut) {
		callbacks.push_back(it.second);
	}
	humbleNetState.pendingAliasQueryOut.clear();
	return callbacks;
}

void internal_alias_on_signaling_reset() {
	humbleNetState.confirmedAliases.clear();
	humbleNetState.aliasLookups.clear();
	humbleNetState.aliasLookupTimeoutScheduled = false;
	humbleNetState.aliasLookupTimerDeadlineMs = 0;
	++humbleNetState.aliasLookupTimerGeneration;
	humbleNetState.aliasHealthCheckScheduled = false;
	++humbleNetState.aliasHealthCheckGeneration;
	humbleNetState.pendingAliasRegistrations.clear();
	humbleNetState.pendingAliasHealthChecks.clear();
}

void internal_alias_on_signaling_ready(bool freshSession, PeerId previousPeerId) {
	if (freshSession) {
		humbleNetState.blockedAliasAcquisitions.clear();
		if (previousPeerId != 0) {
			std::vector<std::string> oldAliases = alias_snapshot();
			for (const auto& alias : oldAliases) {
				humbleNetState.oldSessionAliasOwners[alias] = previousPeerId;
			}
		}
		humbleNetState.sessionAliases.clear();
		humbleNetState.confirmedAliases.clear();
		humbleNetState.pendingAliasRegistrations.clear();
		humbleNetState.aliasLookups.clear();
		humbleNetState.aliasLookupTimeoutScheduled = false;
		humbleNetState.aliasLookupTimerDeadlineMs = 0;
		++humbleNetState.aliasLookupTimerGeneration;
	}

	std::vector<std::string> aliases = alias_snapshot();
	for (const auto& alias : aliases) {
		reconcile_alias_work(alias);
	}
	schedule_alias_health_check();
}

void internal_alias_retry_deferred_work()
{
	if (!humbleNetState.aliasWorkDeferred || !signaling_ready()) {
		return;
	}

	humbleNetState.aliasWorkDeferred = false;
	std::vector<std::string> aliases = alias_snapshot();
	for (const auto& alias : aliases) {
		reconcile_alias_work(alias);
	}
}

void internal_alias_resolved_to( const std::string& alias, PeerId peer ) {
	Connection* connection = NULL;

	auto it = humbleNetState.pendingAliasConnectionsOut.find(alias);
	if (it == humbleNetState.pendingAliasConnectionsOut.end()) {
		LOG("Got resolve message for alias \"%s\" which we're not connecting to\n", alias.c_str());
		return;
	} else {
		connection = it->second;
		humbleNetState.pendingAliasConnectionsOut.erase(it);
	}

	assert(connection != NULL);

	if( peer == 0 ) {
		LOG("AliasError: unable to resolve \"%s\"\n", alias.c_str());
		
		humblenet_connection_set_closed( connection );
		return;
	}

	connection->otherPeer = peer;

	humbleNetState.pendingPeerConnectionsOut.emplace(peer, connection);

	if( is_peer_blacklisted(peer) ) {
		humblenet_set_error("peer blacklisted");
		LOG("humblenet_connect_peer: peer blacklisted %u\n", peer);

		humblenet_connection_set_closed(connection);
	} else {
		int ret = 0;
		{
			HUMBLENET_UNGUARD();
			connection->socket = internal_create_webrtc(humbleNetState.context);
			if( connection->socket ) {
				internal_set_data( connection->socket, connection);

				ret = internal_create_offer(connection->socket );
			}
		}

		if( !ret) {
			LOG("Unable to create offer, aborting connection to alias\n");
			humblenet_connection_set_closed( connection );
		}
	}

}

void internal_alias_handle_resolution(const std::string& alias, PeerId peer)
{
	auto it = humbleNetState.aliasLookups.find(alias);
	if (it == humbleNetState.aliasLookups.end()) {
		LOG("Got resolve message for alias \"%s\" without a pending lookup\n", alias.c_str());
		return;
	}

	AliasLookupInFlight lookup = it->second;
	humbleNetState.aliasLookups.erase(it);

	if (lookup.signalingGeneration != humbleNetState.reconnectGeneration ||
		lookup.intentRevision != humbleNetState.aliasIntentRevision[alias]) {
		reconcile_alias_work(alias);
		return;
	}
	if (lookup.deadlineMs <= now_ms()) {
		LOG("Alias lookup for \"%s\" timed out\n", alias.c_str());
		humblenet_signaling_force_reconnect("alias lookup timed out");
		return;
	}

	switch (lookup.purpose) {
		case AliasLookupPurpose::OldSessionCheck:
			handle_old_session_resolution(alias, peer);
			break;
		case AliasLookupPurpose::Unregister:
			handle_unregister_resolution(alias, peer);
			break;
		case AliasLookupPurpose::Acquire:
			handle_acquire_resolution(alias, peer);
			break;
		case AliasLookupPurpose::Connect: {
			if (humbleNetState.pendingAliasConnectionsOut.find(alias) !=
				humbleNetState.pendingAliasConnectionsOut.end()) {
				internal_alias_resolved_to(alias, peer);
			}
			if (humbleNetState.pendingAliasHealthChecks.find(alias) ==
				humbleNetState.pendingAliasHealthChecks.end() &&
				humbleNetState.pendingAliasRegistrations.find(alias) ==
				humbleNetState.pendingAliasRegistrations.end()) {
				return;
			}
			break;
		}
		case AliasLookupPurpose::Health:
			if (!handle_health_resolution(alias, peer)) {
				if (humbleNetState.pendingAliasConnectionsOut.find(alias) !=
					humbleNetState.pendingAliasConnectionsOut.end()) {
					request_alias_lookup(alias, AliasLookupPurpose::Connect);
				}
				return;
			}
			break;
	}

	reconcile_alias_work(alias);
}

ha_bool internal_alias_is_virtual_peer( PeerId peer ) {
	return (peer & VIRTUAL_PEER) == VIRTUAL_PEER;
}

PeerId internal_alias_get_virtual_peer( Connection* conn ) {
	auto it = virtualPeerConnections.find( conn );
	if( virtualPeerConnections.is_end(it) )
		return 0;
	else
		return it->second;
}

Connection* internal_alias_find_connection( PeerId peer ) {
	if( ! internal_alias_is_virtual_peer(peer) ) {
		return NULL;
	}

	// resolve the VPeer
	auto it = virtualPeerConnections.find( peer );
	if( virtualPeerConnections.is_end(it) ) {
		return NULL;
	} else if( humblenet_connection_status( it->second ) == HUMBLENET_CONNECTION_CLOSED ) {
		virtualPeerConnections.erase( it );
		return NULL;
	} else {
		return it->second;
	}
}

Connection* internal_alias_create_connection( PeerId peer ) {
	Connection* conn = internal_alias_find_connection( peer );
	if( conn == NULL ) {
		auto nit = virtualPeerNames.find( peer );
		if( virtualPeerNames.is_end( nit ) ) {
			// Invalid VPeerId !!!
			humblenet_set_error("Not a valid VPeerId");
			return NULL;
		}

		std::string name = nit->second;

		conn = new Connection(Outgoing);
		humbleNetState.pendingAliasConnectionsOut.emplace(name, conn);
		if (!request_alias_lookup(name, AliasLookupPurpose::Connect)) {
			humbleNetState.pendingAliasConnectionsOut.erase(name);
			delete conn;
			return NULL;
		}

		LOG("Establishing a connection to \"%s\"...\n", nit->second.c_str() );

		virtualPeerConnections.insert( peer, conn );
	}

	return conn;
}

void internal_alias_remove_connection( Connection* conn ) {
	virtualPeerConnections.erase( conn );
}
