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

## Tuning order

Start on wired Ethernet with 5 ms packets, two receiver packets, and 128-frame
device requests. Confirm stability, then try 64-frame periods and 2.5 ms
packets separately. A requested period is not necessarily the negotiated
period. Wi-Fi power saving, Bluetooth audio, aggregate devices, software
mixers, and sample-rate conversion can each add latency or instability.

The nominal software budget in `docs/architecture.md` excludes ADC/DAC and
driver safety offsets. No latency figure is valid until measured through the
physical signal path.
