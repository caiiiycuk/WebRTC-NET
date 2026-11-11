## Purpose

Provides a trustworthy TURN over TLS check by the turnutils_uclient tester: TLS read boundaries must not change messages, delivery results or loss counters.

## ADDED Requirements

### Requirement: TURN frames are independent of TLS reads

The tester SHALL reassemble STUN messages and ChannelData from the TLS control connection regardless of read boundaries, process each complete frame exactly once and keep an incomplete tail until the next read. It MUST account for the length and the stream padding of the respective message kind. The result MUST stay correct at the normal send interval without mandatory pacing.

#### Scenario: Several messages in one read
- **WHEN** a TLS read returns several complete frames and the beginning of the next one
- **THEN** all complete messages are processed in order exactly once
- **AND** the tail is kept and completed by a subsequent read, not discarded

#### Scenario: Split header and payload
- **WHEN** the header or the body of one message arrives in several reads
- **THEN** the partial frame is not passed to the handler as a complete message
- **AND** after all bytes are received the original message is processed without an artificial loss

#### Scenario: STUN and ChannelData interleaved
- **WHEN** the stream contains a STUN response, ChannelData with padding and a STUN Data indication
- **THEN** the boundaries of all messages are determined by their wire format, and padding does not leak into the next frame

### Requirement: Bounded and independent connection handling

The tester MUST bound the receive memory and check the message length before exceeding the buffer. Waiting for data SHALL keep the partial frame without a busy loop; a connection close or error SHALL end the read without presenting a truncated message as delivered. The state MUST belong to the specific connection and be cleaned up when it ends.

#### Scenario: Malformed or oversized frame
- **WHEN** an invalid header or a length not supported by the tester's bounded buffer is received
- **THEN** the connection ends with a diagnosable error without an overflow, unbounded accumulation or a false success counter

#### Scenario: Temporary absence of data
- **WHEN** a nonblocking TLS read cannot proceed for the moment
- **THEN** the partial data is kept, and the processing returns control to the readiness wait without data loss and without an active endless retry

#### Scenario: Close after part of a message
- **WHEN** the connection closes with an incomplete frame
- **THEN** the frame is not counted as received, and the test does not report successful delivery of all messages

#### Scenario: Repeated or parallel connection
- **WHEN** a new connection is created after one, or several clients work simultaneously
- **THEN** the bytes accumulated by one TLS connection are not used by another connection

### Requirement: Other transport modes are preserved

The fix SHALL keep the existing semantics of UDP, DTLS, plain TCP and the TLS data connection of the TCP relay. The raw TCP relay payload MUST NOT be mistakenly parsed as a STUN or ChannelData control frame. Counting of sent, received and lost messages MUST stay strict; masking losses by changing the expected count is forbidden.

#### Scenario: Non-TLS transport regression
- **WHEN** the existing UDP, DTLS and plain TCP checks run
- **THEN** their data format and the criteria of successful delivery do not change

#### Scenario: Raw data after a TCP relay connect
- **WHEN** the TLS data connection carries a raw payload, including first bytes that look like a STUN header
- **THEN** the payload is handled in TCP relay mode, not by the control connection framing

#### Scenario: Confirmed losses
- **WHEN** the number of received messages is lower than a positive number of sent messages
- **THEN** the delivery check is not considered successful even after the TLS framing fix
