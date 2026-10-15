#!/usr/bin/env python3
"""Measure delay between two mono/stereo PCM WAV recordings by correlation."""

import argparse
import array
import wave


def read_first_channel(path: str) -> tuple[int, list[float]]:
    with wave.open(path, "rb") as source:
        if source.getsampwidth() != 2:
            raise ValueError(f"{path}: only signed 16-bit PCM WAV is supported")
        channels = source.getnchannels()
        rate = source.getframerate()
        values = array.array("h", source.readframes(source.getnframes()))
        if values.itemsize != 2:
            raise ValueError("unexpected host integer width")
        if __import__("sys").byteorder == "big":
            values.byteswap()
        return rate, [values[i] / 32768.0 for i in range(0, len(values), channels)]


def normalized_correlation(reference: list[float], received: list[float],
                           offset: int, window: int) -> float:
    count = min(window, len(reference), len(received) - offset)
    if count <= 0:
        return -1.0
    dot = ref_energy = received_energy = 0.0
    for index in range(count):
        left = reference[index]
        right = received[index + offset]
        dot += left * right
        ref_energy += left * left
        received_energy += right * right
    denominator = (ref_energy * received_energy) ** 0.5
    return dot / denominator if denominator else -1.0


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", help="directly recorded source WAV")
    parser.add_argument("received", help="recorded receiver-output WAV")
    parser.add_argument("--max-ms", type=float, default=250.0)
    parser.add_argument("--window-ms", type=float, default=1000.0)
    args = parser.parse_args()

    reference_rate, reference = read_first_channel(args.reference)
    received_rate, received = read_first_channel(args.received)
    if reference_rate != received_rate:
        raise ValueError("recordings must use the same sample rate")
    max_offset = int(reference_rate * args.max_ms / 1000.0)
    window = int(reference_rate * args.window_ms / 1000.0)
    best_offset, best_score = max(
        ((offset, normalized_correlation(reference, received, offset, window))
         for offset in range(max_offset + 1)),
        key=lambda candidate: candidate[1],
    )
    print(f"delay_samples={best_offset}")
    print(f"delay_ms={best_offset * 1000.0 / reference_rate:.3f}")
    print(f"correlation={best_score:.6f}")


if __name__ == "__main__":
    main()
