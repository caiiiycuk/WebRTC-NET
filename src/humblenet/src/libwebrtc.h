#ifndef LIBWEBRTC_H
#define LIBWEBRTC_H

// shared ABI to Microstack WebRTC and Browsers WebRTC.

#ifdef __cplusplus
extern "C" {
#endif

enum libwebrtc_callback_reasons {
    LWRTC_CALLBACK_LOCAL_DESCRIPTION = 1,
    LWRTC_CALLBACK_ICE_CANDIDATE = 2,
    LWRTC_CALLBACK_ESTABLISHED = 3,
    LWRTC_CALLBACK_DISCONNECTED = 4,
    
    LWRTC_CALLBACK_CHANNEL_ACCEPTED = 5,
    LWRTC_CALLBACK_CHANNEL_CONNECTED = 6,
    LWRTC_CALLBACK_CHANNEL_RECEIVE = 7,
    LWRTC_CALLBACK_CHANNEL_CLOSED = 8,
    
    LWRTC_CALLBACK_ERROR = 9,
    
    LWRTC_CALLBACK_DESTROY = 10
};


struct libwebrtc_context;

// this represents the connection to the peer.
struct libwebrtc_connection;

// this represents an active data stream to the peer.
struct libwebrtc_data_channel;

enum libwebrtc_ice_server_type {
	LIBWEBRTC_ICE_SERVER_STUN = 1,
	LIBWEBRTC_ICE_SERVER_TURN = 2,
};

struct libwebrtc_ice_server {
	enum libwebrtc_ice_server_type type;
	const char* url;
	const char* username;
	const char* password;
};
    
typedef int (*lwrtc_callback_function)(struct libwebrtc_context *context,
    struct libwebrtc_connection *connection, struct libwebrtc_data_channel* channel,
			 enum libwebrtc_callback_reasons reason, void *user,
    void *in, int len);
    
struct libwebrtc_context* libwebrtc_create_context( lwrtc_callback_function );
void libwebrtc_destroy_context( struct libwebrtc_context* );

void libwebrtc_set_ice_servers( struct libwebrtc_context* ctx, const struct libwebrtc_ice_server* servers, int count);
    
struct libwebrtc_connection* libwebrtc_create_connection_extended( struct libwebrtc_context*, void* user_data );
struct libwebrtc_data_channel* libwebrtc_create_channel( struct libwebrtc_connection* conn, const char* name );

int libwebrtc_create_offer( struct libwebrtc_connection* );
int libwebrtc_set_offer( struct libwebrtc_connection* , const char* sdp );
int libwebrtc_set_answer( struct libwebrtc_connection*, const char* sdp );
int libwebrtc_add_ice_candidate( struct libwebrtc_connection*, const char* candidate );
    
int libwebrtc_write( struct libwebrtc_data_channel*, const void*, int len );
    
void libwebrtc_close_channel( struct libwebrtc_data_channel* );
void libwebrtc_close_connection( struct libwebrtc_connection* );
    
#ifndef EMSCRIPTEN
#include <stdint.h>
struct lwrtc_portal_api {
    uint32_t version;
    uint64_t capabilities;
    int (*enable)(struct libwebrtc_context *, void (*wake)(void *), void *user);
    int (*write)(struct libwebrtc_data_channel *, const void *, int);
    int (*reclaim)(struct libwebrtc_connection *);
    uint64_t (*buffered)(struct libwebrtc_context *);
};
const struct lwrtc_portal_api *libwebrtc_portal_api(void);
#endif
#ifdef __cplusplus
}
#endif

#endif // LIBWEBRTC_H
