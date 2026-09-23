#ifdef EMSCRIPTEN

#include "libwebrtc.h"

#include <emscripten.h>

#define USE_STUN

struct libwebrtc_context {
	lwrtc_callback_function callback;
};

extern "C"
int EMSCRIPTEN_KEEPALIVE libwebrtc_helper(struct libwebrtc_context *context, struct libwebrtc_connection *connection, struct libwebrtc_data_channel* channel,
								  enum libwebrtc_callback_reasons reason, void *user, void *in, int len) {
	return context->callback( context, connection, channel, reason, user, in, len );
}

struct libwebrtc_context* libwebrtc_create_context( lwrtc_callback_function callback ) {
	libwebrtc_context* ctx = new libwebrtc_context();
	ctx->callback = callback;

	bool supported = EM_ASM_INT({
		var libwebrtc = {};

		if( ! self.RTCPeerConnection || ! self.RTCIceCandidate || ! self.RTCSessionDescription )
			return 0;

		var ctx = $0;
		libwebrtc.connections = new Map();
		libwebrtc.channels = new Map();
		libwebrtc.nextConnectionId = 1;
		libwebrtc.nextChannelId = 1;
		libwebrtc.next_id = function(counter) {
			var next = this[counter];
			if (!Number.isSafeInteger(next) || next < 1 || next > 0x7fffffff) {
				return 0;
			}
			this[counter] = next + 1;
			return next;
		};
		libwebrtc.on_event = Module.cwrap('libwebrtc_helper', 'number', ['number', 'number', 'number', 'number', 'number', 'number', 'number']);
		libwebrtc.options = {};

		libwebrtc.create = function() {
			var connectionId = this.next_id("nextConnectionId");
			if (!connectionId) {
				return 0;
			}

			if (window.netConfig && window.netConfig.debug) {
				console.log("[DEBUG] RTCPeerConnection created");
			}
			var connection = new RTCPeerConnection(this.options,null);
			connection.trickle = true;

			connection.destroy = this.destroy;

			// Keep bridge ownership explicit; callback receivers are not native socket state.
			connection.ondatachannel = function(event) {
				libwebrtc.on_datachannel(connection, event);
			};
			connection.onicecandidate = this.on_candidate.bind(connection);
			connection.onsignalingstatechange = this.on_signalstatechange.bind(connection);
			connection.oniceconnectionstatechange = this.on_icestatechange.bind(connection);

			connection.id = connectionId;

			this.connections.set( connection.id, connection );

			return connection;
		};
		libwebrtc.create_channel = function(connection, name) {
			var channelId = this.next_id("nextChannelId");
			if (!channelId) {
				return 0;
			}

			var ordered = window.netConfig ? window.netConfig.ordered == true : false;
			if (window.netConfig && window.netConfig.debug) {
				console.log("[DEBUG] created channel", name, "ordered", ordered);
			}
			var channel = connection.createDataChannel( name, { ordered: ordered, maxRetransmits: ordered ? null : 0 } );
			channel.parent = connection;
			// use the parents data initially
			channel.user_data = connection.user_data;
			channel.binaryType = 'arraybuffer';

			channel.onopen = libwebrtc.on_channel_connected.bind(channel);
			channel.onclose = libwebrtc.on_channel_close.bind(channel);
			channel.onmessage = libwebrtc.on_channel_message.bind(channel);
			channel.onerror = libwebrtc.on_channel_error.bind(channel);

			channel._id = channelId;

			libwebrtc.channels.set( channel._id, channel);

			return channel;
		};

		libwebrtc.on_sdp = function(){
			if( ! this.trickle && this.iceGatheringState != "complete" ) {
				return;
			}

			var sdp = this.localDescription.sdp;
			var stack = stackSave();
			// local description //
			const array  = intArrayFromString(sdp);
			const buffer = stackAlloc(array.length);
			Module.HEAPU8.set(array, buffer);
			libwebrtc.on_event( ctx, this.id, 0, 1, this.user_data, buffer, sdp.length);
			stackRestore(stack);
		};
		libwebrtc.on_candidate = function(event){
			if( !event ) {
				return;
			}

			if( this.iceConnectionState === 'completed') {
				Module.out("ignoring ice, were not trying to connect: " + this.iceConnectionState);
				return;
			}

			if( !event.candidate ) {
				Module.out("no more candidates: " + this.iceGatheringState );
				if( ! this.trickle ) {
					libwebrtc.on_sdp.call(this);
				}
				return;
			}

			Module.out("ice candidate " + event.candidate.candidate + " -- " + this.iceGatheringState);

			if( this.trickle ) {
				var stack = stackSave();
				// ice_candidate //
				const array = intArrayFromString(event.candidate.candidate);
				const buffer = stackAlloc(array.length);
				Module.HEAPU8.set(array, buffer);
				libwebrtc.on_event( ctx, this.id, 0, 2, this.user_data, buffer, event.candidate.candidate.length);
				stackRestore(stack);
			}
		};
		libwebrtc.on_signalstatechange = function(event){
			Module.out("signalingState: "+ this.signalingState);
		};
		libwebrtc.on_icestatechange = function(event){
			Module.out( "icestate: " + this.iceConnectionState + " / iceGatheringState: " + this.iceGatheringState);
			if( this.iceConnectionState == 'failed' || this.iceConnectionState == 'disconnected'  ) {
				this.close();
			} else if( this.iceConnectionState == 'closed' ) {
				libwebrtc.on_disconnected.call(this,event);
			} else if( this.iceConnectionState == 'completed' ) {
				// connected //
				libwebrtc.on_event( ctx, this.id, 0, 3, this.user_data, 0, 0);
			}

			if ( (this.iceConnectionState === "connected" || this.iceConnectionState === "completed") && window.top ) {
				this.getStats().then((report) => {
					const stats = [];

					report.forEach(stat => {
					  stats.push({ ...stat });
					});

					window.top.postMessage(
					  { event: "mp.room.rtcstats", stats },
					  "*"
					);
				});
			}
		};
		libwebrtc.on_disconnected = function(event){
			var stack = stackSave();
			// disconnected //
			libwebrtc.on_event( ctx, this.id, 0, 4, this.user_data, 0, 0);
			stackRestore(stack);
			this.destroy();
		};
		libwebrtc.on_datachannel = function(connection, event){
			Module.out("datachannel");
			var channel = event && event.channel;
			var socket = connection && connection.user_data;

			if (!connection || !channel || socket == null || socket === 0) {
				if (channel && channel.readyState !== 'closed') {
					channel.close();
				}
				throw new Error("HumbleNet: incoming data channel has no native socket context");
			}

			channel.parent = connection;
			// use the parents data initially
			channel.user_data = socket;
			channel.binaryType = 'arraybuffer';

			var channelId = libwebrtc.next_id("nextChannelId");
			if (!channelId) {
				if (channel.readyState !== 'closed') {
					channel.close();
				}
				return;
			}

			channel.onopen = libwebrtc.on_channel_accept.bind(channel);
			channel.onclose = libwebrtc.on_channel_close.bind(channel);
			channel.onmessage = libwebrtc.on_channel_message.bind(channel);
			channel.onerror = libwebrtc.on_channel_error.bind(channel);

			channel._id = channelId;

			libwebrtc.channels.set( channel._id, channel);
		};
		libwebrtc.on_channel_accept = function(event){
			Module.out("accept");
			var stack = stackSave();
			// channel accepted //
			const array = intArrayFromString(this.label);
			const buffer = stackAlloc(array.length);
			Module.HEAPU8.set(array, buffer);
			libwebrtc.on_event(ctx, this.parent.id, this._id, 5, this.user_data, buffer, this.label.length);
			stackRestore(stack);
		};
		libwebrtc.on_channel_connected = function(event){
			Module.out("connect");
			var stack = stackSave();
			// channel connected //
			const array = intArrayFromString(this.label);
			const buffer = stackAlloc(array.length);
			Module.HEAPU8.set(array, buffer);
			libwebrtc.on_event(ctx, this.parent.id, this._id, 6, this.user_data, buffer, this.label.length);
			stackRestore(stack);
		};
		libwebrtc.on_channel_message = function(event){
			var stack = stackSave();
			var len = event.data.byteLength;
			var ptr = stackAlloc(len);

//            Module.out("Data: " + len );
			var data = new Uint8Array( event.data );
			Module.HEAPU8.set(data, ptr);

			// channel data //
			libwebrtc.on_event( ctx, this.parent.id, this._id, 7, this.user_data, ptr, len);
			stackRestore(stack);
		};
		libwebrtc.on_channel_error = function(event){
			Module.out("Got channel error: " + event);
			this.close();
		};
		libwebrtc.on_channel_close = function(event){
			var channel = this;
			var channelId = channel._id;
			var parent = channel.parent;
			var parentId = parent ? parent.id : 0;
			var userData = channel.user_data;

			channel.onopen = undefined;
			channel.onclose = undefined;
			channel.onmessage = undefined;
			channel.onerror = undefined;
			channel.close();
			libwebrtc.channels.delete(channelId);

			var stack = stackSave();
			// close channel //
			libwebrtc.on_event(ctx, parentId, channelId, 8, userData, 0, 0);
			stackRestore(stack);
		};
		libwebrtc.destroy = function() {
			var connection = this;
			if (connection.destroyed) {
				return;
			}
			connection.destroyed = true;
			var connectionId = connection.id;
			var userData = connection.user_data;

			connection.ondatachannel = undefined;
			connection.onicecandidate = undefined;
			connection.onsignalingstatechange = undefined;
			connection.oniceconnectionstatechange = undefined;
			libwebrtc.channels.forEach(function(channel, channelId) {
				if (channel.parent !== connection) {
					return;
				}

				channel.onopen = undefined;
				channel.onclose = undefined;
				channel.onmessage = undefined;
				channel.onerror = undefined;
				channel.parent = undefined;
				channel.user_data = undefined;
				channel.close();
				libwebrtc.channels.delete(channelId);
			});
			connection.close();
			libwebrtc.connections.delete(connectionId);

			// destroy (connection) //
			libwebrtc.on_event(ctx, connectionId, 0, 10, userData, 0, 0);
			Module.out("Destroy webrtc: " + connectionId );
		};


		Module.__libwebrtc = libwebrtc;

		return 1;
	}, ctx);

	if( !supported ) {
		delete ctx;
		return nullptr;
	}

	return ctx;
}

void libwebrtc_destroy_context(struct libwebrtc_context* ctx)
{
	delete ctx;
}

void libwebrtc_set_ice_servers(struct libwebrtc_context* ctx, const struct libwebrtc_ice_server* servers, int count)
{
	EM_ASM({
		Module.__libwebrtc.options.iceServers = [];
	});

	for(int i = 0; i < count; ++i) {
		const struct libwebrtc_ice_server* server = servers + i;
		EM_ASM({
			const type = HEAP32[$0 >> 2];
			const urlPtr = HEAPU32[($0 + 4) >> 2];
			const usernamePtr = HEAPU32[($0 + 8) >> 2];
			const passwordPtr = HEAPU32[($0 + 12) >> 2];
			const url = urlPtr ? UTF8ToString(urlPtr) : "";
			const username = usernamePtr ? UTF8ToString(usernamePtr) : "";
			const password = passwordPtr ? UTF8ToString(passwordPtr) : "";
			if (!url) {
				return;
			}

			if (type === 1) {
				Module.__libwebrtc.options.iceServers.push({
					urls: url.startsWith("stun:") || url.startsWith("stuns:") ? url : "stun:" + url
				});
				return;
			}

			const iceServer = {};
			if (username.length > 0 && password.length > 0) {
				iceServer.username = username;
				iceServer.credential = password;
			}

			if (url.startsWith("turn:") || url.startsWith("turns:")) {
				iceServer.urls = url;
				Module.__libwebrtc.options.iceServers.push(iceServer);
			} else if (url.endsWith("5349")) {
				iceServer.urls = "turns:" + url;
				Module.__libwebrtc.options.iceServers.push(iceServer);
			} else {
				Module.__libwebrtc.options.iceServers.push({
					urls: "turn:" + url + "?transport=udp",
					...iceServer
				});
				Module.__libwebrtc.options.iceServers.push({
					urls: "turn:" + url + "?transport=tcp",
					...iceServer
				});
			}
		}, server);
	}
}

struct libwebrtc_connection* libwebrtc_create_connection_extended(struct libwebrtc_context* ctx, void* user_data) {
	return (struct libwebrtc_connection*)EM_ASM_INT({
		var connection = Module.__libwebrtc.create();
		if( ! connection ) {
			return 0;
		}
		connection.user_data = $0;
		return connection.id;
	}, user_data);
}

void libwebrtc_set_user_data(struct libwebrtc_connection* connection, void* user_data ) {
	EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return;
		}
		connection.user_data = $1;
	}, connection, user_data);
}

int libwebrtc_create_offer( struct libwebrtc_connection* connection ) {
	return EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return 0;
		}

		// in order to create and offer at least one stream must have been added.
		// simply create one, then drop it.
		connection.default_channel = Module.__libwebrtc.create_channel( connection,"default");

		connection.createOffer({})
			.then(function(offer){
				connection.setLocalDescription( new RTCSessionDescription( offer ) )
					.then( function() {
						Module.__libwebrtc.on_sdp.call( connection );
					}).catch(function(error){
						alert( "setLocalDescription(create): " + error );
					});
			}).catch(function(error){
				alert("createOffer: " + error);
			});

		return 1;
	}, connection, 0);
}

int libwebrtc_set_offer( struct libwebrtc_connection* connection, const char* sdp ) {
	return EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return 0;
		}

		var offer = {};
		offer.type = 'offer';
		offer.sdp = UTF8ToString( $1 );

		connection.setRemoteDescription( new RTCSessionDescription( offer ) )
			.then(function() {
				connection.createAnswer()
					.then(function(offer){
						connection.setLocalDescription( new RTCSessionDescription( offer ) )
							.then( function() {
								Module.__libwebrtc.on_sdp.call( connection );
							}).catch(function(error){
								alert( "setLocalDescription(answer): " + error );
							});
					}).catch(function(error){
						alert("createAnswer: " + error);
				});
			}).catch(function(error){
				alert("setRemoteDescriptor(answer): " + error );
			});
		return 1;
	}, connection, sdp );
}

int libwebrtc_set_answer( struct libwebrtc_connection* connection, const char* sdp ) {
	return EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return 0;
		}

		var offer = {};
		offer.type = 'answer';
		offer.sdp = UTF8ToString( $1 );

		connection.setRemoteDescription( new RTCSessionDescription( offer ) )
			.then( function() {
				// nothing, as this is the answer to our offer
			}).catch(function(error){
				alert("setRemoteDescriptor(answer): " + error );
			});
		return 1;
	}, connection, sdp );
}

int libwebrtc_add_ice_candidate( struct libwebrtc_connection* connection, const char* candidate ) {
	return EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return 0;
		}

		var options = {};
		options.candidate = UTF8ToString($1);
		options.sdpMLineIndex = 0;

		if( connection.iceConnectionState == 'checking' || connection.iceConnectionState == 'connected'
		   // FF workaround
		   || connection.iceConnectionState == 'new') {
			Module.out( "AddIce: " + options.candidate );
			connection.addIceCandidate( new RTCIceCandidate( options ) ).catch((e) => {
				console.error("Failed to add ice candidate: " + e);
			});
		} else {
			Module.out( "Not negotiating (" + connection.iceConnectionState + "), ignored candidate: " + options.candidate );
		}

	}, connection, candidate );
}

struct libwebrtc_data_channel* libwebrtc_create_channel( struct libwebrtc_connection* connection, const char* name ) {
	return (struct libwebrtc_data_channel*)EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return 0;
		}

		var channel;

		if( connection.default_channel ){
			channel = connection.default_channel;
			connection.default_channel = 0;
		}else{
			channel = Module.__libwebrtc.create_channel( connection, UTF8ToString($1) );
		}
		if( ! channel ) {
			return 0;
		}

		return channel._id;

	}, connection, name );
}

int libwebrtc_write( struct libwebrtc_data_channel* channel, const void* data, int len ) {
	return EM_ASM_INT({
		var channel = Module.__libwebrtc.channels.get($0);
		if( ! channel ) {
			return -1;
		}

		// alloc a Uint8Array backed by the incoming data.
		var data_in = new Uint8Array(Module.HEAPU8.buffer, $1, $2 );
		// allow the dest array
		var data = new Uint8Array($2);
		// set the dest from the src
		data.set(data_in);

		channel.send( data );
		return $2;

	}, channel, data, len );
}

void libwebrtc_close_connection( struct libwebrtc_connection* channel ) {
	EM_ASM_INT({
		var connection = Module.__libwebrtc.connections.get($0);
		if( ! connection ) {
			return -1;
		}

		connection.destroy();

	}, channel );
}

void libwebrtc_close_channel( struct libwebrtc_data_channel* channel ) {
	EM_ASM_INT({
		var channel = Module.__libwebrtc.channels.get($0);
		if( ! channel ) {
			return -1;
		}

		var channelId = channel._id;
		channel.onopen = undefined;
		channel.onclose = undefined;
		channel.onmessage = undefined;
		channel.onerror = undefined;
		channel.close();
		Module.__libwebrtc.channels.delete(channelId);
	}, channel );
}


#endif
