#include "libsocket.h"
#include "humblenet_portal_internal.h"

#include <map>
#include <condition_variable>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <cassert>
#include <atomic>
#include <cstring>

#ifdef EMSCRIPTEN
#include "libwebsockets_asmjs.h"
#else
#include "libwebsockets_native.h"	// SKIP_AMALGAMATOR_INCLUDE
#include "libpoll.h"			// SKIP_AMALGAMATOR_INCLUDE
#include "cert_pem.h"				// SKIP_AMALGAMATOR_INCLUDE
#include <openssl/ssl.h>
#endif

#include "humblenet.h"
#include "libwebrtc.h"
#include "humblepeer.h"

// TODO: should have a way to disable this on release builds
#define LOG printf

#ifndef EMSCRIPTEN
static const lws_retry_bo_t websocket_retry_policy = {
	NULL, 0, 0, 30, 100, 0
};
#endif

struct internal_socket_t {
	bool owner;
	bool closing;			// if this is set, ignore close attempts as the close process has already been initiated.
	void* user_data;
	struct internal_context_t* context;
	internal_callbacks_t callbacks;
	
	// web socket connection info
	struct lws *wsi;
	bool websocket_established;
	std::string url;
	
	// webrtc connection info
	struct libwebrtc_connection* webrtc;
	std::atomic<struct libwebrtc_data_channel*> webrtc_channel; // portal send reads it off the signaling thread
	
	internal_socket_t(bool owner=true)
	:owner(owner)
	,closing(false)
	,user_data(NULL)
	,context(NULL)
	,wsi(NULL)
	,websocket_established(false)
	,webrtc(NULL)
	,webrtc_channel(NULL)
	{}
	
	~internal_socket_t(){
		assert( owner );
	}
};

struct internal_context_t {
	internal_context_t()
	: callbacks()
	, websocket(NULL)
	, webrtc(NULL)
#ifndef EMSCRIPTEN
	, shuttingDown(false)
	, shutdownComplete(false)
	, suppressWebSocketCallbacks(false)
#endif
	{}

	internal_callbacks_t callbacks;
	
	std::map<std::string,internal_callbacks_t> protocols;
	
	// websocket
	struct lws_context *websocket;

	// webrtc
	struct libwebrtc_context* webrtc;

#ifndef EMSCRIPTEN
	std::mutex shutdownMutex;
	std::condition_variable shutdownCondition;
	std::set<internal_socket_t*> websockets;
	bool shuttingDown;
	bool shutdownComplete;
	std::atomic<bool> suppressWebSocketCallbacks;
#endif
};

static internal_context_t* g_context;

static bool websocket_callbacks_suppressed(internal_socket_t* socket)
{
#ifdef EMSCRIPTEN
	return false;
#else
	return socket != NULL && socket->context != NULL &&
		socket->context->suppressWebSocketCallbacks.load();
#endif
}

#ifndef EMSCRIPTEN
static void internal_shutdown_step(void* data);

static void websocket_destroyed(internal_context_t* context, internal_socket_t* socket)
{
	bool continueShutdown = false;
	{
		std::lock_guard<std::mutex> lock(context->shutdownMutex);
		context->websockets.erase(socket);
		continueShutdown = context->shuttingDown && context->websockets.empty();
	}
	if (continueShutdown && poll_chain() != NULL)
		poll_dispatch(internal_shutdown_step, context);
}
#endif

int websocket_protocol(  struct lws *wsi
					   , enum lws_callback_reasons reason
					   , void *user, void *in, size_t len) {

//	LOG("%p %p %d %p\n", context, wsi, reason, user );

	internal_socket_t* socket = reinterpret_cast<internal_socket_t*>( user );

	int ret = 0;
	
	switch (reason) {
		case LWS_CALLBACK_WSI_CREATE: {
			socket->wsi = wsi;
#ifndef EMSCRIPTEN
			if (socket->context != NULL) {
				std::lock_guard<std::mutex> lock(socket->context->shutdownMutex);
				socket->context->websockets.insert(socket);
			}
#endif
		}
		break;

		case LWS_CALLBACK_WSI_DESTROY: {
			internal_context_t* context = socket->context;
			socket->wsi = NULL;
			if (!websocket_callbacks_suppressed(socket))
				ret = socket->callbacks.on_destroy( socket, socket->user_data );
#ifndef EMSCRIPTEN
			if (context != NULL)
				websocket_destroyed(context, socket);
#endif
			if( socket->owner )
				delete socket;
		}
		break;

		case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
			if( socket ) {
				socket->closing = true;
				socket->websocket_established = false;
				if (!websocket_callbacks_suppressed(socket))
					ret = socket->callbacks.on_disconnect( socket, socket->user_data );
			}
		} break;
			
		case LWS_CALLBACK_ESTABLISHED:
		{
			socket->websocket_established = true;
			if (!websocket_callbacks_suppressed(socket))
				ret = socket->callbacks.on_accept( socket, socket->user_data );
		}
		break;
			
		case LWS_CALLBACK_CLIENT_ESTABLISHED:
		{
			socket->websocket_established = true;
			if (!websocket_callbacks_suppressed(socket))
				ret = socket->callbacks.on_connect( socket, socket->user_data );
		}
		break;
			
		case LWS_CALLBACK_CLOSED:
		{
			socket->closing = true;
			socket->websocket_established = false;
			if (!websocket_callbacks_suppressed(socket))
				ret = socket->callbacks.on_disconnect( socket, socket->user_data );
		}
		break;
			
		case LWS_CALLBACK_RECEIVE:
		case LWS_CALLBACK_CLIENT_RECEIVE:
		{
			if (!websocket_callbacks_suppressed(socket))
				ret = socket->callbacks.on_data( socket, in, len, socket->user_data );
		}
		break;
			
		case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
			std::map<std::string, internal_callbacks_t>::iterator it = g_context->protocols.find( std::string( (const char*)in, len ) );
			if( it == g_context->protocols.end() ) {
				LOG("Unknown protocol: %s\n", (const char*)in);
				return -1;
			}
			
			socket->callbacks = it->second;
			return 0;
		} break;
			
		case LWS_CALLBACK_CLIENT_WRITEABLE:
		case LWS_CALLBACK_SERVER_WRITEABLE:
		{
			if (socket->closing && socket->wsi) {
				return -1;
			}
			if (!websocket_callbacks_suppressed(socket))
				ret = socket->callbacks.on_writable( socket, socket->user_data );
		}
		break;

#if !defined(EMSCRIPTEN)
		case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
		{
			int i, count = 0;
			SSL_CTX *ctx = (SSL_CTX*)user;
			BIO *in = NULL;
			X509 *x = NULL;
			in = BIO_new_mem_buf((void*)cert_pem, cert_pem_len);
			if (in == NULL) {
				break;
			}

			X509_STORE *store = SSL_CTX_get_cert_store( ctx );

			for (;;) {
				x = PEM_read_bio_X509_AUX(in, NULL, NULL, NULL);
				if (x == NULL) {
					if ((ERR_GET_REASON(ERR_peek_last_error()) == PEM_R_NO_START_LINE) && (count > 0)) {
						ERR_clear_error();
						break;
					} else {
#ifndef OPENSSL_IS_BORINGSSL
						ERR_put_error(ERR_LIB_X509, 0, ERR_R_PEM_LIB, NULL, 0);
#else
						OPENSSL_PUT_ERROR(X509, ERR_R_PEM_LIB);
#endif
						break;
					}
				}
				i = X509_STORE_add_cert(store, x);
				if (!i) break;
				count++;
				X509_free(x);
				x = NULL;
			}
			if (x != NULL)
				X509_free(x);
			BIO_free(in);
		}
			break;
#endif

		default:
			//LOG("callback_humblenet %p %p %u %p %p %u\n", context, wsi, reason, user, in, static_cast<unsigned int>(len));
			break;
	}
	
	return ret;
}

int webrtc_protocol(struct libwebrtc_context *context,
							  struct libwebrtc_connection *connection, struct libwebrtc_data_channel* channel,
							  enum libwebrtc_callback_reasons reason, void *user,
							  void *in, int len)
{
	internal_socket_t* socket = reinterpret_cast<internal_socket_t*>( user );

//	LOG("%p %p %p %d %p\n", context, connection, channel, reason, user );

	int ret = 0;
	
	switch( reason ) {
		case LWRTC_CALLBACK_LOCAL_DESCRIPTION:
			ret = socket->callbacks.on_sdp( socket, (const char*)in, socket->user_data );
			break;

		case LWRTC_CALLBACK_ICE_CANDIDATE:
			ret = socket->callbacks.on_ice_candidate( socket, (const char*)in, socket->user_data );
			break;

		case LWRTC_CALLBACK_ESTABLISHED:
			ret = socket->callbacks.on_connect( socket, socket->user_data );
			break;

		case LWRTC_CALLBACK_DISCONNECTED:
			socket->closing = true;
			ret = socket->callbacks.on_disconnect( socket, socket->user_data );
			break;

		case LWRTC_CALLBACK_CHANNEL_ACCEPTED:
			socket->webrtc_channel = channel;
			ret = socket->callbacks.on_accept_channel( socket, (const char*)in, socket->user_data );
			break;

		case LWRTC_CALLBACK_CHANNEL_CONNECTED:
			socket->webrtc_channel = channel;
			ret = socket->callbacks.on_connect_channel( socket, (const char*)in, socket->user_data );
			break;

		case LWRTC_CALLBACK_CHANNEL_RECEIVE:
			ret = socket->callbacks.on_data( socket, in, len, socket->user_data );
			break;

		case LWRTC_CALLBACK_CHANNEL_CLOSED:
			socket->webrtc_channel = NULL;
            if (portal_enabled()) {
                socket->closing = true;
                ret = socket->callbacks.on_disconnect(socket, socket->user_data);
                break;
            }

			// we are 1-1 DC -> channel, otherwise we would delegate this up.
			// socket->callbacks.on_disconnect_channel( socket, socket->user_data );

			// instead we simply close the connection as well
			if( !socket->closing && socket->webrtc )
				libwebrtc_close_connection( socket->webrtc );

			break;
			
		case LWRTC_CALLBACK_DESTROY:
			socket->callbacks.on_destroy( socket, socket->user_data );
			if( socket->owner )
				delete socket;
			break;
		case LWRTC_CALLBACK_ERROR:
			break;
	}

	return ret;
}


#define MAX_PROTOCOLS 16

struct lws_protocols protocols[MAX_PROTOCOLS] = {
	{ "default", websocket_protocol, sizeof(internal_socket_t) }
	,{ NULL, NULL, 0 }
};

int ok_callback() {
	LOG("Ok_CB\n");
	return 0;
}

int err_callback() {
	return -1;
}

void sanitize_callbacks( internal_callbacks_t& callbacks ) {
	intptr_t* ptr = (intptr_t*)&callbacks;

	for( int i = 0; i < sizeof(internal_callbacks_t)/sizeof(void*); ++i, ++ptr ) {
		if( !*ptr ) {
			LOG("Sanitize callback #%d\n", i+1 );
			*ptr = (intptr_t)&ok_callback;
		}
	}
}

internal_context_t* internal_init(internal_callbacks_t* callbacks) {
	internal_context_t* ctx = new internal_context_t();

	if( callbacks )
		ctx->callbacks= *callbacks;

	sanitize_callbacks( ctx->callbacks );

	struct lws_context_creation_info info;
	memset(&info, 0, sizeof(info));
	info.protocols = protocols;
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.gid = -1;
	info.uid = -1;
#ifdef LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
#else
	info.options = 0;
#endif
#ifndef EMSCRIPTEN
	info.retry_and_idle_policy = &websocket_retry_policy;
#endif
#if 0
#if defined __APPLE__ || defined(__linux__)
	// test a few wll known locations
	const char* certs[] = { "./cert.pem", "/etc/openssl/cert.pem", "/opt/local/etc/openssl/cert.pem", "/etc/pki/tls/cert.pem" , NULL };
	for( const char** test = certs; *test; test++ ) {
		if( access( *test, F_OK ) != -1 ) {
			info.ssl_ca_filepath = *test;
			break;
		}
	}
#elif defined(WIN32)
	info.ssl_ca_filepath = "cert.pem";
#endif
#endif

	ctx->websocket = lws_create_context_extended(&info);
	if (ctx->websocket == NULL) {
		delete ctx;
		return NULL;
	}
	ctx->webrtc = libwebrtc_create_context(&webrtc_protocol);
#ifndef EMSCRIPTEN
    if (ctx->webrtc && portal_enabled()) {
        const auto *api = libwebrtc_portal_api();
        if (!api || !api->enable(ctx->webrtc, portal_notify, nullptr)) {
            libwebrtc_destroy_context(ctx->webrtc); ctx->webrtc = nullptr;
        }
    }
#endif
	if (ctx->webrtc == NULL) {
		lws_context* websocket = ctx->websocket;
		ctx->websocket = NULL;
		if (websocket != NULL)
#ifdef EMSCRIPTEN
			lws_context_destroy(websocket);
#else
			lws_context_destroy_extended(websocket);
#endif
#ifndef EMSCRIPTEN
		poll_deinit();
#endif
		delete ctx;
		return NULL;
	}

#ifndef EMSCRIPTEN
	poll_start();
#endif

	return ctx;
}

void internal_publish_context(internal_context_t* ctx) {
	g_context = ctx;
}

bool internal_supports_webRTC(internal_context_t* ctx) {
	return ctx->webrtc != NULL;
}

void internal_set_ice_servers(internal_context_t* ctx, const humblenet::ICEServer* servers, int count) {
	if(ctx == NULL || ctx->webrtc == NULL) {
		return;
	}

	std::vector<libwebrtc_ice_server> tempServers;
	tempServers.reserve(count);
	for(int i = 0; i < count; ++i) {
		const humblenet::ICEServer& server = servers[i];
		libwebrtc_ice_server item;
		item.type = server.type == humblenet::ICEServerType::TURNServer
			? LIBWEBRTC_ICE_SERVER_TURN
			: LIBWEBRTC_ICE_SERVER_STUN;
		item.url = server.server.c_str();
		item.username = server.username.c_str();
		item.password = server.password.c_str();
		tempServers.push_back(item);
	}

	libwebrtc_set_ice_servers(ctx->webrtc, tempServers.data(), tempServers.size());
}

void internal_set_callbacks(internal_socket_t* socket, internal_callbacks_t* callbacks ) {
	if( ! callbacks ) {
		socket->callbacks = g_context->callbacks;
	} else {
		socket->callbacks = *callbacks;
		sanitize_callbacks( socket->callbacks );
	}
}

bool internal_register_protocol( internal_context_t* ctx, const char* name, internal_callbacks_t* callbacks ) {
	if (ctx == NULL || name == NULL || callbacks == NULL || ctx->protocols.size() + 2 > MAX_PROTOCOLS)
		return false;

	internal_callbacks_t cb = *callbacks;
	sanitize_callbacks( cb );

	ctx->protocols.insert( std::make_pair( std::string(name), cb ) );
	// TODO: Sanatize callbacks

	lws_protocols* protocol = protocols + ctx->protocols.size();
	// make sure the next protocols is "empty"
	*(protocol + 1) = *protocol;
	// now copy the prior record
	*protocol = *(protocol-1);
	// and update the name
	protocol->name = name;
	return true;
}

#ifndef EMSCRIPTEN
static void internal_shutdown_step(void* data)
{
	internal_context_t* ctx = static_cast<internal_context_t*>(data);
	libwebrtc_context* webrtc = NULL;
	lws_context* websocket = NULL;
	std::vector<internal_socket_t*> sockets;

	{
		std::lock_guard<std::mutex> lock(ctx->shutdownMutex);
		if (ctx->shutdownComplete)
			return;
		webrtc = ctx->webrtc;
		ctx->webrtc = NULL;
		sockets.assign(ctx->websockets.begin(), ctx->websockets.end());
		if (sockets.empty()) {
			websocket = ctx->websocket;
			ctx->websocket = NULL;
		}
	}

	if (webrtc != NULL)
		libwebrtc_destroy_context(webrtc);

	if (!sockets.empty()) {
		for (internal_socket_t* socket : sockets)
			internal_close_socket(socket);
		return;
	}

	if (websocket != NULL)
		lws_context_destroy_extended(websocket);

	{
		std::lock_guard<std::mutex> lock(ctx->shutdownMutex);
		ctx->shutdownComplete = true;
	}
	ctx->shutdownCondition.notify_all();
}
#endif

void internal_deinit(internal_context_t* ctx) {
	if (!ctx) return;
	if (g_context == ctx)
		g_context = NULL;

#ifdef EMSCRIPTEN
	lws_context_destroy(ctx->websocket);
	libwebrtc_destroy_context(ctx->webrtc);
#else
	{
		std::lock_guard<std::mutex> lock(ctx->shutdownMutex);
		ctx->shuttingDown = true;
	}
	if (poll_chain() != NULL)
		poll_dispatch(internal_shutdown_step, ctx);
	else
		internal_shutdown_step(ctx);

	std::unique_lock<std::mutex> lock(ctx->shutdownMutex);
	bool shutdownComplete = ctx->shutdownCondition.wait_for(
		lock, std::chrono::seconds(2), [ctx] { return ctx->shutdownComplete; });
	libwebrtc_context* webrtc = NULL;
	if (!shutdownComplete) {
		LOG("Timed out waiting for graceful websocket shutdown\n");
		ctx->shutdownComplete = true;
		ctx->suppressWebSocketCallbacks.store(true);
		webrtc = ctx->webrtc;
		ctx->webrtc = NULL;
	}
	lws_context* websocket = ctx->websocket;
	ctx->websocket = NULL;
	lock.unlock();
	if (!shutdownComplete) {
		if (webrtc != NULL)
			libwebrtc_destroy_context(webrtc);
		poll_deinit();
		if (websocket != NULL)
			lws_context_destroy_extended(websocket);
	} else {
		poll_deinit();
	}
#endif

	delete ctx;
}

void internal_set_data(internal_socket_t* socket, void* user_data) {
	socket->user_data = user_data;
}

void * internal_get_data(internal_socket_t* socket ) {
	return socket->user_data;
}

internal_socket_t* internal_connect_websocket( const char *server_addr, const char* protocol ) {
	std::map<std::string, internal_callbacks_t>::iterator it = g_context->protocols.find( protocol );
	if (it == g_context->protocols.end())
		return NULL;

	internal_socket_t* socket = new internal_socket_t(false);
	socket->context = g_context;
	socket->callbacks = it->second;
	socket->url = server_addr;

	struct lws* wsi = lws_client_connect_extended(g_context->websocket, server_addr, protocol, socket );
	socket->owner = true;
	if (wsi == NULL) {
		delete socket;
		return NULL;
	}
#ifdef EMSCRIPTEN
	socket->wsi = wsi;
#endif

	return socket;
}

internal_socket_t* internal_create_webrtc(internal_context_t* ctx) {
	if( ! ctx->webrtc )
		return NULL;

	internal_socket_t* socket = new internal_socket_t(true);
	socket->context = ctx;

	 socket->webrtc = libwebrtc_create_connection_extended(ctx->webrtc, socket);
    if (!socket->webrtc) { delete socket; return nullptr; }
	 socket->callbacks = ctx->callbacks;

	return socket;
}

int internal_create_offer(internal_socket_t* socket ) {
	assert( socket->webrtc != NULL );
	assert( socket->webrtc_channel == NULL );

	if( ! libwebrtc_create_offer( socket->webrtc ) )
		return 0;

	return 1;
}

int internal_set_offer( internal_socket_t* socket, const char* inOffer ){
	assert( socket->webrtc != NULL );
	assert( socket->webrtc_channel == NULL );

	if( ! libwebrtc_set_offer( socket->webrtc, inOffer ) )
		return 0;
	else
		return 1;
}

int internal_set_answer( internal_socket_t* socket, const char* inOffer ){
	assert( socket->webrtc != NULL );
	assert( socket->webrtc_channel == NULL );

	if( ! libwebrtc_set_answer( socket->webrtc, inOffer ) )
		return 0;
	else
		return 1;
}

int internal_add_ice_candidate( internal_socket_t* socket, const char* candidate ) {
	assert( socket->webrtc != NULL );
	return libwebrtc_add_ice_candidate( socket->webrtc, candidate );
}

int internal_create_channel( internal_socket_t* socket, const char* name ){
	assert( socket->webrtc != NULL );
	assert( socket->webrtc_channel == NULL );

	socket->webrtc_channel = libwebrtc_create_channel(socket->webrtc, name );

	if( !socket->webrtc_channel )
	   return 0;

	return 1;
}

void internal_close_socket( internal_socket_t* socket ) {
	if( socket->closing )
		// socket clos process has already started, ignore the request.
		return;
	else if( socket->wsi ) {
		socket->closing = true;
#ifndef EMSCRIPTEN
		if (socket->websocket_established) {
			lws_close_reason(socket->wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
			lws_callback_on_writable(socket->wsi);
		} else {
			lws_set_timeout(socket->wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
		}
#else
		lws_callback_on_writable(socket->wsi);
#endif
	} else if( socket->webrtc ) {
		// this will trigger the destruction of the channel and thus the destruction of our socket object.
		socket->closing = true;
		libwebrtc_close_connection( socket->webrtc );
	} else {
		assert( "Destroyed socket passed to close" == NULL );
	}
}

void internal_abort_socket( internal_socket_t* socket ) {
	if( socket->closing )
		return;
	else if( socket->wsi ) {
		socket->closing = true;
#ifndef EMSCRIPTEN
		lws_set_timeout(socket->wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
#else
		lws_callback_on_writable(socket->wsi);
#endif
	} else if( socket->webrtc ) {
		socket->closing = true;
		libwebrtc_close_connection( socket->webrtc );
	} else {
		assert( "Destroyed socket passed to abort" == NULL );
	}
}

int internal_write_socket(internal_socket_t* socket, const void *buf, int bufsize) {
	if( socket->wsi ) {
		// TODO: Should this buffer the data like the docuemntation states and only write on the writable callback ?
		
#if LWS_SEND_BUFFER_PRE_PADDING == 0 && LWS_SEND_BUFFER_POST_PADDING == 0
		int retval = lws_write(socket->wsi, buf, bufsize, LWS_WRITE_BINARY);
#else
		// libwebsocket requires the caller to allocate the frame prefix/suffix storage.
		std::vector<unsigned char> sendbuf(LWS_SEND_BUFFER_PRE_PADDING + bufsize + LWS_SEND_BUFFER_POST_PADDING, 0);
		memcpy(&sendbuf[LWS_SEND_BUFFER_PRE_PADDING], buf, bufsize);

		int retval = lws_write(socket->wsi, &sendbuf[LWS_SEND_BUFFER_PRE_PADDING], bufsize, LWS_WRITE_BINARY);
#endif
		
		// mark it non-writable and tell websocket to inform us when it's writable again
		//connection->writable = false;
		if( retval > 0 ) {
			lws_callback_on_writable(socket->wsi);
		}

		return retval;

	} else if( socket->webrtc && socket->webrtc_channel ) {
		return libwebrtc_write( socket->webrtc_channel, buf, bufsize );
	}

	// bad/disconnected socket
	return -1;
}

void internal_request_writable(internal_socket_t* socket)
{
	if (socket != NULL && socket->wsi != NULL)
		lws_callback_on_writable(socket->wsi);
}

int internal_websocket_message_complete(internal_socket_t* socket)
{
#ifndef EMSCRIPTEN
	if (socket != NULL && socket->wsi != NULL) {
		return lws_remaining_packet_payload(socket->wsi) == 0 &&
			lws_is_final_fragment(socket->wsi);
	}
#endif
	return 1;
}

int internal_portal_write(internal_socket_t *socket, const void *data, unsigned int length) {
#ifndef EMSCRIPTEN
    const auto *api = libwebrtc_portal_api();
    auto *channel = socket ? socket->webrtc_channel.load() : nullptr;
    return channel && api ? api->write(channel, data, length) : -1;
#else
    return -1;
#endif
}
int internal_portal_reclaim(internal_socket_t *socket) {
#ifndef EMSCRIPTEN
    const auto *api = libwebrtc_portal_api();
    return socket && socket->webrtc && api ? api->reclaim(socket->webrtc) : 0;
#else
    return 0;
#endif
}
unsigned long long internal_portal_buffered(internal_context_t *ctx) {
#ifndef EMSCRIPTEN
    const auto *api = libwebrtc_portal_api();
    return ctx && ctx->webrtc && api ? api->buffered(ctx->webrtc) : 0;
#else
    return 0;
#endif
}
