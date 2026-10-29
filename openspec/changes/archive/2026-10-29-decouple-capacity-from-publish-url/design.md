# Design

## Context

The motivation is described in `proposal.md`. In `coturn-docker/publisher.py` the address is part of `self.binding`; the constructor compares the whole saved binding with it. Before each capacity attempt the file is compared with the expected state as a whole. Replacing only the initial comparison is not enough: the next check would disable `known` again.

`attemptedThroughMs` is written atomically with fsync before `known` is sent. After a restart a positive watermark first means `unknown`; a healthy strictly newer snapshot allows `known`. The process lock prevents several publishers on one volume. These mechanisms are preserved.

The existing main spec does not define the identity of the capacity history. The delta adds separate requirements to the existing capability `coturn-docker-image`; it does not replace its requirement `Container behavior is preserved` and does not overlap with the process management in `restart-coturn-publisher`.

## Goals / Non-Goals

**Goals:**
- Separate the delivery address from the history identity without changing the file format.
- Keep the strict control of state changes at run time.
- Read an existing file with an outdated URL without writing at load and without resetting the barrier.

**Non-Goals:**
- Do not add alias lists, URL normalization, environment variables, or a migration command.
- Do not support hot reload of the environment or simultaneous delivery to several URLs.
- Do not move history between different `DOMAIN` values or independent publisher volumes.
- Do not change the metrics source, payload, timeouts, process lock, entrypoint, or Cloud.

## Decisions

### Check identity by domain, keep the original binding

At load, check the version 1 structure, the domain, and a valid integer watermark. The saved binding must have the previous structure with a domain and a string `publishUrl`, but its URL is not compared with the current environment. After a successful check, accept the saved binding as the expected binding of the file for this process.

The current `PUBLISH_URL` is still taken from the environment for HTTP requests and checked at startup. The saved URL is only history metadata; its availability, equality with the current address, and compliance with the current network requirements do not determine whether the history is usable.

The alternative of removing the field changes the format and needs a migration. Automatically rewriting the field at startup adds a write and handling of its failure without need. Keeping the previous binding solves the task with a smaller change.

### Do not weaken the repeated file check

Keep the existing comparison of the full saved state with the expected one, but use the binding accepted from disk rather than one constructed from the current URL. New watermark writes keep that same binding. This removes the repeated disabling caused by differing addresses and keeps detecting any external change of the accepted state, including a change of its URL.

Initialization of a genuinely new history stays the same: version 1, the current domain and URL, watermark 0. Reading and rewriting such a file after a URL change does not reset the watermark. Separate wrapper functions and a general migration system are not needed.

### Do not change recovery after a restart

An existing positive barrier stays positive even after an address change. An equal snapshot after a restart does not restore the lost frozen tuple and stays `unknown`; a strictly larger healthy watermark is saved before `known` is sent. Losing the file does not become a new identity.

### Verify real delivery and state persistence

In `test_publisher.py` use the existing metrics fixtures, a temporary directory, and the local HTTP fixture. Check differing URLs, several consecutive snapshots and a restart, delivery to the current address, preservation of the historic binding, and watermark growth. An equal positive watermark must give `unknown`, and the next healthy snapshot must give `known`.

Separately check another domain, wrong binding/watermark types, file corruption or deletion, its external change, a durable write failure, and the previous source/HTTPS checks. The test HTTP endpoint remains an internal fixture; the production HTTPS check is not weakened.

## Risks / Trade-offs

- The saved URL may differ from the real destination and confuse the operator → the README explicitly describes the field as history metadata; the actual address is determined by the environment.
- The URL may point to another backend with an independent history → moving its history is not promised; the local monotonic barrier is kept and is not reset for the sake of delivery.
- An old publisher version after a rollback will again require a matching URL → state the limitation in advance; do not delete state and do not restore an outdated backup over newer attempts for the sake of a rollback.
- The working tree already contains publisher, test, and documentation changes → apply only this small diff, do not overwrite other work, and do not include it automatically in the change.

## Migration Plan

No preliminary state migration is needed. With a separate deployment approval, update the publisher keeping the volume and restart it in the regular way. Verify the positive watermark, successful publications, and fresh capacity in Cloud. This change does not authorize a production deploy.

On rollback keep the current file with all attempts. If the saved URL differs from the current one, the old version disables capacity; do not work around that by resetting history. For a safe rollback use the fixed version or a separately agreed binding reconciliation after stopping the publisher, keeping the current barrier.
