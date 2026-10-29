# Spec Delta

## ADDED Requirements

### Requirement: Capacity history is independent of the publish address

The publisher SHALL accept a valid saved capacity history of the same `DOMAIN` regardless of whether the current `PUBLISH_URL` matches the saved `binding.publishUrl`. It MUST keep `attemptedThroughMs`, send publications to the currently configured URL, and not require manual history editing or reinitialization on a URL change. Existing version 1 files SHALL remain readable without a prior migration.

#### Scenario: URL change with an existing history
- **WHEN** the publisher starts with a different valid HTTPS `PUBLISH_URL`, and a version 1 file contains the history of the same `DOMAIN` and a positive watermark
- **THEN** the history is accepted without a watermark reset
- **AND** publications go to the current `PUBLISH_URL`
- **AND** the URL change alone does not disable capacity

#### Scenario: History has no capacity attempts yet
- **WHEN** a valid file of the same domain contains `attemptedThroughMs=0` and a different saved URL
- **THEN** the publisher can publish a fresh valid `known` without reinitialization

### Requirement: URL change keeps capacity history protection

On a URL change the publisher MUST keep the checks of the domain, version, valid integer watermark, file integrity, and external state changes. It SHALL keep the existing checks of HTTPS, metrics freshness, quotas, and saving the watermark before sending `known`. After a restart with a positive watermark the publisher MUST publish `unknown` until there is a healthy snapshot with a strictly larger watermark; a URL change does not allow re-announcing an old snapshot as `known`.

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
- **THEN** the publisher disables `known` by the existing rules
- **AND** allowing a URL setting change does not cancel the file integrity control

#### Scenario: Unfit source or address
- **WHEN** the new address fails the HTTPS check or the metrics fail the existing freshness and quota checks
- **THEN** the URL change does not allow bypassing the corresponding check
