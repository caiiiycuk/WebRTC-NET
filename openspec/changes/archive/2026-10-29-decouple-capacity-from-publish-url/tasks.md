# Tasks

## 1. History independent of the URL

- [x] 1.1 In `coturn-docker/publisher.py` accept a valid saved binding of the same domain without comparing its URL with the current `PUBLISH_URL`; keep the accepted structure for the subsequent checks and writes. Add to `test_publisher.py` checks for differing URLs with a zero and a positive barrier, file preservation at load, consecutive updates, and a restart; run `python3 -m unittest discover -s coturn-docker -p 'test_publisher.py'` from the repository root.
- [x] 1.2 Verify delivery to the current URL with the existing local HTTP fixture while the saved address differs. The test must confirm `unknown` on an equal positive watermark after a restart, the transition to `known` only on a newer healthy snapshot, the watermark saved before sending, and the historic binding unchanged; run the `test_publisher.py` suite.
- [x] 1.3 Extend or reuse the negative tests for another domain, wrong binding and watermark types, file corruption/loss/external change, a durable write failure, invalid metrics, and a non-HTTPS address. Verify that all of them keep the previous bans on `known`, and that the publisher tests pass completely.
- [x] 1.4 Update `coturn-docker/README.md`: a URL change does not require state editing, the saved URL is historic, the barrier is not reset, a rollback to the old publisher may disable capacity again. Check the description against the tests and exclude recommendations to delete state or reinitialize an existing history.

## 2. Integration check

- [x] 2.1 Run all existing `coturn-docker/test_*.py` via `python3 -m unittest discover -s coturn-docker -p 'test_*.py'`; confirm compatibility with the existing lifecycle tests without including entrypoint changes in this change.
- [x] 2.2 Run the mandatory builds from `AGENTS.md` of the existing configured Linux and WebAssembly/JavaScript targets, including `ninja -j32 webrtcnet` in `emscripten`; record the results and fix errors related to the change before finishing.
- [x] 2.3 Run `openspec validate decouple-capacity-from-publish-url --strict`, check the diff for the absence of Cloud changes, production server changes, history resets, and unrelated user edits; all checks must pass.
