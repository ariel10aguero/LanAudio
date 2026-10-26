# LanAudio

A C++20 desktop app that streams uncompressed stereo audio between computers
over a local network.

LanAudio captures a stereo input pair on one machine and plays it on another
(a mono input is duplicated to both channels). Audio travels as 16-bit linear
PCM (L16) over RTP/UDP. Every stream is encrypted, and the two
devices must be paired before any audio flows.

<!-- Screenshot placeholder: add an image or GIF of the desktop app at
     docs/images/lanaudio-gui.png and replace this comment with:
     ![LanAudio desktop app](docs/images/lanaudio-gui.png) -->
> **Screenshot / GIF:** _coming soon._

> **Project status:** pairing, encrypted media, automatic format negotiation,
> and the latency profiles are implemented and covered by unit tests. Analog
> end-to-end latency has not been measured, and release packages are not yet
> signed or notarized. See [STATUS.md](STATUS.md).

## Features

- **Encrypted audio transmission.** RTP media is protected with libsrtp2
  using `AEAD_AES_128_GCM`, with replay protection. Media keys are derived from
  the TLS 1.3 session (`SSL_export_keying_material`), not from a custom KDF.
  Plaintext RTP exists only behind a development-only CMake flag, and
  distribution builds refuse it.
- **Secure device pairing.** Each device generates an Ed25519 identity and a
  self-signed certificate (stored in the macOS Keychain). The first connection
  uses mutually authenticated TLS 1.3, and both screens show a six-digit code
  derived from the TLS exporter and both fingerprints. Once the user confirms
  that the codes match on both devices, each device pins the other's
  fingerprint. Later sessions are authenticated against that trust store.
  Trusted devices can be listed, removed, or reset from the GUI or the CLI,
  and renamed from the GUI.
- **Automatic sample-rate negotiation.** The devices exchange their supported
  rates over the control channel (16, 32, 44.1, 48, 88.2, 96, 176.4, and
  192 kHz are recognised). The receiver's preferred rate is used if both
  devices support it. Otherwise the common rate closest to 48 kHz is used. If
  there is no common rate, the session fails with an error instead of
  resampling. Packet duration is negotiated in the same step so that each
  encrypted packet stays within a 1,200-byte payload limit.
- **Configurable latency profiles.** Five presets set packet duration, jitter
  buffer depth, and audio device buffer size together (see
  [Latency](#how-latency-is-handled)).
- **GUI and CLI tools.** A Qt 6 desktop app (`LanAudio.app`) and a
  command-line tool (`lanaudio`) share the same libraries.
- **LAN discovery.** On macOS, receivers advertise themselves over
  Bonjour/DNS-SD (`_lanaudio._tcp`), and senders can scan for them.

## Supported platforms

| Platform | Status |
| --- | --- |
| macOS 13.0+ on Apple silicon (arm64) or Intel (x86_64) | Primary target: GUI, CLI, Core Audio I/O, Keychain-backed identity, Bonjour discovery. |
| Linux, Windows | Only the core and protocol libraries are built and tested in CI, with the GUI, audio I/O, and security stack disabled. The build files can also select ALSA (Linux) or WASAPI (Windows). Without macOS, though, device identity and trust are kept only in memory and discovery is unavailable. End-to-end use on these platforms has not been verified. |

macOS builds target exactly one architecture. Universal binaries are
intentionally disabled.

## Dependencies

| Dependency | Version | Notes |
| --- | --- | --- |
| CMake | 3.24+ | |
| C++ compiler | C++20 | AppleClang, Clang, GCC, or MSVC |
| Ninja | any | Generator used in CI and in the examples below; optional |
| OpenSSL | 3.0+ | TLS 1.3 control channel, Ed25519 identity, SRTP crypto backend |
| Qt | 6.5+ (Widgets, Network) | GUI only. Release builds pin 6.8.3. Qt 6.12+ requires `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.4` |
| RtAudio | 6.0.1 | Fetched automatically and verified by SHA-256 |
| libsrtp | 2.6.0 | Fetched automatically and verified by SHA-256 |
| Python 3 | any | Needed by the macOS GUI build (app bundling) |
| Security.framework, dns_sd | system | macOS Keychain and Bonjour |

Fetched dependencies are pinned by URL and SHA-256 in
[`dependencies.lock.json`](dependencies.lock.json).

## Build

The steps below were tested on macOS 13.4 (Apple silicon) from a fresh clone,
using Homebrew packages.

```bash
brew install cmake ninja openssl@3 qt

git clone https://github.com/ariel10aguero/LanAudio.git
cd LanAudio

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

This builds:

- `build/LanAudio.app`: the desktop GUI
- `build/lanaudio`: the CLI

The first configure downloads RtAudio and libsrtp, so it needs network access.
If Qt is not found, CMake skips the GUI and still builds the CLI. To build
without the GUI on purpose, pass `-DLANAUDIO_BUILD_GUI=OFF`.

Main CMake options:

| Option | Default | Purpose |
| --- | --- | --- |
| `LANAUDIO_BUILD_GUI` | `ON` | Build the Qt desktop app when Qt is found |
| `LANAUDIO_ENABLE_RTAUDIO` | `ON` | Build physical audio I/O |
| `LANAUDIO_ENABLE_SECURITY` | `ON` | Build TLS control, pairing, and SRTP. `send`/`receive` need this |
| `LANAUDIO_FETCH_DEPS` | `ON` | Fetch pinned RtAudio/libsrtp when not installed |
| `LANAUDIO_BUILD_FUZZERS` | `OFF` | libFuzzer harnesses for the RTP and control parsers (Clang) |
| `LANAUDIO_MACOS_DISTRIBUTION` | `OFF` | Require everything needed for a release `.dmg` |

Building release DMGs (`cpack`) for arm64 and x86_64 is covered in
[docs/development.md](docs/development.md).

## Run

Both machines must be on the same LAN. The receiver must accept inbound
**TCP 5005** (control) and **UDP 5004** (media).

### Desktop app

```bash
open build/LanAudio.app
```

1. **Receiver:** choose *Receive*, pick an output device and channel pair,
   then click *Start receiver*.
2. **Sender:** choose *Send*, pick an input device and channel pair, click
   *Scan LAN*, select the receiver, then click *Send audio*.
3. On first contact, both screens show a six-digit code. If they match, click
   *Codes match* on each machine. Audio starts once the secure session is
   ready.

Select a *Latency profile* before starting. macOS asks for microphone
permission the first time the sender captures audio.

### Command line

```text
lanaudio devices
lanaudio discover [--timeout-ms 2500]
lanaudio pair --host ADDRESS [--control-port 5005]
lanaudio list-trusted
lanaudio remove-trusted FINGERPRINT
lanaudio reset-trust
lanaudio send --host ADDRESS [--control-port 5005]
    [--input ID] [--input-channel 1] [--buffer-frames 128]
    [--latency low|balanced|reliable|wifi|congested-wifi]
    [--packet-us 5000] [--preferred-rate 48000] [--jitter-packets 4]
lanaudio receive [--control-port 5005] [--media-port 5004]
    [--output ID] [--output-channel 1] [--buffer-frames 128]
    [--latency low|balanced|reliable|wifi|congested-wifi]
    [--preferred-rate 48000] [--stats-interval-ms 0]
lanaudio diagnostics [--export FILE]
```

List audio devices and their IDs. Device ID `0` selects the system default:

```bash
./build/lanaudio devices
```

Start a receiver that prints stream statistics every second:

```bash
./build/lanaudio receive --output 0 --latency balanced --stats-interval-ms 1000
```

From the other machine, find receivers on the LAN and send audio from input
channels 1–2 of device 0:

```bash
./build/lanaudio discover
./build/lanaudio send --host 192.168.1.50 --input 0 --input-channel 1 --latency balanced
```

On first contact, both terminals print the same six-digit code. Type `yes` on
each side to pair, then press Ctrl-C to stop. Explicit `--buffer-frames`,
`--jitter-packets`, and `--packet-us` values override the selected latency
preset. Numeric options are range-checked, and invalid values are rejected.

Show the build's audio backend and encryption configuration:

```bash
./build/lanaudio diagnostics
```

## Architecture

### Modules

| CMake target | Directory | Responsibility |
| --- | --- | --- |
| `lanaudio_protocol` | `protocol/` | Capability negotiation, length-prefixed control message framing, rate limiting and abuse guards. No OpenSSL dependency, so it can be fuzzed anywhere. |
| `lanaudio_core` | `core/` | RTP packetizing/parsing, lock-free SPSC queues, jitter buffer, drift (PI) controller, UDP sockets, `NetworkSender` / `NetworkReceiver`, latency presets. |
| `lanaudio_security` | `security/` | Ed25519 device identity, Keychain credential store, trust store, pairing transcript and six-digit code. |
| `lanaudio_srtp` | `srtp/` | `SrtpProtector`: libsrtp2 `AEAD_AES_128_GCM` protect/unprotect. |
| `lanaudio_control` | `control/` | TLS 1.3 channel, Bonjour discovery, `SecureSender` / `SecureReceiver` session lifecycle. |
| `lanaudio_audio` | `backends/rtaudio/` | `RtAudioBackend`: device enumeration and input/output streams. |
| `lanaudio` | `apps/cli/` | Command-line tool. |
| `lanaudio-desktop` | `apps/desktop/` | Qt Widgets GUI (built as `LanAudio.app`). Qt is linked only here. |

### Network, encryption, and pairing flow

1. **Discovery (optional).** The receiver advertises `_lanaudio._tcp` over
   Bonjour with its name, version, fingerprint, and control port. The
   advertisement is advisory and is never treated as proof of identity.
2. **TLS 1.3 control channel.** The sender connects to the receiver on TCP 5005.
   TLS 1.3 is the only version allowed, and both sides present their
   self-signed Ed25519 certificates.
3. **Pairing (first contact only).** Each side hashes the TLS exporter output
   (label `EXPORTER-LanAudio-Pairing`) together with both fingerprints in a
   fixed order. The first bytes of the hash are reduced to a six-digit code.
   This code is only for the users to compare and is never used as a key. When
   both users confirm, the peer fingerprint is stored in the trust store.
   Later connections are accepted only if the peer's fingerprint is pinned.
4. **Negotiation.** Over the authenticated channel, the devices agree on
   sample rate, packet duration, jitter depth, a random SSRC and session ID,
   and the UDP media port.
5. **Media.** SRTP keys are exported from the TLS session (label
   `EXPORTER-LanAudio-SRTP`), with a separate context for each direction. The
   sender converts audio to L16, packetizes it into RTP, encrypts it with SRTP,
   and sends it over UDP 5004. The receiver rate-limits incoming packets,
   accepts only the authenticated peer's address, decrypts, validates the RTP
   header, and reorders packets in the jitter buffer.
6. **Supervision.** Heartbeats are exchanged every second over the control
   channel. If the channel is lost (6-second timeout), the session ends, queued
   audio is discarded, and the receiver outputs silence.

Session states shown in the GUI follow `SessionState`: scanning, device found,
pairing required, confirm code, pairing complete, authenticating, secure
session ready, streaming, connection lost, trust revoked.

### How latency is handled

The audio device callbacks are decoupled from the network:

```text
input callback  -> SPSC queue -> packetizer -> SRTP -> UDP
UDP -> SRTP unprotect -> jitter buffer -> SPSC queue -> output callback
```

- Audio callbacks only copy samples into or out of preallocated
  single-producer/single-consumer queues. They do not allocate memory, take
  locks, log, or perform cryptography. Network I/O and encryption run on
  separate threads.
- The receiver's **jitter buffer** waits for a configured number of packets
  before starting playback, reorders out-of-order packets, and resynchronises
  after sustained dropouts.
- A **drift controller** (a filtered PI controller acting on buffer
  occupancy) slightly adjusts the playback rate through fractional
  resampling. This keeps the buffer near its target when the two machines'
  audio clocks differ.
- **Latency profiles** coordinate the three main buffering parameters:

  | Profile | Packet duration | Jitter buffer | Device buffer |
  | --- | --- | --- | --- |
  | Low latency | 2.5 ms | 2 packets | 64 frames |
  | Balanced (default) | 5 ms | 4 packets | 128 frames |
  | Reliable | 5 ms | 8 packets | 256 frames |
  | Wi-Fi | 5 ms | 16 packets | 256 frames |
  | Congested Wi-Fi | 5 ms | 32 packets | 256 frames |

  The Wi-Fi profiles trade latency for headroom to ride out wireless stalls.
  The packet duration is a preference: if it would exceed the encrypted
  payload limit at the negotiated sample rate, negotiation falls back to 5 ms
  or the longest duration that fits. The format is always uncompressed stereo
  PCM, so the profiles never change audio quality.

These are software buffering budgets only. End-to-end analog latency (ADC,
drivers, network, DAC) has not been measured. See
[docs/latency.md](docs/latency.md) for the measurement procedure.

More detail: [docs/architecture.md](docs/architecture.md),
[docs/protocol.md](docs/protocol.md), [docs/security.md](docs/security.md),
[docs/troubleshooting.md](docs/troubleshooting.md).

## How it was built

I owned the system design: the functional and non-functional requirements,
the object-oriented design, and the overall architecture. AI coding agents
(Claude Code, OpenCode, and ORCA) wrote the implementation against that
design.

## License

Released under the MIT License. See [LICENSE](LICENSE).
Third-party components and their licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
