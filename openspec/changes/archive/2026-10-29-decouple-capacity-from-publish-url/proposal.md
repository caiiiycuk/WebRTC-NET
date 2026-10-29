# Proposal

## Why

On `anfit.pp.ua` a `PUBLISH_URL` change disabled capacity while the metrics were healthy: the publisher compares the delivery address with `binding.publishUrl` in the saved history. Changing the publish address of the same TURN domain must not require manual state editing or reinitialization.

## What Changes

- Do not disable capacity because the current `PUBLISH_URL` differs from the saved `binding.publishUrl`.
- Automatically accept an existing valid state file of the same `DOMAIN`, keeping `attemptedThroughMs` and the current `unknown`/`known` rules after a restart.
- Keep the file format and the domain, history integrity, external file change, HTTPS, and metrics checks.
- Add regression tests and clarify the README: the URL is a delivery address, not an identifier of the capacity history.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `coturn-docker-image`: define continued capacity publication after a URL change without resetting history and without weakening the other checks.

## Impact

- Code: `coturn-docker/publisher.py`; tests: `coturn-docker/test_publisher.py`; documentation: `coturn-docker/README.md`.
- The Cloud API, the capacity payload, dependencies, Docker Compose, and the TURN configuration do not change.
- Deployment, server restarts, and manual migration of production state are not part of the change.
- Uncommitted changes and the separate change `restart-coturn-publisher` are preserved; publisher process management does not change here.
