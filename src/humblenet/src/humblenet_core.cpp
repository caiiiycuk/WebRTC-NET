#include "humblenet.h"
#include "humblenet_p2p.h"
#include "humblenet_p2p_internal.h"
#include "humblenet_alias.h"

#define NOMINMAX

#include <cassert>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

#if defined(WIN32)
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
#else
	#include <sys/time.h>
#endif

#if defined(EMSCRIPTEN)
	#include <emscripten/emscripten.h>

	EM_JS(char*, humblenet_get_net_config_ice_servers_json, (), {
		const scope = typeof window !== "undefined" ? window : globalThis;
		if(!scope.netConfig || typeof scope.netConfig.iceServers === "undefined") {
			return 0;
		}

		const json = JSON.stringify(scope.netConfig.iceServers);
		return typeof json === "string" ? stringToNewUTF8(json) : 0;
	});
#endif

#include "humblepeer.h"

#include "libsocket.h"
#include "json.h"

#include "humblenet_p2p_internal.h"
#include "humblenet_utils.h"

#define USE_STUN

HumbleNetState humbleNetState;

namespace {

json_value* find_object_value(json_value* root, const char* key) {
	if(root == nullptr || root->type != json_object) {
		return nullptr;
	}

	for(unsigned int i = 0; i < root->u.object.length; ++i) {
		json_object_entry& entry = root->u.object.values[i];
		if(std::strcmp(entry.name, key) == 0) {
			return entry.value;
		}
	}

	return nullptr;
}

std::string get_optional_json_string(json_value* root, const char* key) {
	json_value* value = find_object_value(root, key);
	if(value == nullptr || value->type != json_string) {
		return "";
	}
	return value->u.string.ptr;
}

bool append_ice_server(const std::string& url,
	const std::string& username,
	const std::string& credential,
	std::vector<humblenet::ICEServer>& servers,
	std::string& error) {
	if(url.empty()) {
		error = "ICE server entry contains an empty url";
		return false;
	}

	if(url.rfind("stun:", 0) == 0 || url.rfind("stuns:", 0) == 0) {
		servers.emplace_back(url);
		return true;
	}

	if(url.rfind("turn:", 0) == 0 || url.rfind("turns:", 0) == 0 || !username.empty() || !credential.empty()) {
		servers.emplace_back(url, username, credential);
		return true;
	}

	servers.emplace_back(url);
	return true;
}

bool parse_ice_servers_json(const char* json,
	std::vector<humblenet::ICEServer>& servers,
	std::string& error) {
	servers.clear();
	if(json == nullptr || json[0] == '\0') {
		return true;
	}

	json_value* root = json_parse(json, std::strlen(json));
	if(root == nullptr) {
		error = "Unable to parse ICE servers JSON";
		return false;
	}

	if(root->type != json_array) {
		json_value_free(root);
		error = "ICE servers JSON must be an array";
		return false;
	}

	for(unsigned int index = 0; index < root->u.array.length; ++index) {
		json_value* entry = root->u.array.values[index];
		if(entry == nullptr || entry->type != json_object) {
			json_value_free(root);
			error = "Each ICE server entry must be an object";
			return false;
		}

		const std::string username = get_optional_json_string(entry, "username");
		const std::string credential = get_optional_json_string(entry, "credential");
		json_value* urls = find_object_value(entry, "urls");
		if(urls == nullptr) {
			json_value_free(root);
			error = "ICE server entry is missing urls";
			return false;
		}

		if(urls->type == json_string) {
			if(!append_ice_server(urls->u.string.ptr, username, credential, servers, error)) {
				json_value_free(root);
				return false;
			}
			continue;
		}

		if(urls->type != json_array) {
			json_value_free(root);
			error = "ICE server urls must be a string or array";
			return false;
		}

		for(unsigned int url_index = 0; url_index < urls->u.array.length; ++url_index) {
			json_value* url = urls->u.array.values[url_index];
			if(url == nullptr || url->type != json_string) {
				json_value_free(root);
				error = "ICE server urls array must only contain strings";
				return false;
			}

			if(!append_ice_server(url->u.string.ptr, username, credential, servers, error)) {
				json_value_free(root);
				return false;
			}
		}
	}

	json_value_free(root);
	return true;
}

bool has_turn_server(const std::vector<humblenet::ICEServer>& servers) {
	return std::any_of(servers.begin(), servers.end(), [](const humblenet::ICEServer& server) {
		return server.type == humblenet::ICEServerType::TURNServer;
	});
}

void abort_invalid_ice_configuration(const std::string& reason) {
	std::fprintf(stderr,
		"\n"
		"########################################################################\n"
		"# HUMBLENET FATAL: INVALID ICE SERVER CONFIGURATION                    #\n"
		"########################################################################\n"
		"%s\n"
		"HumbleNet requires at least one STUN or TURN server before P2P startup.\n"
		"Native: call humblenet_set_iceservers() before humblenet_p2p_init().\n"
		"Web: call that API or define window.netConfig.iceServers before startup.\n"
		"Execution is being terminated.\n"
		"########################################################################\n\n",
		reason.c_str());
	std::fflush(stderr);
	std::abort();
}

void print_missing_turn_server_warning() {
	std::fprintf(stderr,
		"\n"
		"!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
		"! HUMBLENET WARNING: ICE CONFIGURATION CONTAINS NO TURN SERVER         !\n"
		"!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
		"Only direct/STUN connectivity is available. Connections will fail for\n"
		"peers behind restrictive NATs, symmetric NATs, or restrictive firewalls.\n"
		"Add at least one turn: or turns: URL for reliable WebRTC connectivity.\n"
		"!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n");
	std::fflush(stderr);
}

void warn_if_turn_server_is_missing(const std::vector<humblenet::ICEServer>& servers) {
	if(!has_turn_server(servers)) {
		print_missing_turn_server_warning();
	}
}

void validate_ice_configuration(const std::vector<humblenet::ICEServer>& servers) {
	if(servers.empty()) {
		abort_invalid_ice_configuration("The final ICE server list is empty.");
	}

	warn_if_turn_server_is_missing(servers);
}

#if defined(EMSCRIPTEN)
void apply_net_config_ice_servers_fallback() {
	if(humbleNetState.iceServersConfiguredExplicitly) {
		return;
	}

	char* json = humblenet_get_net_config_ice_servers_json();
	if(json == nullptr) {
		return;
	}

	std::vector<humblenet::ICEServer> parsedServers;
	std::string error;
	const bool parsed = parse_ice_servers_json(json, parsedServers, error);
	std::free(json);

	if(!parsed) {
		abort_invalid_ice_configuration("Unable to use window.netConfig.iceServers: " + error);
	}

	humbleNetState.configuredIceServers.swap(parsedServers);
}
#endif

void apply_configured_ice_servers_locked() {
	if(humbleNetState.context == nullptr) {
		return;
	}

	internal_set_ice_servers(
		humbleNetState.context,
		humbleNetState.configuredIceServers.data(),
		humbleNetState.configuredIceServers.size());
}

} // namespace

#ifdef WIN32
#if defined(_MSC_VER) || defined(_MSC_EXTENSIONS)
#define DELTA_EPOCH_IN_MICROSECS  11644473600000000Ui64
#else
#define DELTA_EPOCH_IN_MICROSECS  11644473600000000ULL
#endif
uint64_t sys_milliseconds(void)
{
	FILETIME ft;
	ULARGE_INTEGER temp;

	GetSystemTimeAsFileTime(&ft);
	memcpy(&temp, &ft, sizeof(temp));

	return (temp.QuadPart - DELTA_EPOCH_IN_MICROSECS) / 100000;
}

#else
uint64_t sys_milliseconds (void)
{
	struct timeval tp;
	struct timezone tzp;
	static uint64_t	secbase = 0;

	gettimeofday(&tp, &tzp);

	if (!secbase)
	{
		secbase = tp.tv_sec;
		return tp.tv_usec/1000;
	}

	return (tp.tv_sec - secbase)*1000 + tp.tv_usec/1000;
}
#endif

void signal();

void blacklist_peer( PeerId peer ) {
	humbleNetState.peerBlacklist.insert( std::make_pair( peer, sys_milliseconds() + 1000 ) );
}

/*
 * Deterimine if the peer is currently blacklisted.
 *
 * Side effect: will clear expired blacklist entries.
 */
bool is_peer_blacklisted( PeerId peer_id ) {
	auto it = humbleNetState.peerBlacklist.find(peer_id);
	if( it == humbleNetState.peerBlacklist.end() )
		return false;
	
	if( it->second <= sys_milliseconds() ) {
		humbleNetState.peerBlacklist.erase(it);
		return false;
	}

	return true;
}

/*
 * See if we can try a connection to this peer
 *
 * Side effect: sets error to reason on failure
 */
bool can_try_peer( PeerId peer_id ) {
	if (!humbleNetState.p2pConn) {
		humblenet_set_error("Signaling connection not established");
		// no signaling connection
		return false;
	}
	if (humbleNetState.myPeerId == 0) {
		humblenet_set_error("No peer ID from server");
		return false;
	}
	if (humbleNetState.pendingPeerConnectionsOut.find(peer_id)
		!= humbleNetState.pendingPeerConnectionsOut.end()) {
		// error, already a pending outgoing connection to this peer
		humblenet_set_error("already a pending connection to peer");
		LOG("humblenet_connect_peer: already a pending connection to peer %u\n", peer_id);
		return false;
	}
	if( is_peer_blacklisted(peer_id) ) {
		// peer is currently black listed
		humblenet_set_error("peer blacklisted");
		LOG("humblenet_connect_peer: peer blacklisted %u\n", peer_id);
		return false;
	}
	
	return true;
}


// BEGIN CONNECTION HANDLING

void humblenet_connection_set_closed( Connection* conn ) {
	if( conn->socket ) {
		// clear our state first so callbacks dont work on us.
		humbleNetState.connections.erase(conn->socket);
		internal_set_data(conn->socket, NULL );
		{
			HUMBLENET_UNGUARD();
			internal_close_socket(conn->socket);
		}
		conn->socket = NULL;
	}

	if( conn->inOrOut == Incoming ) {
		blacklist_peer( conn->otherPeer );
	}
	
	conn->status = HUMBLENET_CONNECTION_CLOSED;

	LOG("Marking connections closed: %u\n", conn->otherPeer );

	// make sure were not in any lists...
	erase_value( humbleNetState.connections, conn );
	humbleNetState.pendingNewConnections.erase( conn );
	humbleNetState.pendingDataConnections.erase( conn );
//	humbleNetState.remoteClosedConnections.erase( conn );
	erase_value( humbleNetState.pendingPeerConnectionsOut, conn );
	erase_value( humbleNetState.pendingPeerConnectionsIn, conn );
	erase_value( humbleNetState.pendingAliasConnectionsOut, conn );

	// mark it as being close pending.
	humbleNetState.remoteClosedConnections.insert( conn );

	signal();
}


ha_bool humblenet_connection_is_readable(Connection *connection) {
	return !connection->recvBuffer.empty();
}


ha_bool humblenet_connection_is_writable(Connection *connection) {
	assert(connection != NULL);
	if (connection->status == HUMBLENET_CONNECTION_CONNECTED) {
		return connection->writable;
	}
	return false;
}


int humblenet_connection_write(Connection *connection, const void *buf, uint32_t bufsize) {
	assert(connection != NULL);

	switch (connection->status) {
		case HUMBLENET_CONNECTION_CLOSED:
			// connection has been closed
			assert(connection->socket == NULL);
			return -1;

		case HUMBLENET_CONNECTION_CONNECTING:
			assert(connection->socket != NULL);
			return 0;

		case HUMBLENET_CONNECTION_CONNECTED:
			assert(connection->socket != NULL);
		{
			HUMBLENET_UNGUARD();
			return internal_write_socket( connection->socket, buf, bufsize );
		}
	}
	return -1;
}


int humblenet_connection_read(Connection *connection, void *buf, uint32_t bufsize) {
	assert(connection != NULL);

	switch (connection->status) {
		case HUMBLENET_CONNECTION_CLOSED:
			// connection has been closed
			assert(connection->socket == NULL);
			return -1;

		case HUMBLENET_CONNECTION_CONNECTING:
			assert(connection->socket != NULL);
			return 0;

		case HUMBLENET_CONNECTION_CONNECTED:
			assert(connection->socket != NULL);

			if( connection->recvBuffer.empty() )
				return 0;

			bufsize = std::min<uint32_t>(bufsize, connection->recvBuffer[0].size() );
			memcpy(buf, &connection->recvBuffer[0][0], bufsize);
			connection->recvBuffer.pop_front();

			if( ! connection->recvBuffer.empty() )
				humbleNetState.pendingDataConnections.insert(connection);
			else
				humbleNetState.pendingDataConnections.erase(connection);

			return bufsize;
	}
	return -1;
}

ConnectionStatus humblenet_connection_status(Connection *connection) {
	assert(connection != NULL);

	return connection->status;
}

Connection *humblenet_connect_websocket(const char *server_addr) {
	humblenet_set_error("Websocket support deprecated");
	return NULL;
}


Connection *humblenet_connect_peer(PeerId peer_id) {
	if( ! can_try_peer( peer_id ) )
		return NULL;

	Connection *connection = new Connection(Outgoing);
	connection->otherPeer = peer_id;
	{
		HUMBLENET_UNGUARD();
		connection->socket = internal_create_webrtc(humbleNetState.context);
	}
	internal_set_data(connection->socket, connection);

	humbleNetState.pendingPeerConnectionsOut.emplace(peer_id, connection);

	int ret;

	{
		HUMBLENET_UNGUARD();
		ret = internal_create_offer( connection->socket );
	}

	if( ! ret ) {
		LOG("Unable to generate sdp offer to peer: %d\n", peer_id);
		humblenet_set_error("Unable to generate sdp offer");
		humblenet_connection_close(connection);
		return NULL;
	}

	LOG("connecting to peer: %d\n", peer_id);
	return connection;
}


void humblenet_connection_close(Connection *connection) {
	assert(connection != NULL);

	humblenet_connection_set_closed(connection);

	internal_alias_remove_connection( connection );

	humbleNetState.remoteClosedConnections.erase(connection);

	delete connection;
}

PeerId humblenet_connection_get_peer_id(Connection *connection) {
	assert(connection != NULL);

	return connection->otherPeer;
}


// called to return the sdp offer
int on_sdp( internal_socket_t* s, const char* offer, void* user_data ) {
	HUMBLENET_GUARD();

	Connection* conn = reinterpret_cast<Connection*>(user_data);

	if( conn == NULL ) {
		LOG("on_sdp: Got socket w/o state?\n");
		return -1;
	}

	assert( conn->status == HUMBLENET_CONNECTION_CONNECTING );

	// no trickle ICE
	int flags = 0x2;

	if( conn->inOrOut == Incoming ) {
		LOG("P2PConnect SDP sent %u response offer = \"%s\"\n", conn->otherPeer, offer);
		if( ! sendP2PResponse(humbleNetState.p2pConn.get(), conn->otherPeer, offer) ) {
			return -1;
		}
	} else {
		LOG("outgoing SDP sent %u offer: \"%s\"\n", conn->otherPeer, offer);
		if( ! sendP2PConnect(humbleNetState.p2pConn.get(), conn->otherPeer, flags, offer) ) {
			return -1;
		}
	}
	return 0;
}

// called to send a candidate
int on_ice_candidate( internal_socket_t* s, const char* offer, void* user_data ) {
	HUMBLENET_GUARD();

	Connection* conn = reinterpret_cast<Connection*>(user_data);
	
	if( conn == NULL ) {
		LOG("on_ice: Got socket w/o state?\n");
		return -1;
	}

	// This is OK with trickle ICE
	// assert( conn->status == HUMBLENET_CONNECTION_CONNECTING );

	LOG("Sending ice candidate to peer: %u, %s\n", conn->otherPeer, offer );
	if( ! sendICECandidate(humbleNetState.p2pConn.get(), conn->otherPeer, offer) ) {
		return -1;
	}

	return 0;
}

// called for incoming connections to indicate the connection process is completed.
int on_accept (internal_socket_t* s, void* user_data) {
	HUMBLENET_GUARD();

	auto it = humbleNetState.connections.find( s );

	// TODO: Rework this ?
	Connection* conn = NULL;
	if( it == humbleNetState.connections.end() ) {
		// its a websocket connection
		conn = new Connection( Incoming, s );

		// track the connection, ALL connections will reside here.
		humbleNetState.connections.insert( std::make_pair( s, conn ) );
	} else {
		// should be a webrtc connection (as we create the socket in order to start the process)
		conn = it->second;
	}

	// TODO: This should be "waiting for accept"
	conn->status = HUMBLENET_CONNECTION_CONNECTED;

	// expose it as an incoming connection to be accepted...
	humbleNetState.pendingNewConnections.insert( conn );

	LOG("accepted peer: %d\n", conn->otherPeer );

	signal();

	return 0;
}

// called for outgoing connections to indicate the connection process is completed.
int on_connect (internal_socket_t* s, void* user_data) {
	HUMBLENET_GUARD();

	assert( user_data );

	auto it = humbleNetState.connections.find( s );

	// TODO: Rework this ?
	Connection* conn = reinterpret_cast<Connection*>(user_data);
	if( it == humbleNetState.connections.end() ) {
		// track the connection, ALL connections will reside here.
		humbleNetState.connections.insert( std::make_pair( s, conn ) );
	}

	assert( conn->status == HUMBLENET_CONNECTION_CONNECTING );

	// outgoing always initiates the channel
	if( conn->inOrOut == Outgoing ) {
		if( ! internal_create_channel(s, "dataChannel") )
			return -1;
	}

	LOG("connected peer: %d\n", conn->otherPeer );

	return 0;
}

int on_accept_channel( internal_socket_t* s, const char* name, void* user_data ) {
	HUMBLENET_GUARD();

	assert( user_data );

	Connection* conn = reinterpret_cast<Connection*>(user_data);

	assert( conn->status == HUMBLENET_CONNECTION_CONNECTING );
	conn->status = HUMBLENET_CONNECTION_CONNECTED;
	// This connection is now fully established and must not be treated as pending.
	// Otherwise signaling reconnect logic can incorrectly close active WebRTC channels.
	erase_value(humbleNetState.pendingPeerConnectionsOut, conn);
	erase_value(humbleNetState.pendingPeerConnectionsIn, conn);
	erase_value(humbleNetState.pendingAliasConnectionsOut, conn);

	LOG("accepted channel: %d:%s\n", conn->otherPeer, name );

	signal();

	return 0;
}

int on_connect_channel( internal_socket_t* s, const char* name, void* user_data ) {
	HUMBLENET_GUARD();

	assert( user_data );

	Connection* conn = reinterpret_cast<Connection*>(user_data);

	assert( conn->status == HUMBLENET_CONNECTION_CONNECTING );
	conn->status = HUMBLENET_CONNECTION_CONNECTED;
	// This connection is now fully established and must not be treated as pending.
	// Otherwise signaling reconnect logic can incorrectly close active WebRTC channels.
	erase_value(humbleNetState.pendingPeerConnectionsOut, conn);
	erase_value(humbleNetState.pendingPeerConnectionsIn, conn);
	erase_value(humbleNetState.pendingAliasConnectionsOut, conn);

	LOG("connected channel: %d:%s\n", conn->otherPeer, name );
	return 0;
}

// called each time data is received.
int on_data( internal_socket_t* s, const void* data, int len, void* user_data ) {
	HUMBLENET_GUARD();

	// already disconnected from this socket.
	if( ! user_data )
		return -1;

	Connection* conn = reinterpret_cast<Connection*>(user_data);

	assert( conn->status == HUMBLENET_CONNECTION_CONNECTED );

	if( conn->recvBuffer.empty() ) {
		assert( humbleNetState.pendingDataConnections.find(conn) == humbleNetState.pendingDataConnections.end() );
		humbleNetState.pendingDataConnections.insert(conn);
	}

	conn->recvBuffer.emplace_back(reinterpret_cast<const char*>(data),
							reinterpret_cast<const char*>(data)+len);

	signal();

	return 0;
}

// called to indicate the connection is wriable.
int on_writable (internal_socket_t* s, void* user_data) {
	HUMBLENET_GUARD();

	auto it = humbleNetState.connections.find( s );
	
	if( it != humbleNetState.connections.end() ) {
		// its not here anymore
		return -1;
	}

	Connection* conn = it->second;
	conn->writable = true;

	return 0;
}

// called when the conenction is terminated (from either side)
int on_disconnect( internal_socket_t* s, void* user_data ) {
	HUMBLENET_GUARD();

	// find Connection object
	auto it = humbleNetState.connections.find(s);

	if (it != humbleNetState.connections.end()) {
		// it's still in connections map, not user-initiated close
		// find Connection object and set wsi to NULL
		// to signal that it's dead
		// user code will then call humblenet_close to dispose of
		// the Connection object

		humblenet_connection_set_closed( it->second );
	} else if( user_data != NULL ) {
		// just to be sure...
		humblenet_connection_set_closed( reinterpret_cast<Connection*>( user_data ) );
	}
	// if it wasn't in the table, no need to do anything
	// humblenet_close has already disposed of everything else
	return 0;
}

int on_destroy( internal_socket_t* s, void* user_data ) {
	HUMBLENET_GUARD();

	// if no user_data attached, then nothing to cleanup.
	if( ! user_data )
		return 0;

	// make sure we are fully detached...
	Connection* conn = reinterpret_cast<Connection*>(user_data);
	humblenet_connection_set_closed( conn );

	return 0;
}

// END CONNECTION HANDLING

// BEGIN INITIALIZATION

uint32_t HUMBLENET_CALL humblenet_version() {
	return HUMBLENET_COMPILEDVERSION;
}

ha_bool HUMBLENET_CALL humblenet_loader_init(const char* path) {
	return true;
}

ha_bool HUMBLENET_CALL humblenet_init() {
	return true;
}

ha_bool HUMBLENET_CALL humblenet_set_iceservers(const char* json) {
	{
		HUMBLENET_GUARD();
		humbleNetState.iceServersConfiguredExplicitly = true;
	}

	std::vector<humblenet::ICEServer> parsedServers;
	std::string error;
	if(!parse_ice_servers_json(json, parsedServers, error)) {
		humblenet_set_error(error.c_str());
		return false;
	}
	if(parsedServers.empty()) {
		abort_invalid_ice_configuration("humblenet_set_iceservers() received an empty ICE server list.");
	}
	const bool turnServerMissing = !has_turn_server(parsedServers);

	bool contextExists = false;
	{
		HUMBLENET_GUARD();
		humbleNetState.configuredIceServers.swap(parsedServers);
		apply_configured_ice_servers_locked();
		contextExists = humbleNetState.context != nullptr;
	}

	if(contextExists && turnServerMissing) {
		print_missing_turn_server_warning();
	}

	return true;
}

ha_bool internal_p2p_register_protocol() {
#if defined(EMSCRIPTEN)
	apply_net_config_ice_servers_fallback();
#endif
	validate_ice_configuration(humbleNetState.configuredIceServers);

	internal_callbacks_t callbacks;

	memset(&callbacks, 0, sizeof(callbacks));

	callbacks.on_sdp = on_sdp;
	callbacks.on_ice_candidate = on_ice_candidate;
	callbacks.on_accept = on_accept;
	callbacks.on_connect = on_connect;
	callbacks.on_accept_channel = on_accept_channel;
	callbacks.on_connect_channel = on_connect_channel;
	callbacks.on_data = on_data;
	callbacks.on_disconnect = on_disconnect;
	callbacks.on_writable = on_writable;
	// The fallback callback has an incompatible WASM signature.
	callbacks.on_destroy = on_destroy;

	internal_context_t* context = internal_init(  &callbacks );
	if (context == NULL) {
		return false;
	}

	if (!humblenet::register_protocol(context)) {
		internal_deinit(context);
		return false;
	}

	humbleNetState.context = context;
	humbleNetState.webRTCSupported = internal_supports_webRTC( humbleNetState.context );
	apply_configured_ice_servers_locked();

	return true;
}

void HUMBLENET_CALL humblenet_shutdown() {
	{
		HUMBLENET_GUARD();

		// Destroy all connections
		for( auto it = humbleNetState.connections.begin(); it != humbleNetState.connections.end(); ) {
			Connection* conn = it->second;
			++it;
			humblenet_connection_close( conn );
		}
	}

	humblenet_p2p_shutdown();

}

// END INITIALIZATION

// BEGIN MISC

ha_bool HUMBLENET_CALL humblenet_p2p_supported() {
	// This function should really be "humblenet_has_webrtc"
	// but could be removed entirely from the API
	return humbleNetState.webRTCSupported;
}

#if defined(WIN32)
#define THREAD_LOCAL __declspec(thread)
#else // Everyone else
#define THREAD_LOCAL __thread
#endif

thread_local std::string errorString;

const char * HUMBLENET_CALL humblenet_get_error() {
	return errorString.empty() ? nullptr : errorString.c_str();
}


void HUMBLENET_CALL humblenet_set_error(const char *error) {
	errorString = error ? error : "";
}


void HUMBLENET_CALL humblenet_clear_error() {
	errorString.clear();
}

// END MISC

// BEGIN DEPRECATED

Connection *humblenet_poll_all(int timeout_ms) {
	{
		// check for connections closed by remote and notify user
		auto it = humbleNetState.remoteClosedConnections.begin();
		if (it != humbleNetState.remoteClosedConnections.end()) {
			Connection *conn = *it;
			humbleNetState.remoteClosedConnections.erase(it);
			return conn;
		}

		it = humbleNetState.pendingDataConnections.begin();
		if (it != humbleNetState.pendingDataConnections.end()) {
			// don't remove it from the set, _recv will do that
			Connection *conn = *it;
			assert(conn != NULL);
			assert(!conn->recvBuffer.empty());
			return conn;
		}
	}

	// call service if no outstanding data on any connection
	internal_poll_io(/*timeout_ms*/);

	{
		auto it = humbleNetState.pendingDataConnections.begin();
		if (it != humbleNetState.pendingDataConnections.end()) {
			// don't remove it from the set, _recv will do that
			Connection *conn = *it;
			assert(conn != NULL);
			assert(!conn->recvBuffer.empty());
			return conn;
		}
	}

	return NULL;
}

Connection *humblenet_connection_accept() {
	{
		auto it = humbleNetState.pendingNewConnections.begin();

		if (it != humbleNetState.pendingNewConnections.end()) {
			Connection *conn = *it;
			humbleNetState.pendingNewConnections.erase(it);
			return conn;
		}
	}

	// call service so something happens
	// TODO: if we replace poll loop with our own we could poll only the master socket
	internal_poll_io();

	{
		auto it = humbleNetState.pendingNewConnections.begin();
		if (it != humbleNetState.pendingNewConnections.end()) {
			Connection *conn = *it;
			humbleNetState.pendingNewConnections.erase(it);
			return conn;
		}
	}

	return NULL;
}

// END DEPRECATED

static std::unordered_map<std::string, std::string> hints;

/*
 * Set the value of a hint
 */
HUMBLENET_API ha_bool HUMBLENET_CALL humblenet_set_hint(const char* name, const char* value) {
    HUMBLENET_GUARD();

	auto it = hints.find( name );
	if( it != hints.end() )
		it->second = value;
	else
		hints.insert( std::make_pair( name, value ) );
	return 1;
}

/*
 * Get the value of a hint
 */
HUMBLENET_API const char* HUMBLENET_CALL humblenet_get_hint(const char* name) {
	HUMBLENET_GUARD();

	auto it = hints.find( name );
	if( it != hints.end() )
		return it->second.c_str();
	else
		return NULL;
}

#ifndef EMSCRIPTEN

#include "libpoll.h"	// SKIP_AMALGAMATOR_INCLUDE

void humblenet_lock() {
	poll_lock();
}

void humblenet_unlock() {
	poll_unlock();
}

void signal() {
	poll_interrupt();
}

void humblenet_timer( timer_callback_t callback, int timeout, void* data)
{
	poll_timeout( callback, timeout, data );
}

#else

void humblenet_lock() {
}

void humblenet_unlock() {
}

void signal () {
}

void humblenet_timer( timer_callback_t callback, int timeout, void* data)
{
	EM_ASM_({
	  setTimeout(function () {
		getWasmTableEntry($0)($2);
	  }, $1);
	}, callback, timeout, data);
}

#endif
