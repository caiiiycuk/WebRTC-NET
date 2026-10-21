# Design

## Context

- `coturn-docker/Dockerfile` now: `FROM coturn/coturn:4.9.0-r0` + `USER root` + `gettext-base curl` + `entrypoint.sh`. `docker-compose.yml` builds it via `build:`.
- The fork `caiiiycuk/coturn` has only `master`, which matches upstream (4.18.0). The fork has no tags (checked with `git ls-remote --tags`).
- The fork already has `docker/coturn/debian/Dockerfile`: a multi-stage build from the local context (repo root), runtime on `debian:trixie-slim`, `USER nobody:nogroup`. The runtime stage has no `openssl` CLI. The old `coturn/coturn:4.9.0-r0` had it (`/usr/bin/openssl`, checked with `docker run`). `entrypoint.sh` uses `openssl dgst` for ICE credentials.
- The upstream `.github/workflows/docker.yml` in the fork runs on push to `master`, on PR, and on schedule. Publishing there is gated by `repository_owner == 'coturn'`, but the alpine/debian × 7 arch matrix via QEMU still runs. The other upstream workflows (cifuzz, clang, cmake, codeql, linux, macos, mingw, msvc) also run on push/PR.
- coturn master has no `no-tlsv1`, `no-tlsv1_1`, `dh2066`, `no-loopback-peers` options (an unknown config line produces a WARNING and is ignored). `no-cli` produces ERROR "deprecated, see --cli".

## Goals / Non-Goals

**Goals:**
- Minimal diff in the fork, so that rebasing on upstream has no conflicts.
- The server only downloads the image and compiles nothing.

**Non-Goals:**
- Multi-arch (only `linux/amd64`).
- Alpine variant of the image.
- Own patches to coturn: this change only prepares the infrastructure.
- Changes to `docker-compose.yml`, `init.sh`, `entrypoint.sh`.

## Decisions

### 1. New workflow in the fork instead of editing upstream `docker.yml`
File `.github/workflows/release-image.yml`: trigger `push: tags: ['release/*']`, `permissions: packages: write, contents: read`, steps `actions/checkout` → `docker/login-action` (ghcr, `GITHUB_TOKEN`) → `docker/build-push-action` with `context: .`, `file: docker/coturn/debian/Dockerfile`, `platforms: linux/amd64`, `tags: ghcr.io/caiiiycuk/coturn:${GITHUB_REF_NAME#release/}` (the tag is computed in a separate step into `$GITHUB_OUTPUT`).
- Alternative: edit `docker.yml` (owner conditions, matrix). Rejected: a large diff with upstream and conflicts on every rebase.
- Build from the local context (`coturn_git_ref=HEAD` by default) → the image is built exactly from the tag commit.

### 2. Upstream workflows are disabled via GitHub instead of deleting files
All upstream workflows in the fork are disabled; only `Release image` stays enabled. This is done with `gh workflow disable <workflow> -R caiiiycuk/coturn` (or the button in the Actions UI). Diff in the fork is zero.
- Alternative: delete the files. Rejected: a conflict on every upstream change of those files.

### 3. The thin layer in HumbleNet stays, `openssl` is added
```
FROM ghcr.io/caiiiycuk/coturn:4.18.0-1
USER root
RUN apt-get ... gettext-base curl openssl ...
```
The tag is written directly in `FROM`, without `ARG`: it changes only on release, together with the submodule.
- Alternative: add `curl`/`openssl`/`gettext-base` to the fork Dockerfile. Rejected: extra diff with upstream, and only HumbleNet needs it.

### 4. Submodule `3rdparty/coturn` with SSH URL
Same as the other submodules in `.gitmodules` (`git@github.com:...`). The fork workflow is committed inside the submodule to a separate branch (for example, `ghcr-release`) and pushed to the fork. The user merges it into `master` and creates the tag himself.

### 5. Cleanup of `turnserver.conf.template`
Remove `no-tlsv1`, `no-tlsv1_1`, `dh2066`, `no-loopback-peers`, `no-cli`. The required behavior (TLS 1.0/1.1 off, DH 2066, loopback forbidden) is the default in 4.18.0. For CLI this has to be confirmed in the sources before removing the line (task in tasks.md).

Confirmed in the 4.18.0 sources (`src/apps/relay/mainrelay.c`, `turn_admin_server.c`):
- CLI: `bool use_cli = false;` (`turn_admin_server.c`), enabled only by the `--cli` option; `no-cli` only logs ERROR "deprecated, see --cli" and changes nothing.
- TLS: `enable_tlsv1 = false`, `enable_tlsv1_1 = false` by default; enabled by the `--tlsv1` / `--tlsv1_1` options. When they are off, coturn does not call `SSL_CTX_set_min_proto_version` and relies on the OpenSSL 3 defaults (security level ≥ 1 forbids TLS < 1.2). So TLS 1.0/1.1 are disabled by OpenSSL, not by coturn; checked on a running container (OpenSSL 3.5): `openssl s_client -tls1` / `-tls1_1` on 5349 get `alert protocol version`, TLS 1.2 and 1.3 work.
- DH: `dh_key_size = DH_2066` by default.
- Loopback: `allow_loopback_peers = false` by default, enabled only by `--allow-loopback-peers`.
- `no-ipv6`: no such option in either 4.18.0 or 4.9.0 (the old image also logs `Bad configuration format`, and the IPv6 listener opens anyway). Removed, behavior does not change.

## Risks / Trade-offs

- [The fork image lacks a required tool other than `openssl`] → before publishing, build the image locally from `3rdparty/coturn` and run the `coturn-docker` container on it.
- [The ghcr package is private after the first publication → `docker pull` on servers fails] → after the first release check visibility and make it public manually.
- [The 4.9.0 → 4.18.0 jump changes behavior not covered by the template] → check startup and logs on the local image; rollback is simple (see below).
- [Actions in the fork are disabled by default] → enable Actions in the fork settings before the first tag (current state not checked).
- [HumbleNet CI with `submodules: recursive` clones coturn over SSH] → the other submodules already use SSH, so CI can do it; extra clone time is small.

## Migration Plan

1. Add the submodule, write the workflow in the fork, build the image locally, check `coturn-docker` on the local tag.
2. The user merges the branch into the fork `master`, enables Actions, disables all upstream workflows except `Release image`, pushes tag `release/4.18.0-1`.
3. Check that `ghcr.io/caiiiycuk/coturn:4.18.0-1` can be pulled without login (make the package public if needed).
4. Move the submodule to the tag commit, commit in HumbleNet.
5. On servers: `git pull`, `docker compose build --pull`, `docker compose up -d`.

Rollback: restore `FROM coturn/coturn:4.9.0-r0` and the old template (`git revert`), then `docker compose build --pull && docker compose up -d`.
