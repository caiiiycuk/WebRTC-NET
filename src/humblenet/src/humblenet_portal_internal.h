#ifndef HUMBLENET_PORTAL_INTERNAL_H
#define HUMBLENET_PORTAL_INTERNAL_H
#include "humblenet_portal.h"
struct Connection;
bool portal_enabled();
Connection *portal_offer(PeerId peer);
bool portal_owns(Connection *conn);
void portal_bound(Connection *conn);
void portal_opened(Connection *conn);
void portal_closed(Connection *conn);
void portal_destroyed(Connection *conn);
int portal_ingress(Connection *conn, const void *data, int length);
bool portal_blacklisted(PeerId peer);
void portal_notify(void *unused);
uint64_t portal_now_ns();
void portal_shutdown();
int humblenet_connection_portal_write(Connection *, const void *, uint32_t);
#endif
