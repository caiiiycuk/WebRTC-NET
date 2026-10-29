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
