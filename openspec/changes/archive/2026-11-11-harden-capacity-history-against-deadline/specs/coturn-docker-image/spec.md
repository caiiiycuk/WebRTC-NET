# Spec Delta

## ADDED Requirements

### Requirement: Cycle deadline does not disable the capacity history

A cycle deadline firing while the capacity history is read or written SHALL end only the current cycle as a cycle error and MUST NOT move the history into the "unavailable" state. The next cycle SHALL use the same saved history again without a process restart. A transient history file read error unrelated to the deadline SHALL block `known` publication only in the current cycle.

#### Scenario: Deadline during the history read
- **WHEN** the cycle deadline fires while the publisher reads and verifies the saved capacity history
- **THEN** the cycle ends with a `cycle failed` message containing the exception class and text, and capacity in this cycle is not published as `known`
- **AND** the next cycle with a newer healthy snapshot publishes `known` without a process restart

#### Scenario: Deadline after the barrier write
- **WHEN** the cycle deadline fires after the new barrier is already written to the history file but before the publisher remembered it in memory
- **THEN** the cycle ends as a cycle error, and the next cycle accepts the written barrier and publishes `known` on a newer healthy snapshot

#### Scenario: Transient read error
- **WHEN** reading the history file fails with an error unrelated to the deadline in one cycle and succeeds in the next, with a positive remembered barrier
- **THEN** in the first cycle capacity is published as `unknown` with the previous barrier, and in the next one as `known` on a newer healthy snapshot

### Requirement: History barrier is accepted only forward

If the history file contains the same `binding` and a barrier `attemptedThroughMs` strictly larger than the one remembered in memory, the publisher SHALL accept the barrier from the file as the current one and continue publishing by the usual rules. If the `binding` differs, the barrier is smaller than the remembered one, or the file content is invalid, the publisher MUST stop `known` publication until the process restarts, as before. Accepting a barrier forward MUST NOT lead to publishing a snapshot with `observedAtMs` not exceeding the accepted barrier.

#### Scenario: Barrier in the file ahead of memory
- **WHEN** the history file with the same `binding` contains a barrier larger than the one remembered in memory
- **THEN** the publisher accepts the barrier from the file, publishes `unknown` with that barrier for a snapshot not newer than it and `known` for a strictly newer snapshot

#### Scenario: Barrier in the file behind memory
- **WHEN** the history file with the same `binding` contains a barrier smaller than the one remembered in memory
- **THEN** the publisher stops `known` publication until the process restarts and does not overwrite the file

#### Scenario: Binding changed
- **WHEN** the `binding` in the history file changed
- **THEN** the publisher stops `known` publication until the process restarts, as before the change

### Requirement: History disablement is diagnosable

When the publisher in a running process moves the capacity history into the "unavailable" state, it SHALL write to stderr once a message with the reason and the exception class without secrets and publication contents. It MUST NOT repeat the message in every subsequent cycle.

#### Scenario: History disabled at run time
- **WHEN** the history file check in a cycle finds a different `binding`, a barrier behind memory, or invalid content
- **THEN** one line about the history being unavailable with the reason appears in stderr, and subsequent cycles without a state change do not add new lines

## MODIFIED Requirements

### Requirement: URL change keeps capacity history protection

On a URL change the publisher MUST keep the checks of the domain, version, valid integer watermark, file integrity, and external state changes. It SHALL keep the existing checks of HTTPS, metrics freshness, quotas, and saving the watermark before sending `known`. After a restart with a positive watermark the publisher MUST publish `unknown` until there is a healthy snapshot with a strictly larger watermark; a URL change does not allow re-announcing an old snapshot as `known`. An external file change with the same `binding` and a barrier strictly larger than the remembered one is accepted as the new barrier by the "forward only" rule; other external changes disable `known`.

#### Scenario: Restart with a positive watermark
- **WHEN** after a URL change the publisher restored a positive watermark and a healthy source has not yet provided a strictly newer mark
- **THEN** capacity stays `unknown` with the saved barrier
- **AND** when a strictly newer healthy snapshot appears, its watermark is durably saved before `known` is sent

#### Scenario: History belongs to another domain
- **WHEN** the domain of the saved history does not match the current `DOMAIN`
- **THEN** `known` publication is disabled regardless of the URL
- **AND** the history is not reinitialized automatically

#### Scenario: History is missing or corrupt
- **WHEN** the file is missing, corrupt, or contains an invalid version or watermark
- **THEN** the publisher does not create a new history automatically and does not publish `known`
- **AND** ICE and traffic can be published by the existing rules

#### Scenario: File change at run time
- **WHEN** the saved state changed externally after the publisher accepted it
- **THEN** with a different `binding`, a barrier smaller than the remembered one, or invalid content the publisher disables `known` by the existing rules
- **AND** with the same `binding` and a barrier strictly larger than the remembered one the publisher accepts the barrier from the file by the "forward only" rule
- **AND** allowing a URL setting change does not cancel the file integrity control

#### Scenario: Unfit source or address
- **WHEN** the new address fails the HTTPS check or the metrics fail the existing freshness and quota checks
- **THEN** the URL change does not allow bypassing the corresponding check
