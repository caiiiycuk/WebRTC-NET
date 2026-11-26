#ifndef WEBRTC_PORTAL_TX_SNAPSHOT_V2_H
#define WEBRTC_PORTAL_TX_SNAPSHOT_V2_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct webrtc_portal_tx_snapshot_v2 {
  uint64_t owned_payload_bytes;
  uint64_t owned_records;
  uint64_t reservation_bytes;
  uint64_t reservation_records;
} webrtc_portal_tx_snapshot_v2;
// state: 1 valid, 0 unavailable/overflow (fail closed), 2 socket destroyed.
// Runs on the SDK network thread after the outer SCTP operation returns.
// Store the snapshot and wake; never send/close/reenter the SDK in this callback.
typedef void (*webrtc_portal_tx_notify_v2)(void*, const webrtc_portal_tx_snapshot_v2*, int state);
// Call only on the actual SDK network thread, with DataChannelInterface*.
// A null callback queries without binding. Binding must survive until state2.
int webrtc_portal_tx_observe_v2(void* channel, webrtc_portal_tx_notify_v2 notify,
                               void* user, webrtc_portal_tx_snapshot_v2* out);
// Atomic with actual send on the SDK network thread. Ceilings are absolute
// local limits, reduced by the provider's already-reserved global allowance.
// 1=actual SCTP acceptance,0=no-send backpressure,-1=closed/invalid/unsupported.
int webrtc_portal_tx_try_send_v2(void* channel, const void* data, int length,
                               uint64_t reservation_byte_ceiling,
                               uint64_t reservation_record_ceiling);
// Passive observation prepays pending DCEP; activate only after reserving
// the initial potential in the provider's peer/global account.1/0/-1 as documented.
int webrtc_portal_tx_activate_v2(void* channel, uint64_t reservation_byte_ceiling,
                               uint64_t reservation_record_ceiling);
#ifdef __cplusplus
}
#endif
#endif
