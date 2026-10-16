# Protocol

LanAudio uses two channels: a TLS 1.3 control connection and SRTP-protected
RTP/UDP media. Control protocol version 3 is required. Version 2 lacked the
trust_status exchange and is rejected; version 1 was the unnegotiated 0.1
prototype.

## Control frames

Frames are a 16-bit big-endian body length, a one-byte type, and a bounded
body (at most 2,048 bytes). Text fields are at most 128 bytes and may not
contain control characters. Unknown types, oversized lengths, and invalid
session parameters are rejected before any media starts.

Message types:

1. hello — protocol version, device name, application version, fingerprint, nonce
2. capability_offer — supported sample rates and packet durations
3. session_accept — negotiated media parameters
4. session_reject — typed failure
5. heartbeat
6. goodbye
7. pair_confirm — accepted flag plus transcript digest
8. pair_result — whether the peer stored the fingerprint
9. trust_status — whether the sender already trusts the peer

Hello fingerprints must match the certificate used in the TLS handshake.
Both peers send trust_status right after hello. Pairing runs when either
side does not trust the other, so a trust entry removed on only one device
leads to a fresh pairing on both instead of a stalled handshake.

## Format negotiation

Supported sample rates: 16 kHz, 32 kHz, 44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz,
176.4 kHz, 192 kHz. Channels are stereo. Packet duration candidates are 1 ms,
1.25 ms, 2.5 ms, 5 ms, 10 ms, and 20 ms.

The receiver's preferred rate wins when both devices support it; otherwise the
common rate closest to 48 kHz is chosen. Packet duration is then reduced so
that L16 payload plus the 16-byte SRTP tag stays at or under 1,200 bytes. If
no common rate or no MTU-safe duration exists, negotiation fails instead of
resampling.

The receiver assigns a random non-zero SSRC and session identifier.

## Media

RTP version 2 carries interleaved signed 16-bit PCM, most-significant byte
first, with a dynamic payload type in 96–127 (default 96). After SRTP
unprotect the receiver rejects:

- non-v2 RTP;
- CSRC, extension, or padded headers;
- unexpected payload types;
- payloads over 1,200 bytes;
- unexpected SSRC values;
- payload lengths inconsistent with the negotiated format;
- datagrams from any address other than the authenticated peer.

Sequence ordering uses signed 16-bit modular distance. The jitter buffer
conceals a gap only when a later packet is actually buffered, and
resynchronizes after eight consecutive out-of-window packets so a restart
cannot wedge the receiver.

## Security profile

SRTP uses libsrtp2 `AEAD_AES_128_GCM`. Keys are 28 bytes per direction
(16-byte AES key plus 12-byte salt) taken from the TLS 1.3 exporter label
`EXPORTER-LanAudio-SRTP`. Pairing uses exporter label
`EXPORTER-LanAudio-Pairing`. No custom cipher or key-derivation algorithm is
implemented.
