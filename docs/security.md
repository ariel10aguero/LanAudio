# Security

LanAudio ships with authenticated control and encrypted media. Unauthenticated
plaintext RTP is not available in normal or distribution builds.

## Device identity

On first launch the application generates an Ed25519 key pair and a self-signed
certificate with OpenSSL 3. The SHA-256 digest of the certificate
SubjectPublicKeyInfo is the device fingerprint. The private key is stored in
the macOS Keychain (`kSecClassGenericPassword`,
`kSecAttrAccessibleWhenUnlockedThisDeviceOnly`) under
`com.lanaudio.desktop`. Tests may use an in-memory store; that backend reports
itself as insecure.

Private keys, TLS exporter output, and SRTP keys never appear in logs,
diagnostics, or exported reports. `diagnostics` always sets
`contains_secrets` to false.

## Discovery

macOS uses Bonjour/DNS-SD (`_lanaudio._tcp`) to advertise a device name, app
version, protocol version, fingerprint, and control port. Discovery is
advisory. A discovered fingerprint is never treated as proof of identity; the
TLS handshake is.

## Pairing

A first connection opens a temporary TLS 1.3 channel with mutual certificates.
Both devices derive a six-digit short authentication string from the TLS
exporter label `EXPORTER-LanAudio-Pairing` plus the ordered fingerprints. The
digits are only a human comparison aid. They are never used as, or mixed into,
an encryption key. Both sides must confirm the same code and the same
transcript digest before the peer fingerprint is stored.

Trusted devices can be listed, renamed, removed, or reset from the GUI and
from `lanaudio list-trusted` / `remove-trusted` / `reset-trust`.

## Control channel

The control channel is TLS 1.3 only, with mutual certificates and fingerprint
pinning against the trust store. Frames are length-prefixed and hard-bounded.
The channel negotiates protocol version 2, sample rate, channels, frames per
packet, a random SSRC, a random session identifier, UDP media port, jitter
depth, and SRTP key material. Audio never starts if this channel is not
authenticated.

Media keys come from `SSL_export_keying_material` with the label
`EXPORTER-LanAudio-SRTP`. There is no custom KDF.

Heartbeats keep the control session alive. Losing the channel expires the
media session immediately. The receiver then plays zeros.

## Media protection

RTP is protected with libsrtp2 `AEAD_AES_128_GCM`. Sender and receiver use
separate contexts. Replay protection is enabled. The media socket binds to the
authenticated peer address. Rate limits and malformed-packet thresholds
temporarily block abusive sources. Sequence, timestamp, payload type, SSRC, and
payload length are validated after decryption.

The former fixed SSRC is gone. A session that cannot supply a protector, a
non-zero SSRC, and a non-zero session identifier refuses to start.

## Fail-closed rules

- `LANAUDIO_ALLOW_PLAINTEXT` cannot be combined with
  `LANAUDIO_MACOS_DISTRIBUTION`.
- Distribution builds require `LANAUDIO_ENABLE_SECURITY=ON`.
- CLI `send` and `receive` fail if pairing or authentication is unavailable.
- Numeric CLI options are validated before any narrowing conversion.
- After authentication loss or session expiry, output is zero-filled and
  queued audio is discarded.

## Build and supply chain

Fetched archives (libsrtp 2.6.0, RtAudio 6.0.1) are pinned by URL and SHA-256
in `dependencies.lock.json`. CI scans the lock file, emits an SBOM, and builds
libFuzzer harnesses for the RTP and control parsers. macOS signing and
notarization run when `MACOS_SIGNING_IDENTITY` and `MACOS_NOTARY_PROFILE` are
present; missing production secrets fail closed if
`LANAUDIO_REQUIRE_NOTARIZATION=1`.
