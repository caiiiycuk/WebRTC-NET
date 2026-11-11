# Proposal

## Why

During the TURN recovery on 2026-11-11 the tester from image `4.18.0-3` received all TLS data but reported losses: its `SSL_read` merged several TURN frames and the handler counted the result as one message. The dzcoturn instructions do not verify external UDP/TLS relay, suggest deleting persistent state when switching from staging, and describe only local metrics, while an external Prometheus is required.

## What Changes

- Fix TLS framing in `turnutils_uclient` inside `3rdparty/coturn`: handle coalesced and split frames without losing the tail, with a bounded buffer and regression tests. Raising the send interval does not count as a fix.
- Ship the fixed tester through a new explicitly pinned fork image. Keep the submodule commit matching the image release tag; do not reuse published versions.
- **BREAKING**: change the default `prometheus-address` from `127.0.0.1` to `0.0.0.0`, keeping port `9641` and the publisher's local scrape. Document plain HTTP without authorization, firewall restriction to monitoring addresses, and the option to explicitly restore loopback in the template.
- In the README describe external UDP/TLS relay checks with positive equal send/receive counters and zero loss. Separate the incompatible old tester, the TLS framing bug, and a server fault; keep pacing only as a workaround for older images. Fix the troubleshooting mention of port `443` for TURNS to `5349`, keeping `8000` for the HTTPS ping.
- Fix the backup/restore and staging → production instructions in the README and in the `init.sh` output: keep `.env`, certificates and the whole `coturn-data`, including the capacity and traffic history; do not suggest `down -v`, deleting `.env` or re-initializing an existing identity.

## Capabilities

### New Capabilities

- `turnutils-tls-framing`: correct handling of the TLS stream by the TURN tester, including fragmentation, message coalescing and safe connection termination.

### Modified Capabilities

- `coturn-docker-image`: external Prometheus by default, delivery of a verified tester, and operational instructions that verify relay and preserve state.

## Impact

- Fork: `3rdparty/coturn/src/apps/uclient/uclient.c`, connection state in `session.h` where needed, the nearest C tests and `tests/CMakeLists.txt`.
- Wrapper: `coturn-docker/Dockerfile`, `turnserver.conf.template`, `README.md`, the `init.sh` help output, configuration tests and integration checks. `publisher.py`, the publication format, quotas, TURN ports and the capacity history rules do not change.
- Build and delivery: Linux `amd64` for the image; mandatory builds of the HumbleNet Linux targets and `emscripten/ninja -j32 webrtcnet`. The existing change `harden-capacity-history-against-deadline` is not modified.
- In scope: preparing the new image and a consistent pin; publishing tags/image/archive requires separate permission. Publishing the GitHub wiki and deploying to running servers are out of scope. Changes to the cloud project, VPS firewall, DNS, secrets and old release assets are out of scope.
