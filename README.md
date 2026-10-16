# LanAudio

LanAudio is a C++20 desktop and command-line application for low-latency,
uncompressed audio transport on a local network. It captures and plays stereo
audio with RtAudio, negotiates a common format over a mutually authenticated
TLS 1.3 control channel, and transports L16 PCM as SRTP
(`AEAD_AES_128_GCM`).

> **Development status:** pairing, encrypted media, and automatic format
> negotiation are implemented. Analog latency, signed/notarized public
> packages, and two-machine hardware acceptance are still outstanding. See
> [STATUS.md](STATUS.md).

## Install on macOS

Normal users do not need CMake, Ninja, Homebrew, Qt, RtAudio, a compiler, or
any other development tool.

1. In Apple menu > About This Mac, check whether the Mac has an Apple chip or
   an Intel processor. Alternatively, `uname -m` prints `arm64` on Apple
   Silicon and `x86_64` on Intel.
2. Download the matching release:
   - `LanAudio-0.1.0-macOS-arm64.dmg` for Apple Silicon.
   - `LanAudio-0.1.0-macOS-x86_64.dmg` for Intel.
   Release packages require macOS 13.0 or newer.
3. Open the DMG and drag `LanAudio.app` to Applications.
4. Open LanAudio. The current unsigned development packages may require
   Control-click > Open and explicit confirmation. Production releases should
   be signed and notarized.
5. When sending for the first time, allow microphone access. LanAudio captures
   the selected microphone/audio-input device; it does not capture arbitrary
   macOS system audio.

Both Macs must be on the same reachable Ethernet or Wi-Fi LAN. Firewalls must
allow inbound TCP 5005 (control) and UDP 5004 (media). See
[docs/macos-installation.md](docs/macos-installation.md) for the complete
end-user walkthrough.

## Desktop sender and receiver

On the receiving Mac:

1. Select Receive mode, an output interface, and its stereo output pair.
2. Click Start receiver. The app advertises itself on the LAN.
3. Wait until the status shows Scanning, then leave it running.

On the sending Mac:

1. Connect the USB interface before opening LanAudio, or click Refresh.
2. Select Send mode, the interface (for example Scarlett 2i4), and its input
   pair.
3. Click Scan LAN, select the receiver, and click Send audio (or Pair
   selected).
4. When both screens show the same six-digit code, click Codes match on each
   Mac. Audio buttons stay disabled until the secure session is ready.
5. Grant microphone permission when macOS asks.

The two devices then agree a common sample rate automatically (preferring
48 kHz when both support it) and start SRTP media. If they share no supported
rate, the session fails with a clear error instead of resampling.
See [docs/audio-interfaces.md](docs/audio-interfaces.md) for USB interface and
channel-routing details.

Choose the desktop **Latency profile** to coordinate packet duration, jitter
startup, and device buffer requests. Low latency prefers 2.5 ms packets, two
jitter packets, and 64-frame device periods; Balanced is the default, and
Reliable adds buffering for less stable networks. Low latency falls back to an
MTU-safe packet duration when necessary and the negotiated duration is shown in
Stream format. It remains uncompressed stereo PCM, not a bitrate slider.

## Command line

The packaged CLI is inside the installed application:

```bash
/Applications/LanAudio.app/Contents/MacOS/lanaudio-cli devices
```

Receiver:

```bash
/Applications/LanAudio.app/Contents/MacOS/lanaudio-cli receive \
  --control-port 5005 --media-port 5004 --output 0 \
  --buffer-frames 128
```

Use `--latency low`, `--latency balanced`, or `--latency reliable` to select a
coordinated CLI preset. Explicit `--buffer-frames`, `--jitter-packets`, and
sender-only `--packet-us` values override the preset.

Sender, using the receiver's actual LAN IP:

```bash
/Applications/LanAudio.app/Contents/MacOS/lanaudio-cli send \
  --host 192.168.1.50 --control-port 5005 --input 0 \
  --input-channel 1 --buffer-frames 128
```

`lanaudio discover`, `pair`, `list-trusted`, `remove-trusted`, and
`reset-trust` manage pairing. Device ID `0` selects the backend default. Stop
with Ctrl-C. Terminal-launched capture may require microphone permission for
Terminal as well as LanAudio. The CLI fails closed if security is missing or a
numeric option is out of range.

Developer build instructions are in
[docs/development.md](docs/development.md).

## Important limitations

- Native macOS system-output capture, hot-plug recovery, and production
  Apple signing/notarization are not yet complete.
- No analog end-to-end latency has been measured. The 10–20 ms objective is a
  target, not a current claim.
- Architecture-specific CI produces separate arm64 and x86_64 packages.
  Hardware acceptance remains unverified until both artifacts run on clean
  matching Macs.

Design details are in [docs/architecture.md](docs/architecture.md), the wire
format is in [docs/protocol.md](docs/protocol.md), and the current engineering
record is in [STATUS.md](STATUS.md).
