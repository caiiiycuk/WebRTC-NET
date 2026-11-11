# Design

## Context

Motivation is in `proposal.md`. Today `Publisher.capacity()` in `coturn-docker/publisher.py:183-216` re-reads `capacity_state.json` on every cycle and compares it as a whole with the expected dictionary `{"version": 1, "binding": self.binding, "attemptedThroughMs": self.attempted}`. Any `OSError`/`ValueError` on read or any mismatch → `self.recovered = False`, after which the method always returns `None` (capacity is not published) until the process restarts; there are no stderr messages. The outer `except (ValueError, OSError)` around the snapshot and `durable_write` sets `blocked = True` for the cycle.

The deadline is implemented with `signal.setitimer` and a SIGALRM handler that raises `TimeoutError` at an arbitrary point of the interpreter. `TimeoutError` inherits from `OSError`, so both `except` clauses in `capacity()` catch the deadline as an I/O error. `durable_write` does `mkstemp` → `fsync` → `os.replace` → `fsync` of the directory; a deadline after `os.replace` leaves the file ahead of memory.

The existing test `test_total_scrape_and_post_deadlines` checks that the deadline interrupts `durable_write` (replaced with `time.sleep(10)`) and `cycle()` ends with `TimeoutError` in under 5.5 s; that behavior must be preserved. The test `test_external_history_change_disables_known_regardless_of_url` fixes that a `binding` change in the file disables `known` permanently; it stays as well.

New York host: 1 vCPU, 458 MB, swap, the deadline fires regularly; file read errors under memory pressure are likely.

## Goals / Non-Goals

**Goals:**
- A deadline at any point of `capacity()` is a cycle error, not a history error.
- The process's own write, interrupted before memory was updated, is not treated as an external change.
- A transient read error does not disable capacity permanently.
- Run-time history disablement is visible in the log.

**Non-Goals:**
- Do not change the state file format, the publication protocol, the startup recovery rules in `__init__` (its file check is only extracted into a shared function), `durable_write`, `cycle()`, `main()`, the deadline, or socket timeouts.
- Do not weaken the protection against a barrier rollback and a `binding` change.
- Do not add signal masking, threads, new environment variables, or dependencies.
- Do not touch cloud (15 s capacity TTL) or `entrypoint.sh`.

## Decisions

### `TimeoutError` is re-raised from `capacity()`

In both `except` clauses of `capacity()`, `except TimeoutError: raise` is added before `except (OSError, ValueError)`. The deadline reaches `cycle()` → `main()` prints `cycle failed: TimeoutError: cycle deadline`, sets `blocked = True`, the history is untouched. Today a deadline inside `capacity()` turns into a silent `unknown` publication with `HTTP 200` — after the change it is visible in the log.

The alternative — masking SIGALRM (`pthread_sigmask`) around `durable_write` and the memory update — was rejected: a hung `fsync` would become uninterruptible, and the existing test of `durable_write` being interrupted by the deadline would break.

### A barrier from the file is accepted if it is ahead and the `binding` is the same

The "as a whole" comparison is replaced with parsing the file using the same checks as in `__init__` (version, `binding` shape, domain, type and range of `attemptedThroughMs`), then:

- `binding` differs, content is invalid, or the barrier in the file is smaller than the remembered one → `recovered = False`, one message to stderr, then as before (`None`).
- the barrier in the file is larger than the remembered one → `self.attempted = attempted_from_file`; the cycle continues with the usual logic (`watermark > self.attempted` → write and `known`, otherwise `unknown` with the accepted barrier).
- equal → no change.

This is safe by the barrier invariant: the only writer of the file is this process (`publisher.lock`), and a barrier "forward" only means we may already have sent a snapshot no newer than it; accepting it means becoming more conservative, not more liberal. A rollback and a `binding` change still close publication: those are exactly what can lead to re-sending an old snapshot under someone else's history.

The alternative — updating memory before writing the file — was rejected: on a write error memory moves ahead of the file, and after a restart the barrier rolls back; on an interruption between the operations it is unknown which side is ahead.

### A read error is a block for one cycle, not forever

`OSError` on file read (except `TimeoutError`) → `blocked = True` for this cycle, `recovered` and `attempted` do not change → `unknown` with the previous barrier is returned. If the file really disappeared, every next cycle repeats the block — the same outcome as "forever", but without depending on a restart. If the file came back with valid content, the "forward only" rules apply.

### One message on disablement

When `recovered` turns `False` at run time, `ice-publisher: capacity history unavailable: <reason>` is printed to stderr once; subsequent cycles with `recovered == False` do not read the file and do not print. The reason is a short string without the file contents and without environment values (for example `history changed`, `invalid state`, the exception class). For an accepted barrier forward no message is printed: it is normal self-recovery.

### Tests without a real timer

The behavior is reproduced deterministically in `test_publisher.py` with patches, as in the neighboring tests:
- the history read raises `TimeoutError` (patch `Path.read_text` on the `state_path` object or on `publisher.Path.read_text` with a condition on the file name) → `capacity()` re-raises `TimeoutError`, the next call with a newer snapshot returns `known`;
- `durable_write` is replaced with a wrapper that performs the real write and then raises `TimeoutError` → the file is ahead of memory; the next `capacity()` with the same snapshot gives `unknown` with the barrier from the file, with a newer one — `known`;
- the file is overwritten with a barrier larger/smaller than the remembered one and with a different `binding` → acceptance, disablement, disablement respectively; one line in the captured stderr and its absence on repeated cycles is checked;
- `read_text` raises `OSError` once → `unknown`, then `known`.

The existing `test_total_scrape_and_post_deadlines`, `test_write_failure`, `test_external_history_change_disables_known_regardless_of_url`, `test_durable_barrier_before_send_restart_and_lost_ack` must pass without edits.

## Risks / Trade-offs

- A barrier "forward" is accepted from any writer of the file → an external write of a larger barrier only delays `known` until a newer snapshot; this is documented in the README as a tolerated but not recommended intervention.
- A repeating read error every 5 s is indistinguishable in the log from a one-off → nothing is printed on each block for this reason, to avoid log noise; diagnosis is by the absence of `capacity` in cloud and by `stat` of the file, as in this incident. If needed, add a counter in a separate change.
- A deadline re-raised from `capacity()` skips `update_traffic` in that cycle → losing one traffic update is acceptable: the counters are taken from cumulative metrics and catch up on the next cycle.
- The file check in `__init__` and in `capacity()` must be the same → extract the file validation into one function so the rules do not diverge.

## Migration Plan

There is no state migration: the file format and `binding` do not change. The change reaches the servers with the next dzcoturn archive release and container re-creation with `coturn-data` preserved; the release and deployment are outside this change. Rollback is the previous version of `publisher.py` with the same history file. Before the rollout to New York, with `capacity: null` for longer than 30 s and `HTTP 200` in the log, a `kill` of the publisher process is enough: the entrypoint restarts it in 5 s and the history is restored from the file.
