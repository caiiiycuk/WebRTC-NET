#include "humblenet_portal_internal.h"
#include "humblenet_p2p_internal.h"
#include "humblenet_alias.h"

#if defined(EMSCRIPTEN)
	#include <emscripten/emscripten.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
	#include <sys/utsname.h>
#endif

#if defined(WIN32)
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <unordered_set>


static ha_bool p2pSignalProcess(const humblenet::HumblePeer::Message *msg, void *user_data);

namespace {
	static const uint32_t kReconnectBaseDelayMs = 500;
	static const uint32_t kReconnectMaxDelayMs = 30000;
	static const size_t kMaxSignalingBytes = 1024 * 1024;
	static const size_t kMaxSignalingMessages = 1024;

	uint32_t computeReconnectDelayMs(uint32_t attempt)
	{
		uint32_t cappedAttempt = std::min<uint32_t>(attempt, 6);
		uint32_t exponentialDelay = kReconnectBaseDelayMs << cappedAttempt;
		uint32_t cappedDelay = std::min(kReconnectMaxDelayMs, exponentialDelay);
		uint64_t seed = std::chrono::steady_clock::now().time_since_epoch().count();
		seed ^= static_cast<uint64_t>(attempt + 1) * 0x9E3779B97F4A7C15ULL;
		uint32_t jitterRange = std::max<uint32_t>(1, cappedDelay / 4);
		uint32_t jitter = static_cast<uint32_t>(seed % (jitterRange + 1));
		return std::min(kReconnectMaxDelayMs, cappedDelay + jitter);
	}

	void replay_pending_alias_queries()
	{
		if (!humbleNetState.p2pConn) {
			return;
		}

		std::vector<std::string> queries;
		queries.reserve(humbleNetState.pendingAliasQueryOut.size());
		for (const auto& it : humbleNetState.pendingAliasQueryOut) {
			queries.push_back(it.first);
		}

		for (const auto& query : queries) {
			if (!humbleNetState.p2pConn) {
				return;
			}
			if (!humblenet::sendAliasQuery(humbleNetState.p2pConn.get(), query)) {
				LOG("Failed to replay alias query \"%s\"\n", query.c_str());
				auto it = humbleNetState.pendingAliasQueryOut.find(query);
				if (it != humbleNetState.pendingAliasQueryOut.end()) {
					auto callback = it->second;
					humbleNetState.pendingAliasQueryOut.erase(it);
					HUMBLENET_UNGUARD();
					callback({});
				}
			}
		}
	}

	void reconnect_signaling_timer(void* data);
	void request_signaling_writable_timer(void* data);

	void schedule_signaling_reconnect(const char* reason)
	{
		if (!humbleNetState.signalingReconnectEnabled) {
			return;
		}
		if (humbleNetState.reconnectScheduled) {
			return;
		}

		uint32_t delayMs = computeReconnectDelayMs(humbleNetState.reconnectAttempt);
		++humbleNetState.reconnectAttempt;
		humbleNetState.reconnectScheduled = true;

		uintptr_t generation = static_cast<uintptr_t>(++humbleNetState.reconnectGeneration);
		LOG("Scheduling signaling reconnect in %u ms (%s)\n", delayMs, reason);
		humblenet_timer(reconnect_signaling_timer, delayMs, reinterpret_cast<void*>(generation));
	}

	void reconnect_signaling_timer(void* data)
	{
		HUMBLENET_GUARD();

		uintptr_t generation = reinterpret_cast<uintptr_t>(data);
		if (!humbleNetState.signalingReconnectEnabled ||
			generation != static_cast<uintptr_t>(humbleNetState.reconnectGeneration)) {
			return;
		}

		humbleNetState.reconnectScheduled = false;
		humblenet_signaling_connect();
	}

	bool queue_signaling_message(humblenet::P2PSignalConnection* conn, const uint8_t* buff, size_t length)
	{
		if (length > kMaxSignalingBytes || conn->queuedBytes > kMaxSignalingBytes - length ||
			conn->sendQueue.size() >= kMaxSignalingMessages) {
			return false;
		}
		conn->sendQueue.emplace_back(buff, buff + length);
		conn->queuedBytes += length;
		return true;
	}

	void schedule_signaling_writable(humblenet::P2PSignalConnection* conn)
	{
#ifndef EMSCRIPTEN
		if (conn->writableWakePending) {
			return;
		}
		conn->writableWakePending = true;
		uintptr_t generation = static_cast<uintptr_t>(humbleNetState.reconnectGeneration);
		humblenet_timer(request_signaling_writable_timer, 0, reinterpret_cast<void*>(generation));
#else
		internal_request_writable(conn->wsi);
#endif
	}

	void request_signaling_writable_timer(void* data)
	{
		HUMBLENET_GUARD();

		uintptr_t generation = reinterpret_cast<uintptr_t>(data);
		if (!humbleNetState.p2pConn ||
			generation != static_cast<uintptr_t>(humbleNetState.reconnectGeneration)) {
			return;
		}

		humblenet::P2PSignalConnection* conn = humbleNetState.p2pConn.get();
		conn->writableWakePending = false;
		if (conn->wsi != NULL && !conn->sendQueue.empty()) {
			internal_request_writable(conn->wsi);
		}
	}

	void reset_signaling_connection()
	{
		if (humbleNetState.myPeerId != 0) {
			humbleNetState.reconnectPeerId = humbleNetState.myPeerId;
		}
		humbleNetState.myPeerId = 0;
		internal_alias_on_signaling_reset();

		std::unordered_set<Connection*> inFlightConnections;
		for (const auto& it : humbleNetState.pendingPeerConnectionsOut) {
			inFlightConnections.insert(it.second);
		}
		for (const auto& it : humbleNetState.pendingPeerConnectionsIn) {
			inFlightConnections.insert(it.second);
		}
		for (const auto& it : humbleNetState.pendingAliasConnectionsOut) {
			inFlightConnections.insert(it.second);
		}
		for (Connection* conn : inFlightConnections) {
			if (conn != NULL) {
				humblenet_connection_set_closed(conn);
			}
		}

		if (humbleNetState.p2pConn) {
			humbleNetState.p2pConn.reset();
		}
	}
}

ha_bool humblenet_signaling_connect() {
	HUMBLENET_GUARD();

	using namespace humblenet;

	if (humbleNetState.p2pConn) {
		humbleNetState.p2pConn->disconnect();
		humbleNetState.p2pConn.reset();
	}
	if (humbleNetState.myPeerId != 0) {
		humbleNetState.reconnectPeerId = humbleNetState.myPeerId;
	}
	humbleNetState.myPeerId = 0;

	LOG("connecting to signaling server \"%s\" with gameToken \"%s\" \n", humbleNetState.signalingServerAddr.c_str(), humbleNetState.gameToken.c_str());
	humbleNetState.reconnectScheduled = false;

	humbleNetState.p2pConn.reset(new P2PSignalConnection);
	humbleNetState.p2pConn->wsi = internal_connect_websocket(humbleNetState.signalingServerAddr.c_str(), "humblepeer");

	if (humbleNetState.p2pConn->wsi == NULL) {
		humbleNetState.p2pConn.reset();
		schedule_signaling_reconnect("connect failed");
		// TODO: can we get more specific reason ?
		humblenet_set_error("WebSocket connection to signaling server failed");
		return false;
	}

	LOG("new wsi: %p\n", humbleNetState.p2pConn->wsi);

	internal_poll_io();
	return true;
}

void humblenet_signaling_force_reconnect(const char* reason) {
	if (humbleNetState.p2pConn) {
		humbleNetState.p2pConn->drop();
	}
	reset_signaling_connection();
	schedule_signaling_reconnect(reason);
}

namespace humblenet {
	ha_bool sendP2PMessage(P2PSignalConnection *conn, const uint8_t *buff, size_t length)
	{
		if( conn == NULL ) {
			// this equates to the state where we never established / got disconnected
			// from the peer server.

			return false;
		}

		assert(conn != NULL);
		if (conn->wsi == NULL) {
			// this really shouldn't happen but apparently it can
			// if the game hasn't properly opened a signaling connection
			return false;
		}

#ifndef EMSCRIPTEN
		if (!queue_signaling_message(conn, buff, length)) {
			return false;
		}
		schedule_signaling_writable(conn);
		return true;
#else
		{
			HUMBLENET_UNGUARD();

			int ret = internal_write_socket(conn->wsi, (const void*)buff, length );

			return ret == static_cast<int>(length);
		}
#endif
	}

	// called for incoming connections to indicate the connection process is completed.
	int on_accept (internal_socket_t* s, void* user_data) {
		// we dont accept incoming connections
		return -1;
	}

	// called for outgoing connections to indicate the connection process is completed.
	int on_connect (internal_socket_t* s, void* user_data) {
		HUMBLENET_GUARD();

		assert(s != NULL);
		if (!humbleNetState.p2pConn) {
			// there is no signaling connection in humbleNetState
			// this one must be obsolete, close it
			return -1;
		}

		// temporary copy, doesn't transfer ownership
		P2PSignalConnection *conn = humbleNetState.p2pConn.get();
		if (conn->wsi != s) {
			// this connection is not the one currently active in humbleNetState.
			// this one must be obsolete, close it
			return -1;
		}
		// platform info
		std::string pinfo;
#if defined(__APPLE__) || defined(__linux__)
		struct utsname buf;
		memset(&buf, 0, sizeof(struct utsname));
		int retval = uname(&buf);
		if (retval == 0)
		{
			pinfo.append("Sysname: ");
			pinfo.append(buf.sysname);
			pinfo.append(", Release: ");
			pinfo.append(buf.release);
			pinfo.append(", Version: ");
			pinfo.append(buf.version);
			pinfo.append(", Machine: ");
			pinfo.append(buf.machine);
		}
		else
		{
			LOG("Failed getting system info with uname\n");
		}
#elif defined(_WIN32)
		OSVERSIONINFO osvi;
		ZeroMemory(&osvi, sizeof(OSVERSIONINFO));
		osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
		GetVersionEx(&osvi);
		pinfo.append("Windows " + std::to_string(osvi.dwMajorVersion) + "." + std::to_string(osvi.dwMinorVersion) + ", build " + std::to_string(osvi.dwBuildNumber));

		SYSTEM_INFO si;
		ZeroMemory(&si, sizeof(SYSTEM_INFO));
		GetSystemInfo(&si);
		switch (si.wProcessorArchitecture)
		{
			case PROCESSOR_ARCHITECTURE_INTEL:
				pinfo.append(", Intel x86");
				break;
			case PROCESSOR_ARCHITECTURE_IA64:
				pinfo.append(", Intel Itanium");
				break;
			case PROCESSOR_ARCHITECTURE_AMD64:
				pinfo.append(", AMD or Intel x64");
				break;
			default:
				pinfo.append(", non-desktop architecture");
				break;
		}
#elif defined(EMSCRIPTEN)
		char buff[512];
		int len = EM_ASM_INT({
			var buff = new Uint8Array(Module.HEAPU8.buffer, $0, $1);
			return stringToUTF8Array(navigator.userAgent, buff, 0, $1);
		}, buff, sizeof(buff));
		pinfo = buff;
#endif

		uint8_t flags = 0;
		if (humbleNetState.webRTCSupported) {
			// No trickle ICE on native
			flags = (0x1 | 0x2);
		}
		std::map<std::string, std::string> attributes;
		attributes.emplace("platform", pinfo);
		ha_bool helloSuccess = sendHelloServer(conn, flags, humbleNetState.gameToken, humbleNetState.gameSecret, humbleNetState.authToken, humbleNetState.reconnectToken, attributes);
		if (!helloSuccess) {
			// something went wrong, close the connection
			// don't humblenet_set_error, sendHelloServer should have done that
			reset_signaling_connection();
			schedule_signaling_reconnect("hello failed");
			return -1;
		}
		return 0;
	}

	// called each time data is received.
	int on_data( internal_socket_t* s, const void* data, int len, void* user_data ) {
		HUMBLENET_GUARD();

		assert(s != NULL);

		if (!humbleNetState.p2pConn) {
			// there is no signaling connection in humbleNetState
			// this one must be obsolete, close it
			return -1;
		}

		// temporary copy, doesn't transfer ownership
		P2PSignalConnection *conn = humbleNetState.p2pConn.get();
		if (conn->wsi != s) {
			// this connection is not the one currently active in humbleNetState.
			// this one must be obsolete, close it
			return -1;
		}

		//        LOG("Data: %d -> %s\n", len, std::string((const char*)data,len).c_str());

		if (len < 0 || static_cast<size_t>(len) > kMaxSignalingBytes ||
			conn->recvBuf.size() > kMaxSignalingBytes - static_cast<size_t>(len)) {
			conn->recvBuf.clear();
			reset_signaling_connection();
			schedule_signaling_reconnect("message too large");
			return -1;
		}

		const uint8_t* inBuf = reinterpret_cast<const uint8_t*>(data);
		conn->recvBuf.insert(conn->recvBuf.end(), inBuf, inBuf + len);
		if (!internal_websocket_message_complete(s)) {
			return 0;
		}
		std::vector<uint8_t> message;
		message.swap(conn->recvBuf);
		ha_bool retval = parseMessage(message, p2pSignalProcess, NULL);
		if (!retval) {
			// error while parsing a message, close the connection
			reset_signaling_connection();
			schedule_signaling_reconnect("message parse failed");
			return -1;
		}
		return 0;
	}

	// called to indicate the connection is writable.
	int on_writable (internal_socket_t* s, void* user_data) {
		HUMBLENET_GUARD();

		assert(s != NULL);
		if (!humbleNetState.p2pConn) {
			// there is no signaling connection in humbleNetState
			// this one must be obsolete, close it
			return -1;
		}

		// temporary copy, doesn't transfer ownership
		P2PSignalConnection *conn = humbleNetState.p2pConn.get();
		if (conn->wsi != s) {
			// this connection is not the one currently active in humbleNetState.
			// this one must be obsolete, close it
			return -1;
		}

		conn->writableWakePending = false;
		if (conn->sendQueue.empty()) {
			// no data in sendQueue
			return 0;
		}

		std::vector<uint8_t>& message = conn->sendQueue.front();
		int retval = internal_write_socket(conn->wsi, message.data(), static_cast<int>(message.size()));
		if (retval != static_cast<int>(message.size())) {
			// error while sending, close the connection
			reset_signaling_connection();
			schedule_signaling_reconnect("write failed");
			return -1;
		}

		// successful write
		conn->queuedBytes -= message.size();
		conn->sendQueue.pop_front();
		if (!conn->sendQueue.empty()) {
			internal_request_writable(conn->wsi);
		}

		internal_alias_retry_deferred_work();
		return 0;
	}

	// called when the conenction is terminated (from either side)
	int on_disconnect( internal_socket_t* s, void* user_data ) {
		HUMBLENET_GUARD();

		if( humbleNetState.p2pConn ) {
			if( s == humbleNetState.p2pConn->wsi ) {
				reset_signaling_connection();
				schedule_signaling_reconnect("socket disconnected");
				return 0;
			}
		}

		return 0;
	}


	bool register_protocol(internal_context_t* context) {
		internal_callbacks_t callbacks;

		memset(&callbacks, 0, sizeof(callbacks));

		callbacks.on_accept = on_accept;
		callbacks.on_connect = on_connect;
		callbacks.on_data = on_data;
		callbacks.on_disconnect = on_disconnect;
		callbacks.on_writable = on_writable;
		callbacks.on_destroy = on_disconnect; // on connection failure to establish we only get a destroy, not a disconnect.

		bool registered = internal_register_protocol(context, "humblepeer", &callbacks);
		if (registered)
			internal_publish_context(context);
		return registered;
	}
}

static ha_bool p2pSignalProcess(const humblenet::HumblePeer::Message *msg, void *user_data)
{
	using namespace humblenet;

	auto msgType = msg->message_type();
    if (portal_enabled()) portal_notify(nullptr);

	switch (msgType) {
		case HumblePeer::MessageType::P2POffer:
		{
			auto p2p = reinterpret_cast<const HumblePeer::P2POffer*>(msg->message());
			PeerId peer = static_cast<PeerId>(p2p->peerId());

			if (humbleNetState.pendingPeerConnectionsIn.find(peer) != humbleNetState.pendingPeerConnectionsIn.end()) {
				// error, already a pending incoming connection to this peer
				LOG("p2pSignalProcess: already a pending connection to peer %u\n", peer);
				return true;
			}

			bool emulated = ( p2p->flags() & 0x1 );

			if (emulated || !humbleNetState.webRTCSupported) {
				// webrtc connections not supported
				assert(humbleNetState.p2pConn);
				sendPeerRefused(humbleNetState.p2pConn.get(), peer);
                if (portal_enabled()) break;
			}

			Connection *connection = portal_enabled() ? portal_offer(peer) : new Connection(Incoming);
            if (!connection) { sendPeerRefused(humbleNetState.p2pConn.get(), peer); break; }
			connection->status = HUMBLENET_CONNECTION_CONNECTING;
			connection->otherPeer = peer;
			{
				HUMBLENET_UNGUARD();
				connection->socket = internal_create_webrtc(humbleNetState.context);
			}
            if (!connection->socket) {
                if (portal_owns(connection)) portal_bound(connection);
                humblenet_connection_close(connection); break;
            }
			internal_set_data( connection->socket, connection );
            if (portal_owns(connection)) {
                portal_bound(connection);
                if (connection->status == HUMBLENET_CONNECTION_CLOSED) break;
            }

			humbleNetState.pendingPeerConnectionsIn.insert(std::make_pair(peer, connection));

			auto offer = p2p->offer();

			LOG("RTC negotiation received from peer %u\n", peer);
			int ret;
			{
				HUMBLENET_UNGUARD();
				ret = internal_set_offer( connection->socket, offer->c_str() );
			}
			if( ! ret )
			{
				humblenet_connection_close( connection );

				sendPeerRefused(humbleNetState.p2pConn.get(), peer);
			}
		}
			break;

		case HumblePeer::MessageType::P2PAnswer:
		{
			auto p2p = reinterpret_cast<const HumblePeer::P2PAnswer*>(msg->message());
			PeerId peer = static_cast<PeerId>(p2p->peerId());

			auto it = humbleNetState.pendingPeerConnectionsOut.find(peer);
			if (it == humbleNetState.pendingPeerConnectionsOut.end()) {
				LOG("P2PResponse for connection we didn't initiate: %u\n", peer);
				return true;
			}

			Connection *conn = it->second;
			assert(conn != NULL);
			assert(conn->otherPeer == peer);

			assert(conn->socket != NULL);
			// TODO: deal with _CLOSED
			assert(conn->status == HUMBLENET_CONNECTION_CONNECTING);

			auto offer = p2p->offer();

			LOG("RTC negotiation received from peer %u\n", peer);
			int ret;
			{
				HUMBLENET_UNGUARD();
				ret = internal_set_answer( conn->socket, offer->c_str() );
			}
			if( !ret ) {
				humblenet_connection_set_closed( conn );
			}
		}
			break;

		case HumblePeer::MessageType::HelloClient:
		{
			auto hello = reinterpret_cast<const HumblePeer::HelloClient*>(msg->message());
			PeerId peer = static_cast<PeerId>(hello->peerId());
			PeerId previousPeerId = humbleNetState.reconnectPeerId;
			bool hadReconnectToken = !humbleNetState.reconnectToken.empty();

			humbleNetState.reconnectToken = hello->reconnectToken() ? hello->reconnectToken()->str() : "";
			humbleNetState.myPeerId = peer;
			humbleNetState.reconnectPeerId = 0;
			humbleNetState.reconnectAttempt = 0;
			humbleNetState.reconnectScheduled = false;

			if (previousPeerId != 0 && previousPeerId == peer && hadReconnectToken) {
				LOG("Signaling resume accepted for peer %u\n", peer);
				internal_alias_on_signaling_ready(false, previousPeerId);
			} else if (previousPeerId != 0 && previousPeerId != peer) {
				LOG("Signaling resume rejected, assigned fresh peer id %u (previously %u)\n", peer, previousPeerId);
				internal_alias_on_signaling_ready(true, previousPeerId);
			} else {
				LOG("My peer id is %u\n", peer);
				internal_alias_on_signaling_ready(false, 0);
			}

			replay_pending_alias_queries();
		}
			break;

		case HumblePeer::MessageType::ICECandidate:
		{
			auto iceCandidate = reinterpret_cast<const HumblePeer::ICECandidate*>(msg->message());
			PeerId peer = static_cast<PeerId>(iceCandidate->peerId());

			auto it = humbleNetState.pendingPeerConnectionsIn.find( peer );

			if( it == humbleNetState.pendingPeerConnectionsIn.end() )
			{
				it = humbleNetState.pendingPeerConnectionsOut.find( peer );
				if( it == humbleNetState.pendingPeerConnectionsOut.end() )
				{
					// no connection waiting on a peer.
					return true;
				}
			}

			if( it->second->socket && it->second->status == HUMBLENET_CONNECTION_CONNECTING ) {
				auto offer = iceCandidate->offer();
				LOG("RTC candidate received from peer %u\n", it->second->otherPeer);

				{
					HUMBLENET_UNGUARD();

					internal_add_ice_candidate( it->second->socket, offer->c_str() );
				}
			}
		}
			break;

		case HumblePeer::MessageType::P2PReject:
		{
			auto reject = reinterpret_cast<const HumblePeer::P2PReject*>(msg->message());
			PeerId peer = static_cast<PeerId>(reject->peerId());

			auto it = humbleNetState.pendingPeerConnectionsOut.find(peer);
			if (it == humbleNetState.pendingPeerConnectionsOut.end()) {
				switch (reject->reason()) {
					case HumblePeer::P2PRejectReason::NotFound:
						LOG("Peer %u does not exist\n", peer);
						break;
					case HumblePeer::P2PRejectReason::PeerRefused:
						LOG("Peer %u rejected our connection\n", peer);
						break;
				}
				return true;
			}

			Connection *conn = it->second;
			assert(conn != NULL);

			blacklist_peer(peer);
			humblenet_connection_set_closed(conn);
		}
			break;

		case HumblePeer::MessageType::P2PConnected:
		{
			auto connect = reinterpret_cast<const HumblePeer::P2PConnected*>(msg->message());
			
			LOG("Established connection to peer %u", connect->peerId());
		}
			break;
			
		case HumblePeer::MessageType::P2PDisconnect:
		{
			auto disconnect = reinterpret_cast<const HumblePeer::P2PDisconnect*>(msg->message());
			
			LOG("Disconnecting peer %u", disconnect->peerId());
		}
			break;
			
		case HumblePeer::MessageType::AliasResolved:
		{
			auto resolved = reinterpret_cast<const HumblePeer::AliasResolved*>(msg->message());
			std::string alias = resolved->alias()->c_str();
			PeerId peer = resolved->peerId();
			internal_alias_handle_resolution(alias, peer);
		}
			break;

		case HumblePeer::MessageType::AliasQueryResult: {
			auto aliasQuery = reinterpret_cast<const HumblePeer::AliasQueryResult*>(msg->message());
			auto query = aliasQuery->query();
			std::vector<std::pair<std::string, PeerId>> matched;

			for (const auto& it: *aliasQuery->records()) {
				matched.emplace_back(it->alias()->c_str(), it->peerId());
			}

			internal_alias_query_result(query->c_str(), std::move(matched));
		}
			break;
			
		default:
			LOG("p2pSignalProcess unhandled %s\n", HumblePeer::EnumNameMessageType(msgType));
			break;
	}
	
	return true;
}
