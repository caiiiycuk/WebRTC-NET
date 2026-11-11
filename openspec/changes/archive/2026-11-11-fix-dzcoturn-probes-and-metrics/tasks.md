# Tasks

## 1. Tester TLS receiver in the coturn fork

- [x] 1.1 Add a C regression target to `3rdparty/coturn/tests/CMakeLists.txt` that reproduces the real TLS receive path on controlled chunks: coalesced frames, partial header/body, a full frame with a tail, STUN/ChannelData/Data indication with padding. Check that the original version loses messages and that the expected payload and counters are fixed without pacing.
- [x] 1.2 Fix TLS control framing in `src/apps/uclient/uclient.c` and, where needed, `session.h`: bounded per-connection state, tail preservation, processing of ready frames without a new event, correct sync/nonblocking waits and cleanup. Check the C tests and ASan/UBSan on invalid/oversize length, temporary unreadiness, partial EOF, repeated and parallel connections; do not parse the raw TCP relay payload as STUN.
- [x] 1.3 Check bounded UDP/plain TCP/TLS relay smoke with the fixed tester at the normal interval, keep the DTLS and TCP relay data connection regressions; pass criterion: non-zero equal data counters, zero loss, a deliberately undelivered payload does not count as success.

## 2. External Prometheus

- [x] 2.1 Change the default in `coturn-docker/turnserver.conf.template` to `prometheus-address=0.0.0.0` without a new env API, port mapping or publisher changes. Add a check of the generated config: port `9641`, the peer deny list and the publisher's local scrape are preserved; the existing Python publisher tests pass.
- [x] 2.2 Add an isolated Linux namespace/container check of a real HTTP scrape through a non-loopback IPv4 and through `127.0.0.1`, then the loopback-only opt-out with an external connection refused. Align all README sections about metrics, add a warning about HTTP without authorization, a host/provider firewall allowlist example and an explicit opt-out; check the existing traffic/quota/snapshot families and the absence of automatic host firewall changes. Places that currently claim loopback-only: the metrics section, one sentence in `Capacity publication and recovery`, an item in security, the `curl` example in troubleshooting.

## 3. Checks and state preservation in the instructions

- [x] 3.1 Rewrite the README `Testing TURN` into external UDP/TLS paired-client payload checks with a compatible pinned tester, a bounded timeout, the normal TLS interval and strict counters. Verify the commands on an isolated server/client; a STUN binding, an allocation and a TLS handshake alone do not count as success. The examples do not print the shared secret/credentials and warn about process argv; fix the `443`/`5349`/`8000` port confusion. Add a diagnosis of the two different problems of old testers (`Bad channel number` and miscounting of coalesced TLS frames) and a description of temporary pacing for the old image only.
- [x] 3.2 Fix the README backup/restore: the actual Compose project, a stopped writer, the private `.env`, certificates and the whole `coturn-data`, restore before the publisher starts. Verify the instructions on a fixture volume with an identity, a positive watermark and traffic state: after backup/restore the values and permissions are preserved, the missing history of the previous server is not re-initialized.
- [x] 3.3 Replace the destructive staging → production instructions in the README and in the `init.sh` help output with a scenario that keeps `.env`/volumes, an explicit production ACME server/lineage and takes the occupied standalone port into account. Add a fake Docker/certbot test of the documented commands and the hint: the production server is selected, domain/secret/state are unchanged, `down -v` and `.env` deletion are not called; `bash -n init.sh` passes, no real ACME requests are made.

## 4. Overall verification of the sources and the wrapper

- [x] 4.1 Run the fork CTest with `BUILD_TESTING=ON`, all `coturn-docker/test_*.py`, shell syntax and Compose validation; build all HumbleNet Linux targets and run `ninja -j32 webrtcnet` from `emscripten`. Record the commands and results, do not count skipped transport/namespace checks as done; do not use the production `.env` and do not print the full Compose config with secrets.
- [x] 4.2 Run `openspec validate fix-dzcoturn-probes-and-metrics --strict`, `git diff --check` in HumbleNet and in the fork; check that the change `harden-capacity-history-against-deadline`, the publisher semantics, cloud and the VPS are unchanged, and that the main spec and the new requirements do not contradict each other.

## 5. Consistent delivery of the fixed tester

- [x] 5.1 After separate publication permission release a new free `release/<coturn-ver>-<N>` through the existing fork workflow and verify the image provenance. Update `coturn-docker/Dockerfile` and the `3rdparty/coturn` gitlink to the same release commit, README/release notes to the actual version; check the tag/commit/image correspondence and do not overwrite previous tags/assets. Without permission or an image leave the task open.
- [x] 5.2 Verify the fixed `turnutils_uclient` inside the published pinned image: UDP/TLS smoke without pacing and real external/local metrics of the default template. Check the package contents and the new number if the archive release is separately allowed; record the tag, digest and results without secrets. Do not update running VPS and the GitHub wiki, do not mark unperformed release checks as done.
