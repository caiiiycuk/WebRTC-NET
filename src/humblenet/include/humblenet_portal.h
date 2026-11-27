#ifndef HUMBLENET_PORTAL_H
#define HUMBLENET_PORTAL_H
#include "humblenet.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define HUMBLENET_PORTAL_ABI 1u
#define HUMBLENET_PORTAL_CAPS_V1 UINT64_C(0x1f)
/* Cap bits: ingress, admission/retirement, eventfd/fairness, send, lifetime.
 * They do not claim a total RSS or DTLS/SCTP reassembly cap. */
enum humblenet_portal_send_result {
    HUMBLENET_PORTAL_CLOSED = -1,
    HUMBLENET_PORTAL_BACKPRESSURE = 0,
    HUMBLENET_PORTAL_ACCEPTED = 1
};
enum humblenet_portal_event_type {
    HUMBLENET_PORTAL_OPEN = 1,
    HUMBLENET_PORTAL_DATA = 2,
    HUMBLENET_PORTAL_CLOSE = 3,
    HUMBLENET_PORTAL_RECLAIM = 4
};
typedef struct humblenet_portal_options {
    uint32_t abi_version;
    uint32_t max_connections; /* 1..160, including unreclaimed closing */
    uint32_t max_pending;     /* 1..8, <=max_connections; through established */
    const char *alias;        /* full room.q2portal.*; copied at enable */
} humblenet_portal_options;
typedef struct humblenet_portal_event {
    uint32_t type;
    PeerId peer;
    uint64_t lifetime;
    uint64_t received_monotonic_ns; /* callback timestamp before enqueue */
    uint32_t length;               /* DATA only; no prefix delivery */
} humblenet_portal_event;
typedef struct humblenet_portal_stats {
    uint64_t offers_refused, ingress_oversize, ingress_pressure;
    uint64_t send_accepted, send_backpressure, send_closed;
    uint64_t receive_packets;
    uint64_t ingress_owned_bytes, egress_buffered_bytes;
    uint32_t reservations, pending, closing, retained_records;
} humblenet_portal_stats;
HUMBLENET_API uint64_t HUMBLENET_CALL humblenet_portal_capabilities(void);
/* Enable once before p2p_init. Fails without the optional native backend API.
 * Fixed V1 caps: 1400 raw; ingress32 records per peer; egress64KiB
 * peer/1MiB global payload potential;4096/65536 peer/global record units;
 * retained records512. Actual retained payload and reservation potential differ.
 * Initial admission is closed. No disable/reconfigure while live. */
HUMBLENET_API ha_bool HUMBLENET_CALL humblenet_portal_enable(const humblenet_portal_options *options);
/* Admission also requires live signaling and confirmed ownership of options.alias. */
HUMBLENET_API void HUMBLENET_CALL humblenet_portal_set_admission(ha_bool enabled);
/* Borrowed fd, poll POLLIN only. Never read/write/close it yourself. */
HUMBLENET_API int HUMBLENET_CALL humblenet_portal_event_fd(void);
/* Drains wake and services timers/close reclaim under producer synchronization;
 * rearms unread work. Call once per dispatch, then recv <=128 events; service
 * other fds/timers before repeating. New arrivals cannot be lost at reset. */
HUMBLENET_API void HUMBLENET_CALL humblenet_portal_service(void);
/* 1 event,0 none,-1 invalid buffer. DATA requires capacity>=1400. Round-robin
 * peers; close precedes buffered data. No allocation or partial read. */
HUMBLENET_API int HUMBLENET_CALL humblenet_portal_recv(void *buffer, uint32_t capacity, humblenet_portal_event *event);
/* Exact existing lifetime only; never initiates a new connection. Synchronous
 * whole-message backend handoff. Invalid payload/length returns CLOSED without
 * sending; ordinary pressure never closes. Accepted is not remote receipt. */
HUMBLENET_API int HUMBLENET_CALL humblenet_portal_send(PeerId peer, uint64_t lifetime, const void *data, uint32_t length);
/* Mark first valid getchallenge: provider15s offer/startup deadline stops.
 * The daemon owns subsequent absolute admission/idle deadlines. */
HUMBLENET_API ha_bool HUMBLENET_CALL humblenet_portal_started(PeerId peer, uint64_t lifetime);
/* First valid post-acceptance sequenced packet only. Frees pending once, not
 * total. OPEN and started do not free pending; never-established closing holds
 * pending through safe reclaim. Duplicate/stale acknowledgements return false. */
HUMBLENET_API ha_bool HUMBLENET_CALL humblenet_portal_established(PeerId peer, uint64_t lifetime);
/* 0 absent/retired,1 pending,2 connected,3 logically closed awaiting reclaim.
 * CLOSE is acknowledged by release; RECLAIM means backend callbacks and owned
 * Connection reclaimed and allocation reservation released. */
HUMBLENET_API int HUMBLENET_CALL humblenet_portal_connection_state(PeerId peer, uint64_t lifetime);
HUMBLENET_API ha_bool HUMBLENET_CALL humblenet_portal_disconnect(PeerId peer, uint64_t lifetime);
HUMBLENET_API ha_bool HUMBLENET_CALL humblenet_portal_release(PeerId peer, uint64_t lifetime);
HUMBLENET_API void HUMBLENET_CALL humblenet_portal_get_stats(humblenet_portal_stats *stats);
/* Existing humblenet_dz_signaling_state/alias_state/get_my_peer_id are retained;
 * alias_state2 means confirmed local owner, not merely registration queued. */
#ifdef __cplusplus
}
#endif
#endif
