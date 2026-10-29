# Proposal

## Why

On Paris and New York the `publisher.py` process was missing while coturn kept running and the container status stayed `healthy`; `entrypoint.sh` does not recover an exited publisher, so the server stopped refreshing its registration in cloud. Docker logs do not keep the cause: turnserver with `-v` writes ~10 MB per 10 minutes under `max-size 10m`/`max-file 3`, so only the last ~30 minutes are retained.

Monitoring from 2026-10-23 to 2026-10-29 (a temporary restart loop via `docker exec`, log `/var/lib/coturn/publisher.monitor.log` in the volume) established the cause. On New York the publisher exited with code 1 68 times in 6 days, each time after `ice-publisher: startup failed`; only the outer `except` in `__main__` prints that message, so the exception left `main()` past the cycle handler. Twice `cycle failed` and `startup failed` were glued into one line without a newline: the `TimeoutError` from the SIGALRM handler (`signal.setitimer(..., 5)`) was raised while printing `cycle failed`, i.e. inside the `except` block, before the timer was cancelled in `finally`. The timer fires at the cycle boundary when the host is overloaded: New York (1 vCPU, 458 MB RAM, ~350 MB swap) misses the 5 second deadline in ~15% of cycles. On Paris the deadline fired 9 times in 6 days without exits; the original exit there on 2026-10-22 did not reproduce and is attributed to the same race only tentatively. Recovery from the entrypoint does not remove the crash on New York; the `main()` loop in `publisher.py` needs a fix.

## What Changes

- With `PUBLISH_URL` set, automatically start the publisher again after it exits with any code or signal, with a 5 second pause.
- Recover only the publisher, without interrupting the running `turnserver` and client connections.
- When the container stops, end the publisher and its restart loop without new starts.
- Log the exit code and a restart message without secrets.
- Remove the race in `publisher.py`: a deadline `TimeoutError` raised outside `cycle()` (in `except`, `finally`, or the pause) must not end the process; the 5 second deadline and SIGALRM stay.
- Add the exception class and text to the `cycle failed` and `startup failed` messages so the next diagnosis is not blind.
- Add lifecycle and deadline race checks, describe the behavior in the README.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `coturn-docker-image`: extend the container behavior with requirements for publisher recovery and stopping it together with the container.

## Impact

- Main changes: `coturn-docker/entrypoint.sh` and the `main()` loop with error messages in `coturn-docker/publisher.py`; checks: a deadline race test in `test_publisher.py` and a separate lifecycle test next to it; documentation: `coturn-docker/README.md`.
- Unchanged: cloud API, publication format, secrets, capacity/traffic files, the single-publisher lock, the 5 second cycle deadline, and socket timeouts.
- No new dependencies, separate container, or restart settings are needed.
- The change does not include deployment to Paris/New York, more resources for New York, or recovery of a stuck but alive publisher.
- Existing uncommitted changes to `publisher.py`, `test_publisher.py`, README, and other files are preserved; the loop fix is applied on top of them.
