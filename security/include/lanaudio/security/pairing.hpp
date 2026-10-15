#pragma once

#include "lanaudio/security/identity.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace lanaudio::security {

// Exporter label used to bind the pairing transcript to the TLS session.
inline constexpr const char* kPairingExporterLabel = "EXPORTER-LanAudio-Pairing";
// Exporter label used to derive SRTP key material.
inline constexpr const char* kSrtpExporterLabel = "EXPORTER-LanAudio-SRTP";

inline constexpr std::size_t kPairingExporterBytes = 32;
inline constexpr std::size_t kSrtpKeyMaterialBytes = 56;  // two 28-byte keys

// Transcript digest over the TLS exporter output and both identities. The
// ordering is canonical so both peers derive the same value regardless of who
// initiated the connection.
[[nodiscard]] std::array<std::uint8_t, 32> pairing_transcript(
    std::span<const std::uint8_t> exporter_material,
    const Fingerprint& local, const Fingerprint& remote) noexcept;

// Six-digit human verification code derived from the transcript. It is only a
// comparison aid: it is never used as, or mixed into, an encryption key.
[[nodiscard]] std::string short_authentication_string(
    const std::array<std::uint8_t, 32>& transcript) noexcept;

// Constant-time comparison for transcript digests and fingerprints.
[[nodiscard]] bool constant_time_equal(
    std::span<const std::uint8_t> first,
    std::span<const std::uint8_t> second) noexcept;

}  // namespace lanaudio::security
