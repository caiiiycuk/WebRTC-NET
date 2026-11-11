## ADDED Requirements

### Requirement: External Prometheus by default

The shipped dzcoturn template SHALL enable Prometheus on IPv4 `0.0.0.0:9641`, keeping `http://127.0.0.1:9641/metrics` reachable for the publisher and the existing metric families. The instructions MUST state explicitly that the HTTP endpoint has no authorization and no TLS, and describe allowing TCP `9641` only for monitoring addresses in the host/provider firewall. They SHALL describe explicitly restoring `prometheus-address=127.0.0.1` for local-only monitoring. The default change MUST NOT automatically change the firewall of a running server.

#### Scenario: Scrape from another host
- **WHEN** the container runs with the default template and host network mode, and the firewall allows the monitoring source
- **THEN** another host receives a Prometheus response through the TURN server's IPv4 address and TCP `9641`
- **AND** the local publisher keeps receiving the same metrics through loopback

#### Scenario: Local mode chosen explicitly
- **WHEN** the operator sets `prometheus-address=127.0.0.1` in the template and applies it
- **THEN** the local scrape works, and the external address does not serve the endpoint

### Requirement: Trustworthy relay check instructions

The README SHALL contain an external UDP and TLS relay check with the tester from a compatible pinned image. Success MUST require actual payload transfer, positive equal send/receive counters and zero loss; a STUN binding, a TURN allocation or a TLS handshake alone is not enough. The examples MUST NOT print the shared secret or temporary credentials to logs and MUST warn about the possible visibility of process arguments on the tester machine.

#### Scenario: External check with the fixed tester
- **WHEN** the operator runs the documented UDP/TLS checks from a separate machine with temporary credentials
- **THEN** the instructions let them distinguish a successful transfer from mere port availability
- **AND** TLS is checked at the normal send interval, without a mandatory longer pause

#### Scenario: Older tester
- **WHEN** an old incompatible tester or an image with the known TLS framing bug is used
- **THEN** the instructions distinguish `Bad channel number` from the miscounting of coalesced TLS frames
- **AND** pacing is marked only as a temporary workaround, not as a server fix and not as a capacity measurement

### Requirement: State is preserved during maintenance

The README and the staging → production messages SHALL keep `.env`, the identity, the existing `coturn-data` and the certificates. A backup MUST include the whole `coturn-data`, including `capacity_state.json` and the traffic state, be taken consistently with the writer stopped and be restored before the publisher starts. The instructions MUST NOT suggest `down -v`, deleting `.env` or re-initializing the history as a routine certificate update. The loss of the previous server's history MUST NOT be treated as a new identity.

#### Scenario: Staging is replaced by production
- **WHEN** the operator follows the README or the `init.sh` hint to change the certificate issuer
- **THEN** the previous secret, domain, publication barrier and traffic state are kept, and the production certificate is obtained without deleting persistent volumes

#### Scenario: Data restore
- **WHEN** the operator restores a backup of the previous server
- **THEN** the configuration and the consistent volume are restored first into the Compose project actually in use
- **AND** after the start the history is not reset and the old watermark is not declared a new history

#### Scenario: The previous server's history is missing
- **WHEN** `capacity_state.json` is lost or invalid
- **THEN** the instructions keep the existing behavior without automatically creating a new history; ICE/traffic and capacity are described separately

### Requirement: Fixed tester is delivered reproducibly

A new dzcoturn delivery SHALL use a new explicitly pinned release image with the fixed TLS framing. The `3rdparty/coturn` commit MUST match the commit of that image's release tag, as the existing project policy requires. Previously published image tags and release assets MUST NOT be overwritten; the number of the next delivery is chosen from the available versions at release time.

#### Scenario: Verification of the new delivery
- **WHEN** a consistent new version is released
- **THEN** the tester inside the image passes the framing regression and the UDP/TLS relay smoke at the normal interval
- **AND** the pin, the release tag and the submodule commit correspond to each other unambiguously
