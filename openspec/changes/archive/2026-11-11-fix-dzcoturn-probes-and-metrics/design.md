# Design

## Context

Motivation is in `proposal.md`. The current `coturn-docker` and the submodule `3rdparty/coturn` at commit `d08cf551685f45dc6603dbaf5854f4fa54fdd67f` were examined.

- In `src/apps/uclient/uclient.c` the TLS branch of `recv_buffer` passes the whole `SSL_read` result as one `stun_buffer`. `process_received_buffer` expects a single message. The plain TCP branch already determines the length with `stun_get_message_len_str`; for `atc` it separately handles the raw TCP relay payload.
- The diagnosis on 2026-11-11 showed TLS reads of 104 and 208 bytes: all six frames arrived, but the tester counted four messages. `-z 1000` worked around the bug without proving a framing fix. A separate Ubuntu coturn 4.6.1 tester received `Bad channel number`; that is a different compatibility problem, not a reason to change the server bind.
- `coturn-docker/Dockerfile` uses `ghcr.io/caiiiycuk/coturn:4.18.0-3`. The image is released only by a fork release tag through `.github/workflows/release-image.yml`; the existing main spec requires the pin and the submodule commit to match.
- Coturn runs with `network_mode: host`. The template sets `prometheus-address=127.0.0.1`; the publisher reads `http://127.0.0.1:9641/metrics`. Therefore an external IPv4 scrape only needs a different bind, without Docker port mapping and without changing the publisher.
- The README and the `init.sh` hint contain `rm .env && docker compose down -v`; the backup section does not include `coturn-data`. The active neighboring change about the capacity deadline modifies other parts of the README and the publisher; its logic and artifacts are not touched.

## Goals / Non-Goals

**Goals:** fix the reception of control frames in the tester's real TLS path, provide reproducible tests of read boundaries, and ship that tester through a pinned image. Open the default metrics listener with an explicit description of the new access boundary. Make the verification and maintenance instructions safe for the existing volume.

**Non-Goals:** changing the TURN server data path, UDP/DTLS algorithms, the raw TCP relay protocol, cloud, quotas, capacity history or the publication format. No exporter, HTTP authorization, TLS proxy for metrics, new orchestrator or mandatory dependencies. No firewall/DNS/VPS changes and no wiki publication within this change.

## Decisions

### 1. Reassemble control frames instead of slowing the test down

Use coturn's existing wire-format checks, including `stun_get_message_len_str`, to determine the full length of STUN/ChannelData and the stream padding. Keep a bounded state of the incomplete read on the specific TLS connection. Hand the handler one complete frame and keep the rest; the following complete frames must be processed even without a new network event. Partial header and payload are not lost on `WANT_READ`/`WANT_WRITE`; waiting for readiness is not replaced by a busy loop. Connection termination and an invalid/oversized length produce an error, not a successful delivery of truncated data.

Do not extend STUN framing to the raw data connection of the TCP relay. It is distinguished by the existing `data_connection`/`atc` context; the payload start may look like a STUN header. Place the receive state in the existing connection structure in `session.h` only if it is needed between calls, and release it together with the connection. Do not create a global buffer or a new universal transport layer.

Alternatives: raising `-z` hides the bug, and the assumption "one TLS record = one frame" contradicts stream semantics. Applying plain `MSG_PEEK` to a TLS socket reads ciphertext, so it cannot be copied literally.

### 2. The regression must reproduce stream shapes deterministically

Add a small C test target to the existing `3rdparty/coturn/tests/CMakeLists.txt` with `BUILD_TESTING=ON`. Test exactly the code used by the TLS receiver by feeding controlled chunks: several frames in one read, a header in parts, a payload in parts, a full frame with the tail of the next one, STUN + ChannelData + padding + Data indication, close and an invalid length. Check temporary absence of data, independence of two connections and the raw data mode. An integration TLS relay smoke with the real tester and a compatible coturn at the normal send interval is needed separately; a test of the isolated wire parser alone does not prove the receiver fix.

UDP and plain TCP remain control scenarios; the existing DTLS/TCP relay checks are not disabled. A positive result requires equal non-zero data counters and zero loss. The report format is not fixed by artificially correcting counters.

### 3. Change the metrics default minimally

Replace the single template line with `prometheus-address=0.0.0.0`. The port stays `9641`; the publisher address, scrape deadline/body limit, peer deny list and Compose networking do not change. A new env parameter is not needed: for the local mode the operator explicitly restores `127.0.0.1` in the template.

The README must describe the external bind consistently in all sections, show a local and a remote scrape, a firewall allowlist example for the monitoring address/CIDR, and warn that the endpoint is not encrypted and does not authenticate the reader. This is not an instruction to open the port to the whole internet and not an automatic firewall edit. In an isolated Linux network namespace check access through a non-loopback address and the loopback scrape; then restore the bind to loopback and check that an external connection is refused. Check real HTTP responses and the existing traffic/quota/snapshot families.

### 4. The README is an operational manual, not a VPS test protocol

Update `Testing TURN` with two bounded relay tests from a separate machine: UDP and TLS with paired clients, payload and strict counters. Use the tester from the exactly specified compatible image, not a random distribution package. Show obtaining temporary credentials without `echo` of values and without `set -x`; the shared secret is not copied to an untrusted node. If credentials are passed as tester arguments, explicitly describe their visibility to local processes and use a trusted machine. For the older image separately describe the temporary `-z 1000`, without presenting it as a check of the fixed tester or as capacity calibration.

Describe backup for the actual Compose project, not only the hardcoded `coturn-docker_*`: stop the writer, save the private `.env`, the certificate volume and the whole `coturn-data`, restore into the same volumes before the start. Check that the watermark, identity and traffic fixture are preserved. Do not merge JSON by hand and do not initialize a history instead of a lost one.

For staging → production keep `.env` and the volumes, obtain the production certificate explicitly through the production ACME server with the same domain/lineage, taking the occupied standalone port into account, and restart only the certificate consumers. Verify the exact commands with a fake Docker/certbot fixture: no `down -v`, no `.env` deletion and no real certificate request. The `init.sh` hint must point to the same non-destructive scenario; changing the whole interactive installer is not required. Fix the mention of a mandatory `443` for TURNS to the port actually used, `5349`, and keep the separate purpose of the HTTPS ping `8000`.

### 5. Release the fix through the existing release chain

The tester changes belong to the fork's git history, not to a patch on top of the published image. After the tests prepare a new `release/<coturn-ver>-<N>`, built by the standard workflow, then update `FROM` and the `3rdparty/coturn` gitlink to the same release commit. Do not use `latest`, do not change the previous tag/asset and do not reference a non-existent image. A new dzcoturn archive, if released as a consistent delivery, gets the next free number and consistent README/template.

Publishing requires separate permission; without it the release/pin check stays open, and the missing image is not declared released. Synchronizing the public GitHub wiki remains separate work. The old cloud pacing workaround is not removed here: changing it in another project is not required for this contract.

## Risks / Trade-offs

- Public metrics may expose operational data → the breaking change is described prominently, the firewall is restricted to monitoring addresses; the loopback opt-out is verified.
- TLS may return already decrypted data without a new socket event → the tests check several buffered frames and the absence of a hang between them.
- The tester update touches error handling and synchronous/nonblocking calls → both paths, partial EOF, invalid lengths and cleanup are checked; the raw TCP relay is kept separate.
- Pacing may accidentally hide a regression → the fixed receiver and the new image are checked at the normal interval, the workaround is described only for the old version.
- Concurrent README changes in the capacity change → the edits are limited to the verification, backup, staging and metrics sections; do not change the history rules or close someone else's tasks. Exception: the sentence of the `Capacity publication and recovery` section about the metrics bind to `127.0.0.1` and the note in the security section would contradict the new default; in the capacity section change only that sentence, to avoid conflicting with task 1.6 of change `harden-capacity-history-against-deadline`.
- The certificate instruction may delete the previous identity or trigger an ACME rate limit → the fixture checks that `.env`/volumes are preserved, the production ACME is not called automatically.

## Migration Plan

1. Implement and verify the receiver in the fork; run the C tests, sanitizers on the new tests, UDP/TCP/TLS smoke and the wrapper regressions.
2. Run all builds of the HumbleNet Linux targets and separately `ninja -j32 webrtcnet` from `emscripten`. Check `bash -n`, the Python tests and OpenSpec.
3. After separate permission publish the new tag/image and verify its provenance; update the consistent pin/gitlink and verify the tester inside the new image. Check the contents of the new archive if archive publication is allowed.
4. Document the metrics opt-out before updating a running installation. Deployment is performed separately by the operator with the existing Compose project, `.env` and volume; this change does not touch a running VPS.
5. Rolling back to the previous image and the loopback template does not require deleting volumes or resetting the history; a tester rollback restores the previous TLS framing limitation, not a guarantee of the fix.
