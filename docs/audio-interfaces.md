# USB audio interfaces

LanAudio uses RtAudio's Core Audio backend on macOS. A class-compliant USB
interface such as a Focusrite Scarlett 2i4 appears as an ordinary Core Audio
device and can be selected directly; no LanAudio-specific USB driver is used.

## Sender setup

1. Connect and power the interface before starting LanAudio.
2. Confirm that macOS Audio MIDI Setup shows its input channels.
3. Open LanAudio, select Send, and click Refresh if the interface was connected
   after launch.
4. Select the interface by name and choose the desired input pair. Inputs 1–2
   are the normal choice for a Scarlett 2i4.
5. Confirm that the capability line says `48 kHz supported`.
6. Set the interface's hardware gain while watching LanAudio's input meter.
   Avoid sustained full-scale indication.
7. Enter the receiver IP and send with 5 ms packets and a 128-frame device
   buffer. Try 64 frames only after stable operation is established.

The selected pair is captured as interleaved stereo. A final unpaired mono
channel is duplicated to left and right. LanAudio never silently substitutes
the built-in microphone when a specifically selected interface fails.

## CLI

List IDs, channel counts, current/preferred rates, and supported rates:

```bash
/Applications/LanAudio.app/Contents/MacOS/lanaudio-cli devices
```

Then select the reported interface ID and a one-based first channel:

```bash
/Applications/LanAudio.app/Contents/MacOS/lanaudio-cli send \
  --host 192.168.1.50 --control-port 5005 \
  --input 145 --input-channel 1 \
  --buffer-frames 128
```

Replace `145` with the ID printed on that Mac. Device IDs are Core Audio
runtime identifiers and may change after reconnecting hardware or rebooting.
Use `--input 0` only when the Core Audio default input is desired.

## If the interface is output-only

If `lanaudio-cli devices` reports `inputs=0`, Core Audio currently exposes that
entry only as an output. Check:

- the USB cable, power, and direct connection rather than an unreliable hub;
- Audio MIDI Setup for a separate input-side device entry;
- vendor driver/firmware requirements for that interface generation;
- macOS microphone permission for LanAudio or Terminal;
- the interface sample-rate setting.

LanAudio filters output-only devices out of the sender selector. It does not
claim that an output-only Core Audio entry can capture input.

## Format constraints

The current interoperable stream is fixed at 48 kHz, two channels, signed
16-bit network PCM. The interface may use floating-point samples internally;
LanAudio converts them explicitly for RTP. Interfaces that do not report
48 kHz support are rejected with a clear error rather than opened at an
undisclosed format.
