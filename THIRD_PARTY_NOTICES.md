# Third-party notices

This file records dependencies included by the current LanAudio build and
dependencies selected for future production integration. Release engineering
must bundle the complete applicable license texts and satisfy all source,
relinking, and notice obligations for the exact binary package.

## Included in the current build

### OpenSSL 3

The control channel and device identity use OpenSSL 3 (libssl and libcrypto),
dynamically linked and bundled on macOS. OpenSSL 3 is licensed under Apache
License 2.0:
https://www.openssl.org/source/license.html

### libsrtp 2.6.0

Media protection uses Cisco libSRTP 2.6.0, built from the pinned release
archive and linked statically. The upstream license is BSD-3-Clause:
https://github.com/cisco/libsrtp/blob/v2.6.0/LICENSE

### RtAudio 6.0.1

Copyright (c) 2001–2023 Gary P. Scavone.

RtAudio is distributed under an MIT-like permissive license. The complete
upstream notice is available at:
https://github.com/thestk/rtaudio/blob/6.0.1/LICENSE

### Qt 6

The desktop application uses Qt Widgets and dynamically bundles Qt libraries.
Qt is available under LGPLv3, GPL, or commercial terms:
https://doc.qt.io/qt-6/licensing.html

An LGPL distribution must include the applicable license text, permit
replacement of the Qt shared libraries, provide required source information,
and must not prohibit reverse engineering for debugging modifications.

The packaged Qt build can also carry transitively linked libraries. Their
licenses must be collected from the exact Qt distribution before public
redistribution; this development notice is not a complete binary SBOM.

## Selected but not currently linked

- Boost 1.92.0 — Boost Software License 1.0.
- libsoxr 0.1.3 — LGPL-2.1-or-later.
- Opus 1.6.1 — BSD-3-Clause license and defensive patent grant.
- Avahi 0.8 — LGPL-2.1-or-later.
- Catch2 3.16.1 — Boost Software License 1.0.

These entries document architectural choices only and do not imply that the
corresponding code is included in the current executable.
