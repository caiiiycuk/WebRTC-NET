# Tasks

## 1. Capacity history resilience in `publisher.py`

- [x] 1.1 Extract the parsing and validation of `capacity_state.json` (version, `binding` shape, domain, type and range of `attemptedThroughMs`) from `Publisher.__init__` into one function and use it in `__init__` and in `capacity()`; check that `python3 -m unittest -v test_publisher.py` passes without changes to the existing tests.
- [x] 1.2 In `capacity()` add `except TimeoutError: raise` before both `except (OSError, ValueError)`; in `test_publisher.py` add a test: the history read raises `TimeoutError` → `capacity()` re-raises it, the next call with a newer snapshot returns `known`; `durable_write` is replaced with a wrapper "real write, then `TimeoutError`" → the next `capacity()` with the same snapshot gives `unknown` with the barrier from the file, with a newer one — `known`.
- [x] 1.3 Replace the "as a whole" file comparison with the "forward only" rules: a different `binding`, invalid content, or a barrier smaller than the remembered one → `recovered = False`; a larger barrier → accept into memory; equal → no change. Test: three variants of overwriting the file (larger, smaller, different `binding`) with the expected `known`/`unknown`/`None` and an unchanged file on disablement; the existing `test_external_history_change_disables_known_regardless_of_url` and `test_durable_barrier_before_send_restart_and_lost_ack` pass.
- [x] 1.4 Turn a file read error (`OSError`, except `TimeoutError`) into a block for one cycle without changing `recovered`/`attempted`; test: `read_text` raises `OSError` once → `unknown` with the previous barrier, the next cycle with a newer snapshot → `known`.
- [x] 1.5 When `recovered` turns `False` at run time, print one line `ice-publisher: capacity history unavailable: <reason>` to stderr without the file contents and the environment; test with captured stderr: exactly one line on disablement and none on subsequent calls; no lines when a barrier forward is accepted.
- [x] 1.6 Update the capacity history section in `coturn-docker/README.md` (English): the deadline does not disable the history, a barrier from the file is accepted only forward, a transient read error blocks one cycle, the format of the disablement message; check the description against the tests. Do not touch the sentence about the metrics bind to `127.0.0.1` in this section — it is changed by the change `fix-dzcoturn-probes-and-metrics`.

## 2. Conntrack recipe in the README

- [x] 2.1 In the host preparation section of `coturn-docker/README.md` add `echo nf_conntrack > /etc/modules-load.d/nf_conntrack.conf` before `sysctl --system`, a check `sysctl net.netfilter.nf_conntrack_max net.netfilter.nf_conntrack_count` after it, and a warning that without the loaded module the `nf_conntrack_max` key is silently skipped; for a VM with less than 1 GB of memory give the guideline 65536 (≈256 bytes per entry). Check that the README commands run as written (`bash -n` on the extracted block).

## 3. Integration check

- [x] 3.1 From `coturn-docker` run `python3 -m unittest -v test_publisher.py test_entrypoint.py`; confirm that the deadline, blocking, external history change, and URL change checks pass without changing the state file format.
- [x] 3.2 Run the mandatory build checks from `AGENTS.md`: the build of all project targets and `ninja -j32 webrtcnet` from `emscripten`; record the result and the exact environment blockers if a run is impossible.
- [x] 3.3 Check the final diff with `git diff` and `git status --short`: only `publisher.py` (the `capacity()` method and the extracted file check), `test_publisher.py`, the README, and the change artifacts; no new dependencies, no edits to `entrypoint.sh`, the file format, or cloud, no deployment to servers.
