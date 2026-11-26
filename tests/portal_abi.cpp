#include "humblenet_p2p.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <fcntl.h>

int main(int argc, char **argv) {
	assert(argc == 2);
	const auto capabilities = std::strtoull(argv[1], nullptr, 0);
	assert(capabilities == 0 || capabilities == HUMBLENET_PORTAL_CAPS_V1);
	assert(humblenet_init());
	assert(humblenet_portal_capabilities() == capabilities);
	assert(humblenet_portal_event_fd() == -1);
	assert(!humblenet_portal_enable(nullptr));
	humblenet_portal_options options = {HUMBLENET_PORTAL_ABI, 160, 8, "room.q2portal.test"};
	options.abi_version = 0;
	assert(!humblenet_portal_enable(&options));
	options.abi_version = HUMBLENET_PORTAL_ABI;
	options.max_connections = 161;
	assert(!humblenet_portal_enable(&options));
	options.max_connections = 160;
	options.max_pending = 9;
	assert(!humblenet_portal_enable(&options));
	options.max_pending = 8;
	options.alias = "room.q2portal.invalid/path";
	assert(!humblenet_portal_enable(&options));
	options.alias = "room.q2portal.test";
	assert(bool(humblenet_portal_enable(&options)) == bool(capabilities));
	if (capabilities) {
		const int descriptor = humblenet_portal_event_fd();
		assert(descriptor >= 0);
		assert(fcntl(descriptor, F_GETFL) & O_NONBLOCK);
		assert(fcntl(descriptor, F_GETFD) & FD_CLOEXEC);
		assert(!humblenet_portal_enable(&options));
		humblenet_portal_set_admission(true);
		humblenet_portal_service();
		char buffer[1400] = {};
		humblenet_portal_event event = {};
		assert(humblenet_portal_recv(buffer, sizeof(buffer), &event) == 0);
		assert(humblenet_portal_recv(buffer, 1, &event) == -1);
		humblenet_portal_stats stats = {};
		humblenet_portal_get_stats(&stats);
		assert(!stats.reservations && !stats.pending && !stats.closing);
	}
	assert(humblenet_portal_send(1, 1, "x", 1) == HUMBLENET_PORTAL_CLOSED);
	assert(humblenet_portal_connection_state(1, 1) == 0);
	assert(!humblenet_portal_started(1, 1));
	assert(!humblenet_portal_established(1, 1));
	assert(!humblenet_portal_disconnect(1, 1));
	assert(!humblenet_portal_release(1, 1));
	humblenet_shutdown();
	assert(humblenet_portal_event_fd() == -1);
}
