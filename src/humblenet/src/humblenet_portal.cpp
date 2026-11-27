#include "humblenet_portal_internal.h"
#include "humblenet_p2p_internal.h"
#include "humblenet_p2p.h"
#include "humblenet_utils.h"
#include "libwebrtc.h"
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#if defined(__linux__) && !defined(EMSCRIPTEN)
#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>

namespace {
struct Packet {
    uint64_t received_ns;
    uint32_t length;
    std::array<char, 1400> bytes;
};
struct Inbox {
    std::array<Packet, 32> packets;
    uint32_t first = 0, count = 0;
};
struct Record {
    Connection *conn = nullptr;
    PeerId peer = 0;
    uint64_t lifetime = 0, deadline_ns = 0, expires_ns = 0;
    bool pending = false, open = false, close = false, acknowledged = false;
    bool destroyed = false, reclaimed = false, allocating = true;
    std::unique_ptr<Inbox> inbox;
};
constexpr uint32_t max_connections = 160;
std::array<Record, 512> records;
constexpr uint64_t max_ingress_bytes = sizeof(records) + max_connections * sizeof(Inbox);
humblenet_portal_stats counters = {};
humblenet_portal_options limits = {};
std::string owned_alias;
bool enabled = false, admission = false, backend_failed = false;
int wake_fd = -1;
size_t cursor = 0;
uint64_t scheduled_ns = 0;
uintptr_t timer_generation = 0;

Record *find(Connection *conn) {
    for (auto &r : records) if (r.conn == conn && conn) return &r;
    return nullptr;
}
Record *find(PeerId peer, uint64_t lifetime) {
    for (auto &r : records) if (r.peer == peer && r.lifetime == lifetime && peer && lifetime) return &r;
    return nullptr;
}
uint64_t ingress_bytes() {
    uint64_t result = sizeof(records);
    for (const auto &r : records) if (r.inbox) result += sizeof(Inbox);
    return result;
}
void prune() {
    const auto now = portal_now_ns();
    for (auto &r : records) {
        if (r.peer && !r.conn && !r.reclaimed && r.expires_ns && now >= r.expires_ns) r = Record();
    }
}
void timer_wake(void *token) {
    HUMBLENET_GUARD();
    if (!enabled || reinterpret_cast<uintptr_t>(token) != timer_generation) return;
    scheduled_ns = 0;
    portal_notify(nullptr);
}
void schedule() {
    uint64_t next = 0;
    for (const auto &r : records) {
        const auto t = r.conn && r.conn->status != HUMBLENET_CONNECTION_CLOSED ? r.deadline_ns :
            (!r.conn && !r.reclaimed ? r.expires_ns : 0);
        if (t && (!next || t < next)) next = t;
    }
    if (!next || (scheduled_ns && scheduled_ns <= next)) return;
    scheduled_ns = next;
    const auto now = portal_now_ns();
    const uint64_t ms = next > now ? (next - now + 999999) / 1000000 : 1;
    humblenet_timer(timer_wake, static_cast<int>(ms), reinterpret_cast<void *>(++timer_generation));
}
bool ready() {
    for (const auto &r : records) {
        if (r.open || r.close || r.reclaimed || (r.inbox && r.inbox->count)) return true;
    }
    return false;
}
}

uint64_t portal_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool portal_enabled() { return enabled; }
bool portal_owns(Connection *conn) { return enabled && find(conn); }
void portal_notify(void *) {
    if (wake_fd < 0) return;
    const uint64_t one = 1;
    ssize_t result;
    do { result = write(wake_fd, &one, sizeof(one)); } while (result < 0 && errno == EINTR);
    // EAGAIN means a wake is already pending; the consumer resets under producer lock.
}
bool portal_blacklisted(PeerId peer) {
    if (!enabled) return false;
    prune();
    for (const auto &r : records) if (r.peer == peer) return true;
    return false;
}
Connection *portal_offer(PeerId peer) {
    prune();
    uint32_t total = 0, pending = 0;
    for (const auto &r : records) { total += r.conn != nullptr; pending += r.pending; }
    if (!admission || !peer || !humbleNetState.myPeerId || !humbleNetState.p2pConn || !humbleNetState.p2pConn->wsi ||
        !humbleNetState.desiredAliases.count(owned_alias) || !humbleNetState.confirmedAliases.count(owned_alias) || portal_blacklisted(peer) ||
        total >= limits.max_connections || pending >= limits.max_pending || ingress_bytes() + sizeof(Inbox) > max_ingress_bytes) {
        ++counters.offers_refused; return nullptr;
    }
    for (auto &r : records) if (!r.peer) {
        r.peer = peer; r.pending = true;
        r.inbox.reset(new Inbox());
        r.conn = new Connection(Incoming);
        r.conn->otherPeer = peer;
        r.lifetime = r.conn->lifetime;
        r.deadline_ns = portal_now_ns() + UINT64_C(15000000000);
        schedule(); portal_notify(nullptr);
        return r.conn;
    }
    ++counters.offers_refused; return nullptr;
}
void portal_bound(Connection *conn) {
    auto *r = find(conn);
    if (!r) return;
    r->allocating = false;
    if (!conn->socket) r->destroyed = true;
    else if (conn->status == HUMBLENET_CONNECTION_CLOSED) {
        auto *socket = conn->socket;
        HUMBLENET_UNGUARD(); internal_close_socket(socket);
    }
    portal_notify(nullptr);
}
void portal_opened(Connection *conn) {
    auto *r = find(conn);
    if (!r || conn->status == HUMBLENET_CONNECTION_CLOSED) return;
    r->open = true;
    portal_notify(nullptr);
}
void portal_closed(Connection *conn) {
    auto *r = find(conn);
    if (!r || conn->status == HUMBLENET_CONNECTION_CLOSED) return;
    conn->status = HUMBLENET_CONNECTION_CLOSED;
    r->open = false; r->close = true;
    r->deadline_ns = 0;
    r->expires_ns = portal_now_ns() + UINT64_C(1000000000);
    r->inbox.reset();
    humbleNetState.pendingDataConnections.erase(conn);
    humbleNetState.pendingNewConnections.erase(conn);
    erase_value(humbleNetState.pendingPeerConnectionsIn, conn);
    erase_value(humbleNetState.pendingPeerConnectionsOut, conn);
    portal_notify(nullptr);
    if (conn->socket) {
        auto *socket = conn->socket;
        HUMBLENET_UNGUARD();
        internal_close_socket(socket);
    } else if (!r->allocating) {
        r->destroyed = true;
    }
}
void portal_destroyed(Connection *conn) {
    auto *r = find(conn);
    if (!r) return;
    conn->socket = nullptr;
    r->destroyed = true;
    if (conn->status != HUMBLENET_CONNECTION_CLOSED) portal_closed(conn);
    portal_notify(nullptr);
}
int portal_ingress(Connection *conn, const void *data, int length) {
    auto *r = find(conn);
    if (!r || conn->status != HUMBLENET_CONNECTION_CONNECTED || !r->inbox) return -1;
    const auto received = portal_now_ns();
    if (!data || length < 1 || length > 1400) { ++counters.ingress_oversize; return 0; }
    auto &q = *r->inbox;
    if (q.count == q.packets.size()) { ++counters.ingress_pressure; return 0; }
    auto &p = q.packets[(q.first + q.count) % q.packets.size()];
    p.received_ns = received; p.length = length;
    std::memcpy(p.bytes.data(), data, length); ++q.count;
    ++counters.receive_packets;
    portal_notify(nullptr); return 0;
}

uint64_t HUMBLENET_CALL humblenet_portal_capabilities() {
    const auto *api = libwebrtc_portal_api();
    return api && api->version == 1 ? api->capabilities : 0;
}
ha_bool HUMBLENET_CALL humblenet_portal_enable(const humblenet_portal_options *o) {
    if (enabled || humblenet_p2p_is_initialized() || !o || o->abi_version != 1 ||
        !o->max_connections || o->max_connections > max_connections || !o->max_pending || o->max_pending > 8 ||
        o->max_pending > o->max_connections || !o->alias || strnlen(o->alias, 64) > 50 ||
        std::strncmp(o->alias, "room.q2portal.", 14) || !o->alias[14] || (humblenet_portal_capabilities() & HUMBLENET_PORTAL_CAPS_V1) != HUMBLENET_PORTAL_CAPS_V1) return false;
    for (const char *p = o->alias + 14; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd < 0) return false;
    limits = *o; owned_alias = o->alias; limits.alias = nullptr;
    enabled = true; admission = false; return true;
}
void HUMBLENET_CALL humblenet_portal_set_admission(ha_bool value) {
    HUMBLENET_GUARD();
    const bool next = enabled && value && !backend_failed;
    if (admission != next) { admission = next; portal_notify(nullptr); }
}
int HUMBLENET_CALL humblenet_portal_event_fd() { return wake_fd; }
void HUMBLENET_CALL humblenet_portal_service() {
    HUMBLENET_GUARD();
    if (!enabled) return;
    uint64_t value; ssize_t result;
    do { result = read(wake_fd, &value, sizeof(value)); } while (result < 0 && errno == EINTR);
    uint64_t buffered;
    { HUMBLENET_UNGUARD(); buffered = internal_portal_buffered(humbleNetState.context); }
    if (buffered == UINT64_MAX) { backend_failed = true; admission = false; }
    const auto now = portal_now_ns();
    for (auto &r : records) {
        if (r.conn && r.deadline_ns && now >= r.deadline_ns) portal_closed(r.conn);
        if (r.conn && r.conn->status == HUMBLENET_CONNECTION_CLOSED && r.conn->socket && !r.destroyed) {
            auto *socket = r.conn->socket;
            HUMBLENET_UNGUARD(); internal_portal_reclaim(socket);
        }
        if (r.conn && !r.allocating && r.destroyed && r.acknowledged && !r.close) {
            humbleNetState.connections.erase(r.conn->socket);
            delete r.conn; r.conn = nullptr; r.pending = false; r.reclaimed = true;
        }
    }
    prune(); schedule();
    if (ready()) portal_notify(nullptr);
}
int HUMBLENET_CALL humblenet_portal_recv(void *buffer, uint32_t capacity, humblenet_portal_event *event) {
    HUMBLENET_GUARD();
    if (!enabled || !event || !buffer || capacity < 1400) return -1;
    *event = {};
    for (size_t i = 0; i < records.size(); ++i) {
        auto &r = records[(cursor + i) % records.size()];
        if (!r.peer) continue;
        uint32_t type = r.close ? HUMBLENET_PORTAL_CLOSE : r.reclaimed ? HUMBLENET_PORTAL_RECLAIM :
            r.open ? HUMBLENET_PORTAL_OPEN : r.inbox && r.inbox->count ? HUMBLENET_PORTAL_DATA : 0;
        if (!type) continue;
        event->type = type; event->peer = r.peer; event->lifetime = r.lifetime;
        event->received_monotonic_ns = portal_now_ns();
        if (type == HUMBLENET_PORTAL_CLOSE) r.close = false;
        else if (type == HUMBLENET_PORTAL_RECLAIM) r.reclaimed = false;
        else if (type == HUMBLENET_PORTAL_OPEN) r.open = false;
        else {
            auto &q = *r.inbox; const auto &p = q.packets[q.first];
            event->received_monotonic_ns = p.received_ns; event->length = p.length;
            std::memcpy(buffer, p.bytes.data(), p.length);
            q.first = (q.first + 1) % q.packets.size(); --q.count;
        }
        cursor = (&r - records.data() + 1) % records.size();
        schedule(); return 1;
    }
    schedule(); return 0;
}
int HUMBLENET_CALL humblenet_portal_send(PeerId peer, uint64_t lifetime, const void *data, uint32_t length) {
    HUMBLENET_GUARD();
    auto *r = find(peer, lifetime);
    int result = HUMBLENET_PORTAL_CLOSED;
    if (enabled && r && r->conn && r->conn->status == HUMBLENET_CONNECTION_CONNECTED && data && length && length <= 1400)
        result = humblenet_connection_portal_write(r->conn, data, length);
    if (result == HUMBLENET_PORTAL_ACCEPTED) ++counters.send_accepted;
    else if (result == HUMBLENET_PORTAL_BACKPRESSURE) ++counters.send_backpressure;
    else ++counters.send_closed;
    return result;
}
ha_bool HUMBLENET_CALL humblenet_portal_started(PeerId peer, uint64_t lifetime) {
    HUMBLENET_GUARD(); auto *r = find(peer, lifetime);
    if (!r || !r->conn || r->conn->status != HUMBLENET_CONNECTION_CONNECTED) return false;
    r->deadline_ns = 0; schedule(); return true;
}
ha_bool HUMBLENET_CALL humblenet_portal_established(PeerId peer, uint64_t lifetime) {
    HUMBLENET_GUARD(); auto *r = find(peer, lifetime);
    if (!r || !r->conn || r->conn->status != HUMBLENET_CONNECTION_CONNECTED || !r->pending) return false;
    r->pending = false; portal_notify(nullptr); return true;
}
int HUMBLENET_CALL humblenet_portal_connection_state(PeerId peer, uint64_t lifetime) {
    HUMBLENET_GUARD(); auto *r = find(peer, lifetime);
    return r && r->conn ? (r->conn->status == HUMBLENET_CONNECTION_CLOSED ? 3 : r->conn->status) : 0;
}
ha_bool HUMBLENET_CALL humblenet_portal_disconnect(PeerId peer, uint64_t lifetime) {
    HUMBLENET_GUARD(); auto *r = find(peer, lifetime);
    if (!r || !r->conn) return false;
    portal_closed(r->conn); return true;
}
ha_bool HUMBLENET_CALL humblenet_portal_release(PeerId peer, uint64_t lifetime) {
    HUMBLENET_GUARD(); auto *r = find(peer, lifetime);
    if (!r || !r->conn || r->conn->status != HUMBLENET_CONNECTION_CLOSED || r->close) return false;
    r->acknowledged = true; portal_notify(nullptr); return true;
}
void HUMBLENET_CALL humblenet_portal_get_stats(humblenet_portal_stats *out) {
    HUMBLENET_GUARD(); if (!out) return;
    *out = counters; out->ingress_owned_bytes = ingress_bytes();
    for (const auto &r : records) {
        out->reservations += r.conn != nullptr; out->pending += r.pending;
        out->closing += r.conn && r.conn->status == HUMBLENET_CONNECTION_CLOSED;
        out->retained_records += r.peer != 0;
    }
    { HUMBLENET_UNGUARD(); out->egress_buffered_bytes = internal_portal_buffered(humbleNetState.context); }
}
void portal_shutdown() {
    admission = false;
    // Context shutdown quiesces callbacks before this is called.
    for (auto &r : records) { if (r.conn) delete r.conn; r = Record(); }
    if (wake_fd >= 0) close(wake_fd);
    wake_fd = -1; enabled = false; backend_failed = false; scheduled_ns = 0; ++timer_generation;
}
#else
bool portal_enabled() { return false; }
bool portal_owns(Connection *) { return false; }
Connection *portal_offer(PeerId) { return nullptr; }
void portal_bound(Connection *) {}
void portal_opened(Connection *) {}
void portal_closed(Connection *) {}
void portal_destroyed(Connection *) {}
int portal_ingress(Connection *, const void *, int) { return -1; }
bool portal_blacklisted(PeerId) { return false; }
void portal_notify(void *) {}
uint64_t portal_now_ns() { return 0; }
void portal_shutdown() {}
uint64_t HUMBLENET_CALL humblenet_portal_capabilities() { return 0; }
ha_bool HUMBLENET_CALL humblenet_portal_enable(const humblenet_portal_options *) { return false; }
void HUMBLENET_CALL humblenet_portal_set_admission(ha_bool) {}
int HUMBLENET_CALL humblenet_portal_event_fd() { return -1; }
void HUMBLENET_CALL humblenet_portal_service() {}
int HUMBLENET_CALL humblenet_portal_recv(void *, uint32_t, humblenet_portal_event *) { return -1; }
int HUMBLENET_CALL humblenet_portal_send(PeerId, uint64_t, const void *, uint32_t) { return -1; }
ha_bool HUMBLENET_CALL humblenet_portal_started(PeerId, uint64_t) { return false; }
ha_bool HUMBLENET_CALL humblenet_portal_established(PeerId, uint64_t) { return false; }
int HUMBLENET_CALL humblenet_portal_connection_state(PeerId, uint64_t) { return 0; }
ha_bool HUMBLENET_CALL humblenet_portal_disconnect(PeerId, uint64_t) { return false; }
ha_bool HUMBLENET_CALL humblenet_portal_release(PeerId, uint64_t) { return false; }
void HUMBLENET_CALL humblenet_portal_get_stats(humblenet_portal_stats *s) { if(s) *s = {}; }
#endif
