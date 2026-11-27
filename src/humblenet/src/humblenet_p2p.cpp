#include "humblenet_portal_internal.h"
#include "humblenet_p2p.h"
#include "humblenet_p2p_internal.h"
#include "humblenet_datagram.h"
#include "humblenet_alias.h"

#include <string>
#include <map>

#define P2P_INIT_GUARD( ... )    INIT_GUARD( "humblenet_p2p_init has not been called", initialized, __VA_ARGS__ )

// These are the connections managed by p2p...
std::map<PeerId, Connection*> p2pconnections;

static bool initialized = false;

/*
 * Is the peer-to-peer network initialized.
 */
ha_bool HUMBLENET_CALL humblenet_p2p_is_initialized() {
	return initialized;
}

/*
 * Initialize the peer-to-peer library.
 */
ha_bool HUMBLENET_CALL humblenet_p2p_init(const char* server, const char* game_token, const char* game_secret, const char* auth_token) {
	if( initialized ) {
		return 1;
	}

	LOG("humblenet_p2p_init\n");

	if (!server || !game_token || !game_secret) {
		humblenet_set_error("Must specify server, game_token, and game_secret");
		return 0;
	}
	humbleNetState.signalingServerAddr = server;
	humbleNetState.gameToken = game_token;
	humbleNetState.gameSecret = game_secret;
	humbleNetState.reconnectToken = "";
	humbleNetState.myPeerId = 0;
	humbleNetState.reconnectPeerId = 0;
	humbleNetState.reconnectAttempt = 0;
	humbleNetState.reconnectScheduled = false;
	++humbleNetState.reconnectGeneration;
	humbleNetState.signalingReconnectEnabled = true;
	humbleNetState.pendingAliasRegistrations.clear();
	humbleNetState.pendingAliasUnregistrations.clear();
	humbleNetState.oldSessionAliasOwners.clear();
	humbleNetState.blockedAliasAcquisitions.clear();
	humbleNetState.aliasWorkDeferred = false;
	humbleNetState.desiredAliases.clear();
	humbleNetState.sessionAliases.clear();
	humbleNetState.confirmedAliases.clear();
	humbleNetState.aliasIntentRevision.clear();
	humbleNetState.aliasLookups.clear();
	humbleNetState.aliasLookupTimeoutScheduled = false;
	humbleNetState.aliasLookupTimerDeadlineMs = 0;
	++humbleNetState.aliasLookupTimerGeneration;
	humbleNetState.aliasHealthCheckScheduled = false;
	++humbleNetState.aliasHealthCheckGeneration;
	humbleNetState.pendingAliasHealthChecks.clear();
	humbleNetState.pendingAliasQueryOut.clear();

	if( auth_token ) {
		humbleNetState.authToken = auth_token;
	} else {
		humbleNetState.authToken = "";
	}

	if (!internal_p2p_register_protocol()) {
		humblenet_set_error("Unable to initialize native networking context");
		return 0;
	}

	initialized = true;
 if (!humblenet_signaling_connect()) {
  humblenet_p2p_shutdown();
  return 0;
 }

	return 1;
}

/*
 * Shut down the networking library
 */
void humblenet_p2p_shutdown() {
	internal_context_t* context = NULL;
	std::vector<std::function<void(std::vector<std::pair<std::string,PeerId>>)>> aliasQueryCallbacks;

	{
		HUMBLENET_GUARD();

		if (!initialized) {
            // Portal enable owns an eventfd before P2P initialization.
            portal_shutdown();
			return;
		}

		LOG("humblenet_p2p_shutdown\n");

		initialized = false;
		humbleNetState.signalingReconnectEnabled = false;
		humbleNetState.reconnectScheduled = false;
		++humbleNetState.reconnectGeneration;
		humbleNetState.aliasHealthCheckScheduled = false;
		++humbleNetState.aliasHealthCheckGeneration;
		humbleNetState.pendingAliasHealthChecks.clear();
		context = humbleNetState.context;
		humbleNetState.context = NULL;
	}

	internal_deinit(context);
    portal_shutdown();

	humbleNetState.myPeerId = 0;
	humbleNetState.reconnectPeerId = 0;
	humbleNetState.reconnectToken.clear();
	humbleNetState.pendingAliasRegistrations.clear();
	humbleNetState.pendingAliasUnregistrations.clear();
	humbleNetState.oldSessionAliasOwners.clear();
	humbleNetState.blockedAliasAcquisitions.clear();
	humbleNetState.aliasWorkDeferred = false;
	humbleNetState.desiredAliases.clear();
	humbleNetState.sessionAliases.clear();
	humbleNetState.confirmedAliases.clear();
	humbleNetState.aliasIntentRevision.clear();
	humbleNetState.aliasLookups.clear();
	humbleNetState.aliasLookupTimeoutScheduled = false;
	humbleNetState.aliasLookupTimerDeadlineMs = 0;
	++humbleNetState.aliasLookupTimerGeneration;
	humbleNetState.p2pConn.reset();
	aliasQueryCallbacks = internal_alias_cancel_queries();

	for (const auto& callback : aliasQueryCallbacks) {
		callback({});
	}
}

/*
 * Get the current peer ID of this client
 * returns 0 if not yet connected
 */
PeerId HUMBLENET_CALL humblenet_p2p_get_my_peer_id() {
	P2P_INIT_GUARD( 0 );

	return humbleNetState.myPeerId;
}

/*
 * Register an alias for this peer so it can be found by name
 */
ha_bool HUMBLENET_CALL humblenet_p2p_register_alias(const char* name) {
	P2P_INIT_GUARD( false );

	HUMBLENET_GUARD();

	return internal_alias_register( name );
}

/*
 * Unregister an alias of this peer.
 */
ha_bool HUMBLENET_CALL humblenet_p2p_unregister_alias(const char* name) {
	P2P_INIT_GUARD( false );

	HUMBLENET_GUARD();

	return internal_alias_unregister( name );
}

/*
 * Find the PeerId of a named peer (registered on the server)
 */
PeerId HUMBLENET_CALL humblenet_p2p_virtual_peer_for_alias(const char* name) {
	P2P_INIT_GUARD( 0 );

	HUMBLENET_GUARD();

	return internal_alias_lookup( name );
}

HUMBLENET_API ha_bool HUMBLENET_CALL humblenet_p2p_alias_query(const char* query,
	const std::function<void(std::vector<std::pair<std::string,PeerId>>)>& callback) {
	P2P_INIT_GUARD( 0 );
	HUMBLENET_GUARD();
	return internal_alias_query( query, callback );
}

/*
 * Send a message to a peer.
 */
int HUMBLENET_CALL humblenet_p2p_sendto(const void* message, uint32_t length, PeerId topeer, SendMode sendmode, uint8_t channel) {
    if (portal_enabled()) { humblenet_set_error("Use lifetime-scoped portal_send"); return -1; }
	P2P_INIT_GUARD( -1 );

	HUMBLENET_GUARD();

	Connection* conn = NULL;

	auto cit = p2pconnections.find( topeer );
	if( cit != p2pconnections.end() ) {
		// we have an active connection
		conn = cit->second;
	} else if( internal_alias_is_virtual_peer( topeer ) ) {
		// lookup/create a connection to the virutal peer.
		conn = internal_alias_find_connection( topeer );
		if( conn == NULL )
			conn = internal_alias_create_connection( topeer );
	} else {
		// create a new connection to the peer
		conn = humblenet_connect_peer( topeer );
	}

	if( conn == NULL ) {
		humblenet_set_error("Unable to get a connection for peer");
		return -1;
	} else if( cit == p2pconnections.end() ) {
		p2pconnections.insert( std::make_pair( topeer, conn ) );
		LOG("Connection to peer opened: %u\n", topeer );
	}

	if (humblenet_connection_status(conn) == HUMBLENET_CONNECTION_CLOSED) {
  humblenet_set_error("Connection to peer was closed");
  return -1;
 }
 int flags = 0;

	if( sendmode & SEND_RELIABLE_BUFFERED )
		flags = HUMBLENET_MSG_BUFFERED;

	int ret = humblenet_datagram_send( message, length, flags, conn, channel );
	TRACE("Sent packet for channel %d to %u(%u): %d\n", channel, topeer, conn->otherPeer, ret );
	if( ret < 0 ) {
		LOG("Peer  %p(%u) write failed: %s\n", conn, topeer, humblenet_get_error() );
		if( humblenet_connection_status( conn ) == HUMBLENET_CONNECTION_CLOSED ) {
			LOG("Peer connection was closed\n");

			p2pconnections.erase( conn->otherPeer );
			p2pconnections.erase( topeer );
			humblenet_connection_close(conn);

			humblenet_set_error("Connection to peer was closed");
			return -1;
		}
	}
	return ret;
}

/*
 * See if there is a message available on the specified channel
 */
ha_bool HUMBLENET_CALL humblenet_p2p_peek(uint32_t* length, uint8_t channel) {
	P2P_INIT_GUARD( false );

	HUMBLENET_GUARD();

	Connection* from;
	int ret = humblenet_datagram_recv( NULL, 0, 1/*MSG_PEEK*/, &from, channel );
	if( ret > 0 )
		*length = ret;
	else
		*length = 0;

	return *length > 0;
}

/*
 * Receive a message sent from a peer
 */
static int p2p_recvfrom(void* buffer, uint32_t length, PeerId* frompeer, uint8_t channel, uint64_t* lifetime) {
    if (portal_enabled()) { humblenet_set_error("Use timestamped portal_recv"); return 0; }
	if (lifetime) *lifetime = 0;
	P2P_INIT_GUARD( 0 );

	HUMBLENET_GUARD();

	Connection* conn = NULL;
	int ret = humblenet_datagram_recv( buffer, length, 0, &conn, channel );
	if( !conn || !ret ) {
		*frompeer = 0;
	} else {
		if (lifetime) *lifetime = conn->lifetime;
		PeerId vpeer = internal_alias_get_virtual_peer( conn );
		PeerId peer = conn->otherPeer;

		if( vpeer )
			*frompeer = vpeer;
		else
			*frompeer = peer;

		if( ret > 0 ) {
			TRACE("Got packet for channel %d from %u(%u)\n", channel, *frompeer, peer );
			if( p2pconnections.find( *frompeer ) == p2pconnections.end() ) {
				LOG("Tracking inbound connection to peer %u(%u)\n", *frompeer, peer );
				p2pconnections.insert( std::make_pair( *frompeer, conn ) );
			}
		} else {
			// A queued close belongs to this object, not a replacement with the same PeerId.
			for (auto it = p2pconnections.begin(); it != p2pconnections.end(); ) {
				if (it->second == conn) it = p2pconnections.erase(it);
				else ++it;
			}
			humblenet_connection_close(conn);
			LOG("closing connection to peer: %u(%u)\n",*frompeer,peer);
		}
	}
	return ret;
}

int HUMBLENET_CALL humblenet_p2p_recvfrom(void* buffer, uint32_t length, PeerId* peer, uint8_t channel) {
 return p2p_recvfrom(buffer, length, peer, channel, nullptr);
}
int HUMBLENET_CALL humblenet_dz_recvfrom(void* buffer, uint32_t length, PeerId* peer, uint8_t channel, uint64_t* lifetime) {
 return p2p_recvfrom(buffer, length, peer, channel, lifetime);
}
int HUMBLENET_CALL humblenet_dz_connection_state(PeerId peer, uint64_t* lifetime) {
 HUMBLENET_GUARD();
 *lifetime = 0;
 auto it = p2pconnections.find(peer);
 if (it == p2pconnections.end()) return 0;
 *lifetime = it->second->lifetime;
 return it->second->status;
}
ha_bool HUMBLENET_CALL humblenet_dz_disconnect(PeerId peer, uint64_t lifetime) {
 P2P_INIT_GUARD(false);
 HUMBLENET_GUARD();
 auto it = p2pconnections.find(peer);
 if (!lifetime || it == p2pconnections.end() || it->second->lifetime != lifetime) return false;
 humblenet_connection_set_closed(it->second);
 return true;
}

// Observe the native provider's existing session and alias fences; counters stay uint64.
int HUMBLENET_CALL humblenet_dz_signaling_state(uint64_t* generation) {
 HUMBLENET_GUARD();
 *generation = humbleNetState.reconnectGeneration;
 if (!initialized) return 0;
 if (humbleNetState.myPeerId && humbleNetState.p2pConn && humbleNetState.p2pConn->wsi) return 2;
 return humbleNetState.reconnectScheduled ? 3 : 1;
}
int HUMBLENET_CALL humblenet_dz_alias_state(const char* alias, PeerId* peer, uint64_t* generation) {
 HUMBLENET_GUARD();
 *peer = 0;
 *generation = humbleNetState.reconnectGeneration;
 if (!initialized) return 5;
 if (!alias || !humbleNetState.desiredAliases.count(alias)) return 0;
 if (humbleNetState.confirmedAliases.count(alias) && humbleNetState.myPeerId &&
     humbleNetState.p2pConn && humbleNetState.p2pConn->wsi) {
  *peer = humbleNetState.myPeerId;
  return 2;
 }
 return 1;
}
PeerId HUMBLENET_CALL humblenet_dz_route_for_peer(PeerId peer) {
 HUMBLENET_GUARD();
 PeerId route = 0;
 for (auto& it : p2pconnections)
  if (it.second->otherPeer == peer && (!route || internal_alias_is_virtual_peer(it.first))) route = it.first;
 return route;
}

/*
 * Disconnect a peer
 */
ha_bool HUMBLENET_CALL humblenet_p2p_disconnect(PeerId peer) {
	P2P_INIT_GUARD( false );

	HUMBLENET_GUARD();

	if( peer == 0 /*all*/) {
		for( auto it = p2pconnections.begin(); it != p2pconnections.end(); ++it ) {
			humblenet_connection_set_closed( it->second );
		}
	} else {
		auto it = p2pconnections.find( peer );
		if( it != p2pconnections.end() )
			humblenet_connection_set_closed( it->second );
	}
	// TODO: Should we see if the peer that was passed in is the "real" peer instead of the VPeer?
	return 1;
}

void internal_poll_io() {
	humblenet_p2p_wait(0);
}

#ifndef EMSCRIPTEN

#include "libpoll.h"	// SKIP_AMALGAMATOR_INCLUDE

// Native only API, should be moved to its own file
int HUMBLENET_CALL humblenet_p2p_select(int nfds, fd_set *readfds, fd_set *writefds,
										fd_set *exceptfds, struct timeval *timeout) {

	// not guard needed poll_select is internally init and thread safe.
	return poll_select( nfds, readfds, writefds, exceptfds, timeout );
}

ha_bool HUMBLENET_CALL humblenet_p2p_wait(int ms) {
	P2P_INIT_GUARD( false );

	if( ms > 0 ) {
		HUMBLENET_GUARD();

		if( ! humbleNetState.pendingDataConnections.empty() || ! humbleNetState.pendingNewConnections.empty() || ! humbleNetState.remoteClosedConnections.empty() )
			return true;
	}

	poll_wait(ms);
	HUMBLENET_GUARD();

	return ! humbleNetState.pendingDataConnections.empty() || ! humbleNetState.pendingNewConnections.empty() || ! humbleNetState.remoteClosedConnections.empty();
}

#else
ha_bool HUMBLENET_CALL humblenet_p2p_wait(int ms) {
	P2P_INIT_GUARD( false );

	return ! humbleNetState.pendingDataConnections.empty() || ! humbleNetState.pendingNewConnections.empty() || ! humbleNetState.remoteClosedConnections.empty();
}
#endif
