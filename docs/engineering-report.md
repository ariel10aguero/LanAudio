# Engineering report

Date: 2026-10-15

## Implemented and compiled

The repository contains a shared C++20 engine, RTP/UDP sender and receiver,
bounded jitter reordering, L16 conversion, callback-independent packetization,
adaptive linear resampling, RtAudio device integration, a headless CLI, and a
Qt Widgets desktop application. The UI provides named interface and channel
pair selection, live levels, receiver mute, network controls, latency/packet
settings, and runtime metrics. macOS debug and release builds compiled all
targets. A release-mode build of the dependency-free transport and CLI also
compiled.

## Automated verification

The test executable covers RTP round trips, PCM byte order, callback/packet
size independence, sequence wrap and reordering, SPSC boundaries, drift
controller simulations at ±50, ±100, and ±200 ppm, and sender/receiver
communication over localhost. All tests pass in the available macOS
environment.

## Platform and hardware verification

RtAudio enumerated built-in and virtual macOS devices with channel and
sample-rate capabilities. An external Intel Mac successfully captured its
built-in microphone and sent RTP audio to a receiver on the same LAN. A
Scarlett 2i4 was not available for physical acceptance, so its implemented
Core Audio device/channel path remains hardware-unverified. No Windows or
Linux machine was available. The CI matrix defines portable builds and native
arm64/x86_64 macOS distributions, but remote CI results are not available in
this local repository.

## Performance

No physical analog-to-analog latency measurement or Ethernet/Wi-Fi benchmark
has been performed. No 10–20 ms claim is made. The repository includes a WAV
cross-correlation measurement tool and procedure. Long-duration memory,
dropout, and clock stability measurements remain outstanding.

## Release gaps

TLS control, pairing, SRTP, DNS-SD, session recovery, platform-native
system-output capture, production packet-loss concealment, device hot-plug,
Developer ID signing, and notarization are not implemented. The arm64 DMG is
self-contained and verified locally; the Intel DMG still requires execution
of its native CI job and clean-Intel-Mac acceptance. These remain release
blockers; the current build is a functional trusted-LAN media path, not a
secure production release.
