# Architecture

## Data path

The audio device callback and network packet sizes are intentionally
independent:

```text
input callback -> bounded SPSC queue -> packetizer -> SRTP/UDP
SRTP/UDP -> unprotect -> jitter buffer -> bounded SPSC queue -> output callback
```

`RtAudioBackend` owns the physical stream. Its callbacks only copy fixed-size
floating-point sample blocks to or from preallocated SPSC queues or
non-blocking secure-session callbacks. They do not allocate, wait on
control-plane locks, log, access the UI, or perform cryptography.

`NetworkSender` and `NetworkReceiver` own background threads and UDP sockets.
The sender packetizes, then applies SRTP on the network thread. The receiver
rate-limits, binds to the authenticated peer, unprotects, validates RTP, and
reorders packets. The output callback interpolates; a filtered PI controller
corrects occupancy. An expired session zero-fills output immediately.

The Qt application and CLI drive `SecureSender` / `SecureReceiver`. Qt is not
linked into the core, protocol, security, or CLI libraries.

## Ownership and concurrency

- One audio callback produces `NetworkSender` queue entries; one network thread
  consumes them.
- One network thread produces `NetworkReceiver` queue entries; one audio
  callback consumes them.
- SPSC queue indices use acquire/release publication. Each slot is exclusively
  owned by the producer until publication and by the consumer after removal.
- Queue and jitter-buffer capacities are compile-time bounded. Queue overflow
  drops new media and increments an atomic counter; callbacks never wait.
- `StreamMetrics` uses relaxed atomics because counters do not publish object
  lifetime or audio data.
- Stream shutdown stops the device callback before destroying network objects.

## Protocol boundaries

`core.hpp` contains dependency-free format, RTP, conversion, queue, jitter, and
clock-controller primitives. `stream.hpp` contains media session data paths.
`RtAudioBackend` is an adapter and can be replaced by Core Audio HAL,
`IAudioClient3`, PipeWire/JACK, or ALSA implementations.

Control, discovery, and security are separate from the media path:

- `lanaudio_protocol` — negotiation, framing, abuse limits (no OpenSSL)
- `lanaudio_security` — identity, Keychain/trust, pairing transcript
- `lanaudio_srtp` — libsrtp2 AEAD protector
- `lanaudio_control` — TLS 1.3 channel, Bonjour, session lifecycle

Cryptographic work and UI confirmation run on the control thread. Audio
callbacks only push or render samples. Unencrypted RTP exists only behind
`LANAUDIO_ALLOW_PLAINTEXT` and is rejected for distribution builds.

## Latency budget

At the default settings, software buffering starts with:

- input callback request: 64 frames (1.33 ms at 48 kHz);
- packetization: up to 240 frames (5 ms);
- receiver startup: two packets (10 ms);
- output callback request: 64 frames (1.33 ms).

These values do not include converter, hardware, driver, scheduling, network,
or DAC latency. Actual periods can differ from requests. Consequently the
analog 10–20 ms objective requires measured hardware tuning and cannot be
inferred from this nominal sum.

## Next architectural increments

1. Improve bounded click-suppressed concealment with timestamp-driven gaps.
2. Replace linear interpolation with libsoxr after validating its real-time
   streaming configuration and preallocation behavior.
3. Add automatic reconnect with fresh SRTP keys after a clean control
   re-handshake.
4. Add isolated native system-capture adapters.
5. Complete Developer ID signing and notarization with production Apple
   secrets.
