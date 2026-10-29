# Spec Delta

## ADDED Requirements

### Requirement: Automatic publisher recovery

With a non-empty `PUBLISH_URL` the container SHALL automatically start the exited publisher again regardless of the exit code or the terminating signal. Between the exit and the next attempt there MUST be a 5 second pause, without a limit on the number of attempts. Recovery MUST NOT restart the running coturn or interrupt its client connections. With an empty or missing `PUBLISH_URL` the publisher MUST NOT start. The log SHALL contain the exit code and the message about the next attempt without secret values and publication contents.

#### Scenario: Publisher exits successfully or with an error
- **WHEN** the publisher exits with code 0 or a non-zero code while the container is running and publications are enabled
- **THEN** after a 5 second pause the container starts the publisher again, and the coturn process keeps running without a restart
- **AND** the log contains the exit code and the restart message without secrets

#### Scenario: Publisher is killed by a signal
- **WHEN** the publisher process receives SIGKILL while the container is running
- **THEN** after a 5 second pause a new publisher starts without restarting coturn

#### Scenario: Repeated startup failure
- **WHEN** the publisher exits right after each start
- **THEN** the attempts continue with a 5 second pause between the exit and the new start, without a continuous CPU-consuming loop

#### Scenario: Publications disabled
- **WHEN** the container starts with an empty or missing `PUBLISH_URL`
- **THEN** coturn runs without a publisher and without attempts to restart it

### Requirement: Cycle deadline does not end the publisher

The publisher SHALL keep the 5 second deadline per cycle and MUST keep running if the deadline `TimeoutError` is raised outside `cycle()`: while handling a cycle error, cancelling the timer, or in the pause before the next cycle. The cycle error and process exit messages SHALL contain the exception class and text without secret values and publication contents.

#### Scenario: Deadline fires while handling a cycle error
- **WHEN** the cycle failed with an error and the deadline `TimeoutError` is raised while printing `cycle failed` or before the timer is cancelled
- **THEN** the process runs the next cycle and does not exit

#### Scenario: Cycle error is diagnosable
- **WHEN** the cycle ended with an exception
- **THEN** the `cycle failed` message contains the exception class and text, and capacity in this cycle is published as blocked by the existing rules

### Requirement: Safe publisher lifecycle

The container SHALL stop the publisher and the restart wait on SIGTERM or SIGINT and MUST NOT start the publisher after stopping has begun. Automatic restart MUST preserve the existing single-publisher lock, the capacity/traffic files, and the state recovery rules; it MUST NOT delete or reinitialize them. External container termination and the existing coturn restart on certificate renewal MUST keep their purpose.

#### Scenario: Stop with a running publisher
- **WHEN** the container receives SIGTERM or SIGINT while the publisher is running
- **THEN** the publisher and its restart loop end together with the container, without a new start and without leftover child processes

#### Scenario: Stop during the pause
- **WHEN** the container receives SIGTERM or SIGINT while waiting for a restart
- **THEN** the wait is interrupted and a new publisher does not start

#### Scenario: State preserved on recovery
- **WHEN** the publisher starts again after an exit and the volume has saved state
- **THEN** it uses the existing files and lock by the previous rules, without a history reset or a parallel publication by a second process

#### Scenario: Certificate renewal
- **WHEN** the certificate changes while the publisher is running
- **THEN** coturn restarts by the existing mechanism, and a second publisher does not start
