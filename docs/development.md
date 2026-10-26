# Development

## Dependency baseline

`dependencies.lock.json` is authoritative. CMake fetches archives by URL and
verifies SHA-256; mutable git tags are not used for security-relevant
dependencies.

- OpenSSL 3.0+ (CI uses the platform 3.x package): TLS 1.3, Ed25519, CSPRNG.
- libsrtp 2.6.0: SRTP AEAD_AES_128_GCM, fetched and hashed.
- RtAudio 6.0.1: physical I/O, fetched and hashed.
- Qt 6.8.3 Widgets/Network is the macOS release target.
- CMake 3.24 or newer and a C++20 compiler.
- macOS Security.framework for Keychain storage; dns_sd for Bonjour.

The media core and protocol parsers remain free of OpenSSL so every CI job can
compile and fuzz them. On MSVC, keep the runtime library compatible with Qt
(`RTAUDIO_STATIC_MSVCRT=OFF` when the application uses `/MD`).

## Configure options

- `LANAUDIO_BUILD_GUI`: build Qt Widgets target when Qt is found.
- `LANAUDIO_ENABLE_RTAUDIO`: build the real device adapter.
- `LANAUDIO_FETCH_DEPS`: fetch pinned archives when no package is available.
- `LANAUDIO_ENABLE_SECURITY`: build TLS control, pairing, and SRTP (ON).
- `LANAUDIO_ALLOW_PLAINTEXT`: development only; forbidden with distribution.
- `LANAUDIO_BUILD_FUZZERS`: libFuzzer harnesses for RTP and control parsers.
- `LANAUDIO_WARNINGS_AS_ERRORS`: promote project warnings to errors.
- `LANAUDIO_MACOS_DISTRIBUTION`: require the GUI, RtAudio, security stack, and
  architecture verification needed for a complete macOS package.
- `CMAKE_OSX_ARCHITECTURES`: exactly `arm64` or `x86_64` for a macOS release.
- `CMAKE_OSX_DEPLOYMENT_TARGET`: minimum macOS version; release packages use
  13.0 so they run on the supported Intel deployment baseline.

RtAudio fetch configuration enables only Core Audio on macOS, WASAPI on
Windows, and ALSA on Linux. PipeWire/JACK selection remains future work.

## macOS release builds

Release CI pins Qt 6.8.3 and RtAudio 6.0.1. RtAudio is compiled from source
for the requested architecture. Qt's imported frameworks are checked with
`lipo` during configuration; configuration fails if they do not contain the
requested architecture. Release builders also need Python 3 for the packaging
dependency-closure step; packaged end users do not.

Build each architecture in a separate native environment:

```bash
cmake -S . -B build-arm64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
  -DLANAUDIO_MACOS_DISTRIBUTION=ON \
  -DLANAUDIO_ENABLE_SECURITY=ON \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build-arm64 --parallel
ctest --test-dir build-arm64 --output-on-failure
cpack --config build-arm64/CPackConfig.cmake -G DragNDrop
tools/macos/verify_dmg.sh LanAudio-0.1.0-macOS-arm64.dmg arm64
```

Use an Intel build host and `x86_64` for the Intel package. Cross-compiling
with an arm64-only Qt installation is intentionally rejected. Release CI uses
native `macos-15` (arm64) and `macos-15-intel` (x86_64) runners with an Xcode 16
SDK, because Qt 6.8.3 links AGL.framework, which the macOS 26 SDK removed. The Intel
runner is scheduled for retirement by GitHub after the macOS 15/26 Intel image
lifecycle, so the runner strategy must be revisited before that service ends.

The output DMG contains `LanAudio.app`, including its Qt frameworks and the CLI
at `LanAudio.app/Contents/MacOS/lanaudio-cli`. The verification script mounts the
DMG, checks every Mach-O file for the target architecture, rejects Homebrew or
build-directory runtime references, validates privacy metadata, and smoke
tests both executables.

Universal packaging is deliberately disabled. It may be added only after the
main executable, RtAudio, every Qt framework, every plugin, and every other
native dynamic library all pass two-architecture `lipo` verification.

## Real-time review checklist

Never add allocation, mutex acquisition, system calls, logging, formatting, UI
work, or network I/O to RtAudio callbacks. Keep queue operations bounded.
Changes to SPSC ownership or memory ordering require concurrency tests and an
architecture update.

## Local verification

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The localhost integration test requires permission to bind a loopback UDP
socket. `clang-format` and `clang-tidy` are expected in CI/developer images but
were not installed in the initial local environment.
