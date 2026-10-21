# Tasks

## 1. Fork submodule

- [x] 1.1 Add the submodule `git submodule add git@github.com:caiiiycuk/coturn.git 3rdparty/coturn` and check that `.gitmodules` has an entry in the same style as the others and `git submodule status` shows `3rdparty/coturn`
- [x] 1.2 Check that HumbleNet builds are not affected by `3rdparty/coturn`: all project targets and `cd emscripten && ninja -j32 webrtcnet` build

## 2. Publishing workflow in the fork

- [x] 2.1 In `3rdparty/coturn` create branch `ghcr-release` and add `.github/workflows/release-image.yml` (trigger only `push: tags: ['release/*']`, `linux/amd64`, `docker/coturn/debian/Dockerfile`, tag `ghcr.io/caiiiycuk/coturn:${GITHUB_REF_NAME#release/}`); validate the YAML with `actionlint` or `python3 -c 'import yaml; yaml.safe_load(open(...))'`
- [x] 2.2 Commit and push branch `ghcr-release` to the fork; check that the branch is visible in `git ls-remote origin ghcr-release`

## 3. Local image check

- [x] 3.1 Build the image locally: `docker build --platform linux/amd64 -f docker/coturn/debian/Dockerfile -t ghcr.io/caiiiycuk/coturn:4.18.0-1 3rdparty/coturn`; check that the image contains `turnserver` and `turnutils_stunclient`
- [x] 3.2 Confirm in the sources `3rdparty/coturn/src/apps/relay/mainrelay.c` that in 4.18.0 CLI and TLSv1/1.1 are off by default and loopback peers are forbidden; record the result in design.md (decision 5)

## 4. Switch coturn-docker to the fork image

- [x] 4.1 In `coturn-docker/Dockerfile` replace `FROM` with `ghcr.io/caiiiycuk/coturn:4.18.0-1` and add `openssl` to `apt-get install`; check `docker compose build` in `coturn-docker/` on the locally built image
- [x] 4.2 Remove the lines `no-tlsv1`, `no-tlsv1_1`, `dh2066`, `no-loopback-peers`, `no-cli`, `no-ipv6` from `coturn-docker/turnserver.conf.template`; check that `grep` does not find them
- [x] 4.3 Run the coturn container with a test `.env` and check: healthcheck `healthy`, no `Bad configuration format` and `deprecated` in logs, `curl -s localhost:9641/metrics` contains `turn_total_traffic_rcvb` and `turn_total_traffic_sentb`, entrypoint reports no errors about `openssl`/`curl`/`envsubst`
- [x] 4.4 Update `coturn-docker/README.md`: where the image comes from, release procedure (merge into fork `master` → tag `release/<ver>-<N>` → wait for the workflow → update `FROM` and submodule), the rule "submodule commit = tag commit", updating servers via `docker compose build --pull`; check that README has no `coturn/coturn:4.9.0` mentions left

## 5. Release (manual, user)

- [x] 5.1 Merge `ghcr-release` into fork `master`, enable Actions, disable all upstream workflows except `Release image` (`gh workflow disable <workflow> -R caiiiycuk/coturn`); check `gh workflow list -R caiiiycuk/coturn`
- [x] 5.2 Push tag `release/4.18.0-1` and check that the workflow passed and `docker pull ghcr.io/caiiiycuk/coturn:4.18.0-1` works without login (make the package public if needed)
- [x] 5.3 Move `3rdparty/coturn` to the commit of tag `release/4.18.0-1`, commit in HumbleNet; check that `git -C 3rdparty/coturn describe --tags` prints `release/4.18.0-1`
