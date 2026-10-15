# macOS installation and first stream

## Choose the package

- Apple Silicon (M-series): `LanAudio-0.1.0-macOS-arm64.dmg`
- Intel processor: `LanAudio-0.1.0-macOS-x86_64.dmg`

Release packages require macOS 13.0 or newer.

Check Apple menu > About This Mac. `uname -m` is an optional check: `arm64`
means Apple Silicon and `x86_64` means Intel. Installing the wrong package
causes “Bad CPU type in executable.”

Open the matching DMG, drag `LanAudio.app` into Applications, and eject the
DMG. No compiler, package manager, Qt installation, or build tool is required.
Unsigned development builds may require Control-click > Open. Signed and
notarized public artifacts are not available yet.

## Receive

1. Put both Macs on the same reachable LAN.
2. Open LanAudio on the receiver and select Receive.
3. Select the output device.
4. Click Start receiver. Status should show Scanning.
5. Allow inbound TCP 5005 and UDP 5004 if macOS or another firewall prompts.

## Send

1. Open LanAudio on the sender and select Send.
2. Select a microphone or USB audio interface and its input pair.
3. Click Scan LAN and select the receiver, or type its IP address.
4. Click Send audio. When both devices show the same six-digit code, click
   Codes match on each Mac.
5. Grant microphone permission when macOS asks.

LanAudio currently sends audio input/microphone audio. It does not capture
arbitrary macOS system output. Audio does not start until pairing (or an
existing trust entry) has produced a secure session.

## Packaged CLI

The command-line executable is kept inside the application:

```bash
/Applications/LanAudio.app/Contents/MacOS/lanaudio-cli devices

/Applications/LanAudio.app/Contents/MacOS/lanaudio-cli receive \
  --control-port 5005 --media-port 5004 --output 0 \
  --buffer-frames 128

/Applications/LanAudio.app/Contents/MacOS/lanaudio-cli send \
  --host 192.168.1.50 --control-port 5005 --input 0 \
  --input-channel 1 --buffer-frames 128
```

Replace `192.168.1.50` with the receiver's actual LAN address. Stop with
Ctrl-C.

## Security

Media is SRTP `AEAD_AES_128_GCM` after a TLS 1.3 pairing handshake. The six-digit
code is only a comparison aid. Do not expose the control or media ports to the
internet. See [security.md](security.md).
