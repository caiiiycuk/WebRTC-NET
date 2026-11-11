# Proposal

## Why

On 2026-10-29 on New York (dzcoturn.9) the cloud returned `capacity: null` for `newyork.dos.zone` continuously for 26 minutes, although the publisher received `HTTP 200` every 5 seconds and `traffic_stats` kept updating: `capacity_state.json` stopped changing at 13:54:25 and came back only after a manual `kill` of the publisher (the entrypoint restarted it after 5 seconds). The turnserver metrics were fresh, disk and inodes were fine, `durable_write` worked for `traffic_stats` with the same function in the same directory → the only path with no write and no log line is the `recovered = False` branch in `Publisher.capacity()`: any error reading `capacity_state.json` or any mismatch with memory permanently disables capacity until the process restarts, printing nothing. The likely trigger (not proven, the docker logs for 13:54 had rotated): `TimeoutError` is a subclass of `OSError`, so the SIGALRM deadline landing inside the state-file read block is caught by `except (OSError, ValueError)` as a history error. The same deadline landing inside `durable_write` between `os.replace` and the return leaves the file ahead of memory → on the next cycle "history changed" → the same irreversible disablement. On New York the deadline fires up to several times per minute under load, so the disablement is a matter of time.

The root cause of the load that day was a conntrack table overflow on the host (`nf_conntrack_max` effectively 4096 with 262144 in `/etc/sysctl.d/99-coturn.conf`: the `nf_conntrack` module loads after `systemd-sysctl`, the key is silently skipped). This was fixed on the host by hand; the recipe in the README reproduces the same mistake on any new server.

## What Changes

- The cycle deadline (`TimeoutError`) is no longer treated as a capacity history error: in `Publisher.capacity()` it is re-raised, the cycle ends as `cycle failed: TimeoutError`, and the history stays usable for the next cycle.
- A barrier in the file that moved ahead of memory with the same `binding` (the process's own write interrupted before memory was updated) is accepted as the new barrier rather than as an "external change". A barrier that moved backward, a different `binding`, and invalid content still disable known publication until restart.
- A transient read error of the state file (`OSError`, not `TimeoutError`) blocks only the current cycle and does not disable capacity permanently.
- The run-time transition of the history to "unavailable" is written to stderr as one line with the exception class; today it is silent.
- README (English): the history rules gain the description "a barrier forward is accepted, backward is not"; the host preparation recipe gains `/etc/modules-load.d/nf_conntrack.conf` and a check of `sysctl net.netfilter.nf_conntrack_max` after `sysctl --system`.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `coturn-docker-image`: adds requirements for the resilience of the capacity history to the cycle deadline and to transient read errors, for accepting a barrier that moved forward, and for the diagnosability of history disablement; refines the run-time external file change scenario in the requirement `URL change keeps capacity history protection`.

## Impact

- Code: `coturn-docker/publisher.py`, method `Publisher.capacity()` (handling of `TimeoutError`/`OSError` on history read and write, accepting a barrier forward, the stderr message). The file check from `__init__` is extracted into a shared function without changing the startup recovery rules; `durable_write`, `cycle()`, `main()`, the state file format, and the publication format do not change.
- Tests: `coturn-docker/test_publisher.py` — new checks for `TimeoutError` during read and after write, for a barrier forward/backward, for a transient read error; the existing deadline checks (`test_total_scrape_and_post_deadlines`) and external history change checks are kept.
- Documentation: `coturn-docker/README.md` — the capacity history section and the host preparation section (conntrack).
- Out of scope: the `dzcoturn.10.zip` release and migration notes, deployment to Paris/New York, New York VM resources, quota and "silent" allocations, changes in cloud (15 s TTL for capacity) and in `entrypoint.sh`.
