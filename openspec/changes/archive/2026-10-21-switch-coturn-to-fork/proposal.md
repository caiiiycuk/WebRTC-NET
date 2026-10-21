# Proposal

## Why

The `coturn-docker` container is currently built on top of the stock `coturn/coturn:4.9.0-r0`, so we cannot apply our own patches to coturn. We need to switch to the fork https://github.com/caiiiycuk/coturn, but in a way that weak machines do not build coturn themselves and only download a ready image.

## What Changes

- A GitHub Actions workflow is added to the fork `caiiiycuk/coturn`: on push of a tag `release/<coturn-ver>-<N>` it builds `docker/coturn/debian/Dockerfile` for `linux/amd64` and publishes the image `ghcr.io/caiiiycuk/coturn:<coturn-ver>-<N>`.
- All upstream workflows in the fork are disabled (only `Release image` stays enabled), so that the heavy build matrices do not run.
- The fork is added to HumbleNet as git submodule `3rdparty/coturn` (`git@github.com:caiiiycuk/coturn.git`).
- **BREAKING**: `coturn-docker/Dockerfile` switches to `FROM ghcr.io/caiiiycuk/coturn:<tag>`. The coturn version changes from 4.9.0 to 4.18.0 (current master of the fork). The thin layer (USER root, `gettext-base`, `curl`, `openssl`, `entrypoint.sh`) stays; `openssl` is added explicitly because the fork image does not have it.
- Options that no longer exist in 4.18.0 or are deprecated are removed from `turnserver.conf.template`: `no-tlsv1`, `no-tlsv1_1`, `dh2066`, `no-loopback-peers`, `no-cli`. Their behavior is the default in 4.18.0. `no-ipv6` is also removed: coturn has no such option (in 4.9.0 it also produced `Bad configuration format` and was ignored), so behavior does not change.
- `coturn-docker/README.md` describes the image release procedure and the rule: submodule commit = commit of the tag referenced in `FROM`.

## Capabilities

### New Capabilities
- `coturn-docker-image`: where the coturn image for `coturn-docker` comes from, how it is published, and which container guarantees are preserved.

### Modified Capabilities

## Impact

- HumbleNet: `coturn-docker/Dockerfile`, `coturn-docker/turnserver.conf.template`, `coturn-docker/README.md`, `.gitmodules`, new submodule `3rdparty/coturn`.
- Fork `caiiiycuk/coturn`: new workflow in `.github/workflows/`. The user merges it into `master` himself.
- GitHub: package `ghcr.io/caiiiycuk/coturn`; after the first publication it may need to be made public manually.
- Servers: updating requires `docker compose build --pull`; coturn is upgraded from 4.9.0 to 4.18.0.
- HumbleNet CI checks out with `submodules: recursive`, so it will also clone `3rdparty/coturn`.
