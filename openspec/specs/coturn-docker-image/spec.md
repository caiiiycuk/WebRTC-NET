# coturn-docker-image Specification

## Purpose
Defines where `coturn-docker` gets the coturn image (the fork `caiiiycuk/coturn`, published to ghcr), how this image is released, and which container behavior is preserved after the base change.

## Requirements

### Requirement: Fork image publishing on release tag
The fork `caiiiycuk/coturn` SHALL build and publish the image `ghcr.io/caiiiycuk/coturn` for platform `linux/amd64` only on push of a tag of the form `release/<coturn-ver>-<N>`. The image tag MUST match the part of the git tag after `release/`. Pushes to branches, pull requests, and schedules MUST NOT publish the image.

#### Scenario: Release tag push
- **WHEN** tag `release/4.18.0-1` is pushed to the fork
- **THEN** image `:4.18.0-1` for `linux/amd64`, built from the commit of this tag, appears in `ghcr.io/caiiiycuk/coturn`

#### Scenario: Push to master without a tag
- **WHEN** a commit without a `release/*` tag is pushed to the fork `master`
- **THEN** no image is published and the heavy upstream build matrix does not run

### Requirement: coturn-docker uses a pinned fork image
`coturn-docker` SHALL build the coturn container on top of `ghcr.io/caiiiycuk/coturn` with an explicit release tag. The build on the server MUST NOT compile coturn from source; it only downloads the base image and adds a thin layer.

#### Scenario: Update on a weak machine
- **WHEN** `docker compose build --pull` and `docker compose up -d` are run on the server
- **THEN** the base image is downloaded from ghcr, coturn is not compiled, and the container starts

### Requirement: Fork sources available as a submodule
HumbleNet SHALL include the fork as git submodule `3rdparty/coturn` with URL `git@github.com:caiiiycuk/coturn.git`. The committed submodule commit MUST match the commit of the tag whose image is referenced in `FROM` of `coturn-docker/Dockerfile`.

#### Scenario: Version check
- **WHEN** a developer looks at the `3rdparty/coturn` commit and the tag in `FROM`
- **THEN** tag `release/<image tag>` in the fork points to the same commit

### Requirement: Container behavior is preserved
The container on the new image SHALL preserve the observable behavior of the current one: config generation from `turnserver.conf.template`, TLS with certbot certificates and restart on their renewal, ICE credentials publishing, Prometheus metrics on port 9641 including `turn_total_traffic_rcvb` and `turn_total_traffic_sentb`, healthcheck via `turnutils_stunclient`. TLSv1 and TLSv1.1 MUST stay disabled, loopback peers MUST be forbidden, CLI MUST stay disabled.

#### Scenario: Container start
- **WHEN** the container starts with a working `.env`
- **THEN** `turnserver` starts, the healthcheck passes, `http://localhost:9641/metrics` serves traffic metrics, and the entrypoint outputs ICE credentials without errors about missing `openssl`, `curl`, or `envsubst`

#### Scenario: Config without deprecated options
- **WHEN** `turnserver` reads the generated config
- **THEN** the log has no `Bad configuration format` and no messages about deprecated options

### Requirement: Automatic publisher recovery

With a non-empty `PUBLISH_URL` the container SHALL automatically start the exited publisher again regardless of the exit code or the terminating signal. Between the exit and the next attempt there MUST be a 5 second pause, without a limit on the number of attempts. Recovery MUST NOT restart the running coturn or interrupt its client connections. With an empty or missing `PUBLISH_URL` the publisher MUST NOT start. The log SHALL contain the exit code and the message about the next attempt without secret values and publication contents.

#### Scenario: Publisher exits successfully or with an error
- **WHEN** the publisher exits with code 0 or a non-zero code while the container is running and publications are enabled
- **THEN** after a 5 second pause the container starts the publisher again, and the coturn process keeps running without a restart
- **AND** the log contains the exit code and the restart message without secrets

#### Scenario: Publisher is killed by a signal
- **WHEN** the publisher process receives SIGKILL while the container is running
- **THEN** after a 5 second pause a new publisher starts without restarting coturn

#### Scenario: Repeated startup failure
- **WHEN** the publisher exits right after each start
- **THEN** the attempts continue with a 5 second pause between the exit and the new start, without a continuous CPU-consuming loop

#### Scenario: Publications disabled
- **WHEN** the container starts with an empty or missing `PUBLISH_URL`
- **THEN** coturn runs without a publisher and without attempts to restart it

### Requirement: Cycle deadline does not end the publisher

The publisher SHALL keep the 5 second deadline per cycle and MUST keep running if the deadline `TimeoutError` is raised outside `cycle()`: while handling a cycle error, cancelling the timer, or in the pause before the next cycle. The cycle error and process exit messages SHALL contain the exception class and text without secret values and publication contents.

#### Scenario: Deadline fires while handling a cycle error
- **WHEN** the cycle failed with an error and the deadline `TimeoutError` is raised while printing `cycle failed` or before the timer is cancelled
- **THEN** the process runs the next cycle and does not exit

#### Scenario: Cycle error is diagnosable
- **WHEN** the cycle ended with an exception
- **THEN** the `cycle failed` message contains the exception class and text, and capacity in this cycle is published as blocked by the existing rules

### Requirement: Safe publisher lifecycle

The container SHALL stop the publisher and the restart wait on SIGTERM or SIGINT and MUST NOT start the publisher after stopping has begun. Automatic restart MUST preserve the existing single-publisher lock, the capacity/traffic files, and the state recovery rules; it MUST NOT delete or reinitialize them. External container termination and the existing coturn restart on certificate renewal MUST keep their purpose.

#### Scenario: Stop with a running publisher
- **WHEN** the container receives SIGTERM or SIGINT while the publisher is running
- **THEN** the publisher and its restart loop end together with the container, without a new start and without leftover child processes

#### Scenario: Stop during the pause
- **WHEN** the container receives SIGTERM or SIGINT while waiting for a restart
- **THEN** the wait is interrupted and a new publisher does not start

#### Scenario: State preserved on recovery
- **WHEN** the publisher starts again after an exit and the volume has saved state
- **THEN** it uses the existing files and lock by the previous rules, without a history reset or a parallel publication by a second process

#### Scenario: Certificate renewal
- **WHEN** the certificate changes while the publisher is running
- **THEN** coturn restarts by the existing mechanism, and a second publisher does not start

### Requirement: Capacity history is independent of the publish address

The publisher SHALL accept a valid saved capacity history of the same `DOMAIN` regardless of whether the current `PUBLISH_URL` matches the saved `binding.publishUrl`. It MUST keep `attemptedThroughMs`, send publications to the currently configured URL, and not require manual history editing or reinitialization on a URL change. Existing version 1 files SHALL remain readable without a prior migration.

#### Scenario: URL change with an existing history
- **WHEN** the publisher starts with a different valid HTTPS `PUBLISH_URL`, and a version 1 file contains the history of the same `DOMAIN` and a positive watermark
- **THEN** the history is accepted without a watermark reset
- **AND** publications go to the current `PUBLISH_URL`
- **AND** the URL change alone does not disable capacity

#### Scenario: History has no capacity attempts yet
- **WHEN** a valid file of the same domain contains `attemptedThroughMs=0` and a different saved URL
- **THEN** the publisher can publish a fresh valid `known` without reinitialization

### Requirement: URL change keeps capacity history protection

On a URL change the publisher MUST keep the checks of the domain, version, valid integer watermark, file integrity, and external state changes. It SHALL keep the existing checks of HTTPS, metrics freshness, quotas, and saving the watermark before sending `known`. After a restart with a positive watermark the publisher MUST publish `unknown` until there is a healthy snapshot with a strictly larger watermark; a URL change does not allow re-announcing an old snapshot as `known`. An external file change with the same `binding` and a barrier strictly larger than the remembered one is accepted as the new barrier by the "forward only" rule; other external changes disable `known`.

#### Scenario: Restart with a positive watermark
- **WHEN** after a URL change the publisher restored a positive watermark and a healthy source has not yet provided a strictly newer mark
- **THEN** capacity stays `unknown` with the saved barrier
- **AND** when a strictly newer healthy snapshot appears, its watermark is durably saved before `known` is sent

#### Scenario: History belongs to another domain
- **WHEN** the domain of the saved history does not match the current `DOMAIN`
- **THEN** `known` publication is disabled regardless of the URL
- **AND** the history is not reinitialized automatically

#### Scenario: History is missing or corrupt
- **WHEN** the file is missing, corrupt, or contains an invalid version or watermark
- **THEN** the publisher does not create a new history automatically and does not publish `known`
- **AND** ICE and traffic can be published by the existing rules

#### Scenario: File change at run time
- **WHEN** the saved state changed externally after the publisher accepted it
- **THEN** with a different `binding`, a barrier smaller than the remembered one, or invalid content the publisher disables `known` by the existing rules
- **AND** with the same `binding` and a barrier strictly larger than the remembered one the publisher accepts the barrier from the file by the "forward only" rule
- **AND** allowing a URL setting change does not cancel the file integrity control

#### Scenario: Unfit source or address
- **WHEN** the new address fails the HTTPS check or the metrics fail the existing freshness and quota checks
- **THEN** the URL change does not allow bypassing the corresponding check

### Requirement: Cycle deadline does not disable the capacity history

A cycle deadline firing while the capacity history is read or written SHALL end only the current cycle as a cycle error and MUST NOT move the history into the "unavailable" state. The next cycle SHALL use the same saved history again without a process restart. A transient history file read error unrelated to the deadline SHALL block `known` publication only in the current cycle.

#### Scenario: Deadline during the history read
- **WHEN** the cycle deadline fires while the publisher reads and verifies the saved capacity history
- **THEN** the cycle ends with a `cycle failed` message containing the exception class and text, and capacity in this cycle is not published as `known`
- **AND** the next cycle with a newer healthy snapshot publishes `known` without a process restart

#### Scenario: Deadline after the barrier write
- **WHEN** the cycle deadline fires after the new barrier is already written to the history file but before the publisher remembered it in memory
- **THEN** the cycle ends as a cycle error, and the next cycle accepts the written barrier and publishes `known` on a newer healthy snapshot

#### Scenario: Transient read error
- **WHEN** reading the history file fails with an error unrelated to the deadline in one cycle and succeeds in the next, with a positive remembered barrier
- **THEN** in the first cycle capacity is published as `unknown` with the previous barrier, and in the next one as `known` on a newer healthy snapshot

### Requirement: History barrier is accepted only forward

If the history file contains the same `binding` and a barrier `attemptedThroughMs` strictly larger than the one remembered in memory, the publisher SHALL accept the barrier from the file as the current one and continue publishing by the usual rules. If the `binding` differs, the barrier is smaller than the remembered one, or the file content is invalid, the publisher MUST stop `known` publication until the process restarts, as before. Accepting a barrier forward MUST NOT lead to publishing a snapshot with `observedAtMs` not exceeding the accepted barrier.

#### Scenario: Barrier in the file ahead of memory
- **WHEN** the history file with the same `binding` contains a barrier larger than the one remembered in memory
- **THEN** the publisher accepts the barrier from the file, publishes `unknown` with that barrier for a snapshot not newer than it and `known` for a strictly newer snapshot

#### Scenario: Barrier in the file behind memory
- **WHEN** the history file with the same `binding` contains a barrier smaller than the one remembered in memory
- **THEN** the publisher stops `known` publication until the process restarts and does not overwrite the file

#### Scenario: Binding changed
- **WHEN** the `binding` in the history file changed
- **THEN** the publisher stops `known` publication until the process restarts, as before the change

### Requirement: History disablement is diagnosable

When the publisher in a running process moves the capacity history into the "unavailable" state, it SHALL write to stderr once a message with the reason and the exception class without secrets and publication contents. It MUST NOT repeat the message in every subsequent cycle.

#### Scenario: History disabled at run time
- **WHEN** the history file check in a cycle finds a different `binding`, a barrier behind memory, or invalid content
- **THEN** one line about the history being unavailable with the reason appears in stderr, and subsequent cycles without a state change do not add new lines
