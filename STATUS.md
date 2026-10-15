# Development status

Last updated: 2026-10-15

## Implemented

- C++20/CMake project with a dependency-free media core and protocol library.
- Pinned `dependencies.lock.json` (libsrtp 2.6.0, RtAudio 6.0.1) verified by
  SHA-256 during configuration.
- Ed25519 device identity persisted in the macOS Keychain; in-memory store for
  tests.
- Bonjour/DNS-SD discovery with bounded TXT validation.
- Six-digit SAS pairing, fingerprint pinning, list/rename/remove/reset trust.
- TLS 1.3 mutual-auth control channel, versioned framing, capability
  negotiation across 16–192 kHz, MTU-safe packet durations.
- Mandatory libsrtp2 `AEAD_AES_128_GCM`, random SSRC/session id, peer-address
  binding, replay/rate/malformed guards, fail-closed teardown.
- Jitter resync after sustained dropouts; expired sessions play zeros.
- GUI session states from Scanning through Streaming, with Send/Receive gated
  on a secure session.
- Secure CLI: discover, pair, list-trusted, remove-trusted, reset-trust,
  send, receive; numeric options fail closed.
- Security unit tests, localhost SRTP session tests, parser fuzzers, SBOM,
  lock-file scan, and a notarization script that waits for Apple secrets.
- Architecture-specific macOS packaging for arm64 and x86_64.

## Verification record

- Host: Apple Silicon macOS 13.4, AppleClang 14.0.3, CMake 4.4.2, OpenSSL 3.6.3.
- `lanaudio_tests` passed in both the portable core build and the security
  build (localhost stream uses a test protector; unprotected media is refused).
- `lanaudio_security_tests` passed: negotiation, framing, identity, trust,
  SAS, SRTP round-trip/replay, end-to-end pairing + negotiated 48 kHz stream,
  unpaired refusal, declined-code abort.
- `LanAudio-0.1.0-macOS-arm64.dmg` was rebuilt with the security stack,
  static libsrtp, bundled OpenSSL, and a Frameworks rpath on the CLI.
  `tools/macos/verify_dmg.sh` passed: thin arm64 GUI and CLI, no Homebrew
  leftover paths, SRTP diagnostics, GUI smoke launch. SHA-256
  `6e8a6bd311d08960b1bac5b307aca8c09137ccbabcdabf44ca867bd5583e6dd1`.
- `LanAudio-0.1.0-macOS-x86_64.dmg` was rebuilt with the same security stack
  against a local x86_64 OpenSSL 3.5.8. Verification passed for the thin
  Intel GUI and CLI, bundled OpenSSL, SRTP diagnostics, and GUI smoke launch
  through Rosetta. SHA-256
  `8db35bd3299436c5376d1db316b4ba3ab6884852044f4aa6ce2d464c33b4a09e`.
- Analog end-to-end latency: not measured.
- Signed/notarized artifacts: pipeline wired; Apple secrets not applied.
- Windows/Linux runtime: not yet verified.

## Known blockers and gaps

- Production Developer ID signing and notarization still need Apple
  credentials (`MACOS_SIGNING_IDENTITY`, `MACOS_NOTARY_PROFILE`).
- Native system-audio capture, hot-plug handling, and automatic reconnect
  remain future work.
- Two-machine secure-audio acceptance on matching arm64 and Intel Macs has
  not been run with this stack.
- Physical capture from a Scarlett 2i4 has not yet been run in this workspace.

## Next action

Rebuild architecture-specific DMGs with security enabled, run the two-machine
pairing and Scarlett 2i4 path, then apply Apple signing secrets for a
production package.
