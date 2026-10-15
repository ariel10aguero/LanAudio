# Troubleshooting

## No audio devices

On macOS, open System Settings > Privacy & Security > Microphone and enable
LanAudio. Quit and reopen it after changing permission. The packaged CLI is:

```bash
/Applications/LanAudio.app/Contents/MacOS/lanaudio-cli devices
```

Terminal-launched capture can require permission for Terminal too. On Windows,
enable microphone access for desktop applications. On Linux, confirm that ALSA
development/runtime support and the expected PipeWire/JACK compatibility
service are installed.

The initial backend uses Core Audio on macOS, WASAPI on Windows, and ALSA on
Linux. Native system-output capture is not implemented yet; microphone capture
is never silently substituted for it.

For a USB interface, click Refresh after connecting it. If the device appears
with `inputs=0`, macOS currently exposes that entry as output-only; inspect
Audio MIDI Setup for a separate input entry and check its cable, power,
firmware, driver, and microphone permission. See
[audio-interfaces.md](audio-interfaces.md).

## Receiver gets no packets

Allow inbound TCP 5005 (control) and UDP 5004 (media), verify both machines
are on the same reachable LAN, and scan or enter the receiver address. Guest
Wi-Fi often isolates clients; both machines must be able to reach each other.
If pairing is required, confirm that both screens show the same six-digit
code. Audio will not start until the secure session is ready.

## “Bad CPU type in executable”

The wrong architecture package is installed. Apple Silicon Macs require
`LanAudio-0.1.0-macOS-arm64.dmg`; Intel Macs require
`LanAudio-0.1.0-macOS-x86_64.dmg`. Check Apple menu > About This Mac or run
`uname -m`. Normal users should install the matching DMG rather than build from
source.

## macOS says the application cannot be opened

Current development packages are unsigned. Control-click `LanAudio.app`,
choose Open, and confirm only if the package came from a trusted project
release. Signing and notarization require external Apple credentials and
remain release work.

## Microphone permission was denied

LanAudio sends the selected physical or virtual audio input. It does not
capture arbitrary macOS system audio. Enable access under System Settings >
Privacy & Security > Microphone. If LanAudio does not appear, start Send mode
once so macOS presents the permission prompt.

## Dropouts

Use wired Ethernet, select 5 ms packets, raise `--jitter-packets` from 2 to 4 or
8, and request a 128-frame device buffer. Disable Wi-Fi power saving where
appropriate. Do not use Bluetooth output for latency testing.

## Developer build without network access

Install RtAudio and Qt through the platform package manager, then configure
with `-DLANAUDIO_FETCH_DEPS=OFF`. To validate only the portable transport core,
also use `-DLANAUDIO_ENABLE_RTAUDIO=OFF -DLANAUDIO_BUILD_GUI=OFF`.
