# Latency measurement and tuning

Do not use ping or software queue depth as analog end-to-end latency.

## Reproducible loopback measurement

1. Connect one output of a measurement audio interface to the sender input.
2. Split the same source signal into a reference input on that interface.
3. Connect the receiver computer's physical output to a second measurement
   input.
4. Configure every device for 48 kHz and record both measurement inputs in one
   multichannel clock domain.
5. Play a click, maximum-length sequence, or other broadband test sequence.
6. Export the reference and received channels as signed 16-bit PCM WAV files.
7. Run:

```bash
python3 tools/latency-measurement/measure_latency.py \
  reference.wav received.wav --max-ms 250
```

Repeat enough times to report median, p95, p99, minimum, and maximum delay.
Record the interfaces, drivers, negotiated device periods, operating systems,
network type, signal routing, packet duration, and jitter target.

## Latency profiles

The profiles coordinate software buffering without changing PCM quality:

- **Low latency:** 2.5 ms preferred packet, 2 jitter packets, 64-frame device
  request. Use on wired Ethernet or strong Wi-Fi; switch to Balanced if you
  hear concealment clicks or underruns.
- **Balanced (default):** 5 ms packet, 4 jitter packets, 128-frame request.
- **Reliable:** 5 ms packet, 8 jitter packets, 256-frame request.
- **Wi-Fi:** 5 ms packet, 16 jitter packets (80 ms), 256-frame request.
- **Congested Wi-Fi:** 5 ms packet, 32 jitter packets (160 ms), 256-frame
  request. Wi-Fi links can stall for 100 ms or more (power save, AWDL
  off-channel time); these profiles trade latency for riding out the stall.

The 2.5 ms preference is not a guarantee. The offer still advertises all
supported durations, and negotiation falls back to an encrypted MTU-safe
duration at the negotiated sample rate. No codec, bitrate reduction, or
resampling is used. A 1 ms packet or one-packet jitter buffer is not exposed as
a user-facing profile.

## Tuning order

Start with Balanced on wired Ethernet. Confirm stability, then select Low
latency as one coordinated change. A requested device period is not necessarily
the period selected by the driver, and a requested packet duration is not
necessarily the negotiated duration. Wi-Fi power saving, Bluetooth audio,
aggregate devices, software mixers, and sample-rate conversion can each add
latency or instability.

The nominal software budget in `docs/architecture.md` excludes ADC/DAC and
driver safety offsets. No latency figure is valid until measured through the
physical signal path.
