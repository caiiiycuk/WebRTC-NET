WebRTC-NET
==========

[![WebRTC-NET OpenSSL Linux](https://github.com/caiiiycuk/WebRTC-NET/actions/workflows/humblenet-openssl-linux.yml/badge.svg)](https://github.com/caiiiycuk/WebRTC-NET/actions/workflows/humblenet-openssl-linux.yml)
[![WebRTC-NET BoringSSL Linux](https://github.com/caiiiycuk/WebRTC-NET/actions/workflows/humblenet-boringssl-linux.yml/badge.svg)](https://github.com/caiiiycuk/WebRTC-NET/actions/workflows/humblenet-boringssl-linux.yml)

WebRTC-NET is a cross platform networking library that utilizes WebRTC and WebSockets to handle network communication.

Using WebRTC and Websockets allows WebRTC-NET to support not only traditional platforms such as Windows, OS X, and Linux, but also web platforms such as ASM.JS / Emscripten.

Language Support
----------------
WebRTC-NET is a simple clean C based API that allows for wrappers to be written for any language that offers C binding.

Included Language Bindings
==========================
* C/C++
  - a simple C header is available for both C and C++ development.  There is also a BSD socket wrapper that will redirect the C socket API through WebRTC-NET.

Demo applications
=================
We have several demos to show off integrating WebRTC-NET.

TestPeer
--------
This is a simple "chat" app in the tests folder for C. The C app will run on Windows, MacOS X, Linux, and Emscripten.

Quake 2
-------
We have a port of Quake2 (Based on [R1Q2](http://www.r1ch.net/stuff/r1q2/)) that includes WebRTC-NET networking and runs on Windows, Mac OS X, Linux and Emscripten.

[Quake 2 WebRTC-NET port](https://github.com/HumbleNet/quake2)

Quake 3
-------
We have a port of Quake3 (Based on [QuakeJS](http://quakejs.com/)) that includes WebRTC-NET networking and runs on Windows, Mac OS X, Linux and Emscripten.

[Quake 3 WebRTC-NET port](https://github.com/HumbleNet/quake3)

Build Instructions
==================

Project Dependencies:
---------------------

1. CMake (https://cmake.org/):
    - Linux: use your preferred package manager, like `apt-get`.
    - Mac: can be installed using `brew`.
    - Win: download the installer via site https://cmake.org/download/.
2. Make (https://www.gnu.org/software/make/manual/make.html)
    - Unix: already installed.
    - Win: you can use Cygwin.
3. FlatBuffers (https://google.github.io/flatbuffers/index.html)
    - Download the source code. WebRTC-NET is way behind the current version of FlatBuffers. It's using the version 1.6.0 (currently 1.7.1).
        1. You can download/clone the repo, and revert the master to commit `81ecc98e023f85fe003a27e920e78b34db8a0087` [here](https://github.com/google/flatbuffers/commit/81ecc98e023f85fe003a27e920e78b34db8a0087#diff-e644a513ebf0d4b999ed39c245f8f3db).
        2. Or download the Release [here](https://github.com/google/flatbuffers/releases/tag/v1.6.0). This should work (not tested).
    - Follow the instructions here (https://google.github.io/flatbuffers/flatbuffers_guide_building.html);
    - You should get as result the `flatc` compiler executable.
    - Put your output directory (where the `flatc` exists, generally a `build` dir) into your `PATH`, so the compiler can be accessed anywhere.
4. Go Language [here](https://golang.org/)
    - Download and install the compiler via [download page](https://golang.org/dl/).
    - Test on your terminal if you can execute `go version`;
    - Tested here with version `go1.9.1 darwin/amd64`.

Compilation:
------------

Follow steps from [action file](https://github.com/caiiiycuk/HumbleNet/blob/master/.github/workflows/humblenet-linux.yml)

Confiugring
===========

Additionaly you can configure iceServes via js:
```js
window.netConfig = {
   debug: boolean, // verbose output
   ordered: boolean, 
   iceServers: //...,
}
```

Native clients must provide their own ICE server list with
`humblenet_set_iceservers()` before establishing WebRTC/P2P connections.
`peer-server` is used for peer matching and signaling only, so STUN/TURN
configuration must be supplied by the application.

Linux portal ABI
----------------

`humblenet_portal.h` defines an opt-in, lifetime-scoped ABI. Pass
`HUMBLENET_PORTAL_ABI` in the options and check every capability in
`HUMBLENET_PORTAL_CAPS_V1` before enabling it, before P2P initialization. The
backend requires the pinned SDK's V2 send seam and advertises **0x1f**, including
actual SCTP payload ownership and send-credit retirement. Ordinary callers
and the existing `humblenet_dz_*` ABI retain their defaults.

The portal reserves at most 160 connections, including unreclaimed closing, and
8 pending lifetimes before PeerConnection allocation. Pending includes RTC OPEN
and the application handshake. `started` transfers the initial15s deadline to
the application; only `established(peer,lifetime)` releases pending once, on the
first valid post-acceptance sequenced packet. A never-established closing
lifetime holds pending until reclaim. CLOSE is logical; consume it and call
`release`, then retain ownership until RECLAIM. Retained close/tombstone state
has 512 slots and expires using monotonic time without repeated same-peer lookups.

Poll the borrowed eventfd; never read, write or close it. Call `service` to reset
and rearm notifications under producer synchronization, then receive at most128
events before servicing other fds. Receive rotates peers and returns complete
raw1..1400-byte datagrams with monotonic ingress timestamps. There is no added
game framing or prefix delivery. The fixed ingress ring holds 32 messages per
peer. Its allocation budget is `sizeof(records) + 160 * sizeof(Inbox)`.
The SDK callback queue has a separate 1 MiB payload limit.
Only one unordered, maxRetransmits0 DataChannel is accepted per lifetime.

Send results are accepted1 (whole backend handoff, not remote receipt),
backpressure0 (whole drop, no retry queue or fatal close), and closed-1.
V2 egress admission caps reservation potential at64KiB per peer/1MiB globally
and4096/65536 potential record units per peer/globally. A new n-byte datagram
reserves2n bytes and n+1 record units before the SDK guarded send; queued
originals retain their complete fragment-family/restart prepayment. Absolute
snapshots retire credits while other traffic remains owned. `buffered_amount`
alone excludes retained SCTP payloads and cannot qualify that contract.
The public `egress_buffered_bytes` gauge reports actual retained payload bytes;
the SDK snapshot separately reports actual owned records and byte/record
reservation potential. Record units do not claim allocator/RSS size. Unknown
snapshot state latches admission closed; it never becomes zero ownership.
Stats expose monotonic refusal,
oversize, pressure and send-outcome counters plus current reservations,
pending/closing/retained records and owned-byte gauges. These limits do not
claim total RSS or DTLS/SCTP reassembly bounds, gameplay or performance acceptance.

HumbleNet rejects P2P startup when the final ICE server list is empty. Web
builds use `window.netConfig.iceServers` only when
`humblenet_set_iceservers()` was not called explicitly. A STUN-only list is
accepted, but produces a prominent warning because reliable connectivity
requires at least one TURN server.

For Linux production deployments, the intended backend is the external
Chromium WebRTC implementation documented in [README.chromium.md](/home/caiiiycuk/js-dos/HumbleNet/README.chromium.md).
That is the path that should be validated for real NAT traversal behavior.
The internal microstack backend is retained only as a demo and fallback path.
It is not the recommended production stack and should not be treated as the
reference implementation for feature completeness or ICE behavior.

Current microstack limitations:

- it is a fallback/demo backend, not the primary production target;
- it does not aim to match the Linux Chromium backend feature-for-feature;
- only the first TURN entry is currently applied on that backend, so multi-TURN
  failover configurations are not fully represented there.

Example JSON accepted by `humblenet_set_iceservers()`:

```json
[
  { "urls": ["stun:stun.l.google.com:19302"] },
  {
    "urls": ["turn:global.expressturn.com:3478?transport=udp"],
    "username": "user",
    "credential": "pass"
  }
]
```
