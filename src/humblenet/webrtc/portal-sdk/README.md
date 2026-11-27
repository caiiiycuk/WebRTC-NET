# Portal SDK overlay v2

This directory carries the exact SDK source patch, public header and locked
offline build recipe. It does not by itself enable portal send capability 0x08.
V2 measures actual retained payload/records separately from the source-proven
reservation potential, including peer restart. The wrapper must pass prospective
peer/global admission, actual proxy send/drain/close controls and review before
advertising that capability.

Supply the pinned 6422.8505a9.224 SDK and the five original `.cc` files from
WebRTC revision `8505a9838ea91c66c96c173d30cd66f9dbcc7548`, preserving their
relative paths in `source-root`. Their SHA256 values are in `inputs.lock.json`.
The recipe verifies those sources, affected original SDK headers, static
libraries, compiler, archiver, patch and public header before writing output.
It does not fetch, sync, configure a foreign SDK, or modify a gitlink.

```sh
python3 build.py --sdk-root /path/to/pinned-sdk --source-root /path/to/pinned-sources \
  --output-dir /path/to/fresh-output --cxx /path/to/pinned-clang++ --ar /path/to/pinned-ar
```

Output contains a full matching include/lib SDK plus the compiled overlay,
commands and hashes. Five uniquely named archive members are replaced in a copy;
unchanged members retain the supplied base provenance. Build paths in rebuilt
objects are normalized. This is a new mixed SDK identity, not reproduction of
the original upstream compiler/GN build. Select it explicitly using existing
`HUMBLENET_EXTERNAL_WEBRTC_PROVIDER_ROOT`, `_INCLUDE_DIR` and `_LIB_DIR` CMake
inputs, with a fresh output directory outside the source tree. There is no
automatic SDK build or download target.

Controls and runtime evidence belong outside this directory. Full send
qualification additionally requires prospective payload/record bounds, whole
accepted/backpressure/closed propagation, actual drain/resumption and safe
callback/socket destruction. Snapshot/build success alone does not qualify it.
The original v1 candidate is retained separately in recovery evidence; this
product selects v2.

## CI and release status

The portal sources and locks come from
`caiiiycuk/quake2#9`, commit `5dfc9e2f0edb377b4766fee7f3f2ed7161743cc7`,
file `q2pro/doszone/native/humblenet-provider.patch`.
The upstream SDK build used by the existing Linux workflows has no portal
overlay. Those workflows check that the portal reports no capabilities and
refuses enablement. Their artifacts are not accepted portal providers.

The locked SDK inputs and compiler must be supplied before CI can build a portal
release. The recipe checks the compiler and archiver bytes as well as the SDK.
Do not replace the expected hashes with hashes of whatever tools CI provides.
After building the overlay, configure WebRTC-NET with its explicit include/lib
paths, build all targets, and run:

```sh
LD_LIBRARY_PATH="$PWD/build" ./build/humblenet_test_portal.bin.x86_64 0x1f
```

The smoke test checks ABI exports, option validation and eventfd ownership
without starting signaling. It does not qualify traffic bounds, callback
retirement under load, or Quake II gameplay. An accepted release still needs
those checks and independent hashes for both shared libraries and public headers.
