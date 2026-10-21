# Spec Delta

## Purpose

Defines where `coturn-docker` gets the coturn image (the fork `caiiiycuk/coturn`, published to ghcr), how this image is released, and which container behavior is preserved after the base change.

## ADDED Requirements

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
