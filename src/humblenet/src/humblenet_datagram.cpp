#include "humblenet_datagram.h"

#include "humblenet_p2p_internal.h"

// TODO : If this had access to the internals of Connection it could be further optimized.

#include <map>
#include <vector>
#include <stdlib.h>
#include <cstring>
#include <stdio.h>
#include <algorithm>

#ifdef EMSCRIPTEN
#include <emscripten.h>
#endif

struct datagram_connection {
	Connection*			conn;			// established connection.
	PeerId				peer;			// "address"

	// packets in and out
	std::deque<std::vector<char>>	buf_in;
	std::deque<std::vector<char>>	buf_out;

	
	datagram_connection( Connection* conn, bool outgoing )
	:conn( conn )
	,peer( humblenet_connection_get_peer_id( conn ) )
	{
	}
};

typedef std::map<Connection*, datagram_connection> ConnectionMap;

static ConnectionMap	connections;
static bool				queuedPackets = false;

static int datagram_get_message( datagram_connection& conn, void* buffer, size_t length, int flags, uint8_t channel ) {

	assert(channel == 0);
	if (conn.buf_in.empty())
		return 0;
	size_t len = std::min(conn.buf_in.front().size(), length);
	if( flags & HUMBLENET_MSG_PEEK )
		return len;
	memcpy(buffer, &conn.buf_in.front()[0], len);
	conn.buf_in.pop_front();
	return len;
}

static void datagram_flush( datagram_connection& dg, const char* reason ) {
	if( ! dg.buf_out.empty() )
	{
		if( ! humblenet_connection_is_writable( dg.conn ) ) {
			LOG("Waiting(%s) %zu packets to  %p\n", reason, dg.buf_out.size(), dg.conn );
			return;
		}

		while( ! dg.buf_out.empty() ) {
			int ret = humblenet_connection_write( dg.conn, dg.buf_out.front().data(), dg.buf_out.front().size() );
			dg.buf_out.pop_front();
			if( ret < 0 ) {
#ifdef EMSCRIPTEN
				EM_ASM((
					if (Module.onNetworkError) {
						Module.onNetworkError($0);
					}
				), dg.peer);
#else
    // Native owners consume the resulting closed-route event before their final send.
    humblenet_connection_set_closed(dg.conn);
    dg.buf_out.clear();
#endif
				humblenet_clear_error();
			}
		}
	}
}

int humblenet_datagram_send( const void* message, size_t length, int flags, Connection* conn, uint8_t channel )
{
	// if were still connecting, we can't write yet
	// TODO: Should we queue that data up?
	switch( humblenet_connection_status( conn ) ) {
		case HUMBLENET_CONNECTION_CONNECTING: {
			// If send using buffered, then allow messages to be queued till were connected.
			if( flags & HUMBLENET_MSG_BUFFERED )
				break;
			
			humblenet_set_error("Connection is still being established");
			return 0;
		}
		case HUMBLENET_CONNECTION_CLOSED: {
			humblenet_set_error("Connection is closed");
			// should this wipe the state ?
			return -1;
		}
		default: {
			break;
		}
	}

	ConnectionMap::iterator it = connections.find( conn );
	if( it == connections.end() ) {
		connections.insert( ConnectionMap::value_type( conn, datagram_connection( conn, false ) ) );
		it = connections.find( conn );
	}

	datagram_connection& dg = it->second;

	dg.buf_out.emplace_back(reinterpret_cast<const char*>( message ), reinterpret_cast<const char*>( message ) + length);

	// if( !( flags & HUMBLENET_MSG_BUFFERED ) ) {
		datagram_flush( dg, "no-delay" );
	// } else if( dg.buf_out.size() > 1024 ) {
	// 	datagram_flush( dg, "max-length" );
	// } else {
	// 	queuedPackets = true;
	// 	//if( dg.queued > 1 )
	// 	//    LOG("Queued %d packets (%zu bytes) for  %p\n", dg.queued, dg.buf_out.size(), dg.conn );
	// }
	return length;
}

int humblenet_datagram_recv( void* buffer, size_t length, int flags, Connection** fromconn, uint8_t channel )
{
	assert(channel == 0);
	// flush queued packets
	if( queuedPackets ) {
		for( ConnectionMap::iterator it = connections.begin(); it != connections.end(); ++it ) {
			datagram_flush( it->second, "auto" );
		}
		queuedPackets = false;
	}

#ifndef EMSCRIPTEN
 // Deliver the old lifetime's close before any replacement's buffered packet.
 for (auto it = connections.begin(); it != connections.end(); ++it) {
  if (humblenet_connection_status(it->first) == HUMBLENET_CONNECTION_CLOSED) {
   *fromconn = it->first;
   connections.erase(it);
   return -1;
  }
 }
#endif

	// first we drain all existing packets.
	for( ConnectionMap::iterator it = connections.begin(); it != connections.end(); ++it ) {
		int ret = datagram_get_message( it->second, buffer, length, flags, channel );
		if( ret > 0 ) {
			*fromconn = it->second.conn;
			return ret;
		}
	}

	// next check for incoming data on existing connections...
	while(true) {
		Connection* conn = humblenet_poll_all(0);
		if( conn == NULL )
			break;

		PeerId peer = humblenet_connection_get_peer_id( conn );

		ConnectionMap::iterator it = connections.find( conn );
		if( it == connections.end() ) {
			// hmm connection not cleaned up properly...
			LOG("received data from peer %u, but we have no datagram_connection for them\n", peer );
			connections.insert( ConnectionMap::value_type( conn, datagram_connection( conn, false ) ) );
			it = connections.find( conn );
		}

		// read whatever we can...
		uint8_t internalBuffer[1500];
		int retval = humblenet_connection_read(conn, internalBuffer, sizeof(internalBuffer));
		if( retval < 0 ) {
			connections.erase( it );
			LOG("read from peer %u(%p) failed with %s\n", peer, conn, humblenet_get_error() );
			humblenet_clear_error();
			if( humblenet_connection_status( conn ) == HUMBLENET_CONNECTION_CLOSED ) {
				*fromconn = conn;
				return -1;
			}
			continue;
		} else {
			it->second.buf_in.emplace_back(internalBuffer, internalBuffer + retval);
			retval = datagram_get_message( it->second, buffer, length, flags, channel );
			// LOG("after datagram_get_message, read %d bytes packet from peer %u(%p), [0]=%d\n", retval, peer, conn, buffer[0] );
			if( retval > 0 ) {
				*fromconn = it->second.conn;
				return retval;
			}
		}
	}

	// no existing connections have a packet ready, see if we have any new connections
	while( true ) {
		Connection* conn = humblenet_connection_accept();
		if( conn == NULL )
			break;

		PeerId peer = humblenet_connection_get_peer_id( conn );
		if( peer == 0 ) {
			// Not a peer connection?
			LOG("Accepted connection, but not a peer connection: %p\n", conn);
		   // humblenet_connection_close( conn );
			continue;
		}

		connections.insert( ConnectionMap::value_type( conn, datagram_connection( conn, false ) ) );
	}

	return 0;
}

/*
* See if there is a message waiting on the specified channel
*/
ha_bool humblenet_datagram_select( size_t* length, uint8_t channel ) {
	Connection* conn = NULL;
	int ret = humblenet_datagram_recv( NULL, 0, HUMBLENET_MSG_PEEK, &conn, channel );
	if( ret > 0 )
		*length = ret;
	else
		*length = 0;

	return *length > 0;
}

ha_bool humblenet_datagram_flush() {
	// flush queued packets
	if( queuedPackets ) {
		for( ConnectionMap::iterator it = connections.begin(); it != connections.end(); ++it ) {
			datagram_flush( it->second, "manual" );
		}
		queuedPackets = false;
		return true;
	}
	return false;
}
