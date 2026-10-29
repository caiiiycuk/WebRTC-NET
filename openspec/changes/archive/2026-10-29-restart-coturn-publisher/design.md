# Design

## Context

The motivation is described in `proposal.md`. In `coturn-docker/entrypoint.sh:70-71` the certificate watcher and the publisher are started in the background. The main loop waits only for `turnserver`. The Compose policy `restart: unless-stopped` applies to the container, and the healthcheck only checks STUN.

The cause of the exit was established by monitoring from 2026-10-23 to 2026-10-29 (see `proposal.md`): `main()` in `publisher.py` limits the cycle with `signal.setitimer(ITIMER_REAL, 5)` and a SIGALRM handler raising `TimeoutError`, but `try/except Exception/finally` covers only the `cycle()` call. The Python handler runs at the interpreter's next signal check, so a timer that fires during `except` (printing `cycle failed`) or in `finally` before `setitimer(0)` raises `TimeoutError` outside the `try` → the exception leaves `main()` → `startup failed`, code 1. The same asynchronous `TimeoutError` inside the `HTTPResponse` constructor produces "Exception ignored … has no attribute 'fp'" (8 times on New York); that case is caught. On New York ~15% of cycles miss the deadline because of CPU and RAM shortage, so the race triggers ~10 times a day; on Paris the deadline fired 9 times in 6 days without exits.

`publisher.py` already handles errors of individual cycles, uses a process lock, and stores capacity/traffic in `/var/lib/coturn`. The `test_publisher.py` tests check the scrape/POST deadlines and the `main()` loop with a stub `cycle(deadline)`. The current `test_publisher.py` tests also check state recovery after a process exit, but not child process management from the entrypoint. The Dockerfile already contains Bash and Python; no extra packages are required.

## Goals / Non-Goals

**Goals:**
- Control process exit from outside Python, including SIGKILL and startup errors.
- Keep TURN connections independent of publisher restarts.
- Explicitly verify stopping with an active process and during the pause.
- Prevent publisher exit because of a deadline `TimeoutError` raised outside `cycle()`, without changing the deadline itself.

**Non-Goals:**
- Do not change the 5 second deadline, socket timeouts, network error handling, payload, publication period, or capacity rules inside the publisher.
- Do not add a supervisor package, a separate service, a hang watchdog, or new environment variables.
- Do not fix or include unrelated working tree changes in the change; the `main()` fix is applied on top of the uncommitted `publisher.py` changes.
- Do not address the New York overload (VM resources, `-v` on turnserver).

## Decisions

### Supervision in the existing Bash entrypoint

Replace the one-shot publisher start with a background loop: start Python, wait for exit, log the status, pause 5 seconds, and repeat. Handle the wait result explicitly so that `set -e` does not end the loop on a non-zero code. The start condition remains a non-empty `PUBLISH_URL`.

Restarting the whole container is easier to delegate to Docker, but it would drop TURN connections. Restarting from Python itself does not cover SIGKILL. A separate supervisor adds a dependency for one process.

### Stopping takes priority over restarting

The loop must handle TERM/INT correctly, terminate and wait for its current child process, including `sleep`, and not resume after stopping. Keep the child PID and the loop PID only where needed for stopping and `wait`. Align this with the existing entrypoint trap: stopping must not leave Python, the loop, or the timer behind and must not create recursive signal fan-out. Also verify the existing exit on an unexpected coturn exit and the restart on certificate renewal.

Do not consider signaling only the background Bash sufficient: the signal must reach Python and the pause wait. Do not rely on Docker's forced SIGKILL as the regular stop mechanism.

During implementation it turned out that the existing trap `kill 0; wait; exit 0` in the entrypoint creates recursion by itself: `kill 0` sends TERM to the shell itself as well, `wait` inside the handler is interrupted by that signal, the handler runs nested, and so on until the stack overflows and bash exits with SIGSEGV (reproduced with the entrypoint from HEAD under bash 5.3 on the host). The same pattern is in the unexpected coturn exit branch. Solution: a shared `stop_all` function that sets `trap '' TERM INT` before `kill 0`, then `wait`; both the trap and the coturn exit branch use it.

### Fixed delay and short log

Use a 5 second pause without configuration or an exponential scheme. This limits the start rate under a persistent error and does not change the period of successful publications. Write the exit status and the restart message in English, without printing the environment, payload, or TURN_SECRET.

### State remains the publisher's responsibility

Start the same command again without removing lock/state and without `--initialize-new-identity`. The existing lock excludes concurrent senders. After a restart the previous capacity recovery rules apply, including the temporary `unknown`. The `publisher.py` change is limited to the `main()` loop and message texts.

### The deadline is caught at the level of the whole iteration

Wrap the whole body of the `while True` iteration in `publisher.py` — arming the timer, `cycle()`, its `except`/`finally` with `setitimer(0)`, and the pause — in an outer `try/except TimeoutError`. The timer is one-shot (no interval), so one `TimeoutError` is possible per iteration; wherever it is raised, it stays inside the iteration, `blocked` is already set by then, and a skipped pause does not speed up the loop because the deadline has already passed. The next iteration starts with `try` again.

Rejected alternatives: dropping SIGALRM in favor of socket timeouts only does not cover `getaddrinfo` and breaks the existing deadline tests; a flag instead of an exception in the handler does not interrupt blocking calls; `except TimeoutError` only around `print` does not cover `finally` and the pause.

Extend the `cycle failed` and `startup failed` messages with `type(error).__name__` and the exception text. The exception texts of `urllib`/`OSError`/`ValueError` in this code do not contain `TURN_SECRET` or the payload; the publish URL is not a secret.

### Test the real entrypoint with stub commands

Add `coturn-docker/test_entrypoint.py` on the standard `unittest`/`subprocess`. Run the real entrypoint in a separate process group with controlled replacements of external commands via PATH; temporary files must exclude writes to the real `/etc` and `/var/lib/coturn`. Do not add production variables for tests and do not copy the loop into the test.

Isolation from the real `/etc`, `/var/lib`, and `/var/run` without variables in the entrypoint: the test runs `unshare -rm` and inside it mounts an overlay with an upperdir in a temporary directory over `/etc` and `/var/lib` and a tmpfs over `/run`, then `exec bash entrypoint.sh`. This needs unprivileged user namespaces and overlay in a userns (kernel ≥ 5.11); it works on the development host. The stub `sleep` shortens only the 60 second certificate poll to 1 second; the 5 second restart pause stays real.

The stub publisher records starts, PIDs, and exit moments; the stub coturn stays alive. Check exit 0/error, SIGKILL, repeated failures, missing PUBLISH_URL, TERM/INT in both states. Bound the waits and guarantee process group cleanup even when an assert fails. The existing `test_publisher.py` suite confirms that the state rules are preserved. Add a check in local Docker of the actual SIGTERM delivery from PID 1.

### Deadline race test without a real timer

Reproduce the race deterministically in `test_publisher.py` on the skeleton of the existing `main()` loop test: the stub `cycle(deadline)` raises `Exception`, and the replaced `sys.stderr` output raises `TimeoutError` while printing `cycle failed`; after that the loop must run the next iteration instead of leaving `main()`. Do not use the real `setitimer`/SIGALRM in the test; the hit is not reproducible reliably (a local attempt over 2000 iterations produced no hit).

## Risks / Trade-offs

- A permanently invalid configuration will be restarted forever → the fixed pause and the status in the log allow diagnosing the failure without stopping TURN.
- Signal races between start, `wait`, and `sleep` may leave a process or cause an extra start → separate TERM/INT tests and a Docker smoke check.
- An alive but stuck publisher is not recovered → this is an explicit boundary of the change; a watchdog needs a separate request.
- The Paris exit on 2026-10-22 did not reproduce in 6 days of monitoring → presumably the same race (the deadline fires rarely there); the restart loop in the entrypoint also covers unconfirmed causes.
- New York is overloaded: ~15% of cycles with `cycle failed` and `blocked=True` → after the fix the process does not exit, but capacity is regularly published as blocked; VM resources are outside the change.
- The outer `except TimeoutError` hides a deadline raised outside `cycle()` without a separate message → `cycle failed` with the exception class is already printed or `blocked` is set inside the iteration; a repeated message is not needed.

## Migration Plan

There is no state migration. After a separate deployment approval, rebuild the thin image and recreate the container keeping `coturn-data`; verify publications and killing only the publisher. Recreating the container interrupts current TURN connections, so agree on a window. Do not change the servers within this change. Since 2026-10-23 Paris and New York run temporary restart loops started via `docker exec` and writing to `/var/lib/coturn/publisher.monitor.log`; they disappear when the container is recreated, the log in the volume remains and can be deleted. To roll back, restore the previous entrypoint/image keeping the volume; automatic publisher recovery and the race fix disappear with the rollback.
