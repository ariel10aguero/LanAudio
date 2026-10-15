#include "lanaudio/security/pairing.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace lanaudio::security {
namespace {

constexpr std::string_view kTranscriptDomain = "LanAudio pairing transcript v1";

}  // namespace

std::array<std::uint8_t, 32> pairing_transcript(
    std::span<const std::uint8_t> exporter_material, const Fingerprint& local,
    const Fingerprint& remote) noexcept {
  // Canonical ordering removes the initiator/responder asymmetry so both
  // devices compute an identical code.
  const Fingerprint* first = &local;
  const Fingerprint* second = &remote;
  if (std::lexicographical_compare(remote.begin(), remote.end(), local.begin(),
                                   local.end())) {
    first = &remote;
    second = &local;
  }

  std::vector<std::uint8_t> input;
  input.reserve(kTranscriptDomain.size() + first->size() + second->size() +
                exporter_material.size());
  input.insert(input.end(), kTranscriptDomain.begin(), kTranscriptDomain.end());
  input.insert(input.end(), first->begin(), first->end());
  input.insert(input.end(), second->begin(), second->end());
  input.insert(input.end(), exporter_material.begin(),
               exporter_material.end());

  std::array<std::uint8_t, 32> digest{};
  if (!sha256(input, digest)) digest.fill(0);
  secure_clear(input);
  return digest;
}

std::string short_authentication_string(
    const std::array<std::uint8_t, 32>& transcript) noexcept {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value = (value << 8U) | transcript[i];
  }
  value %= 1'000'000U;
  std::string digits = std::to_string(value);
  digits.insert(digits.begin(), 6 - digits.size(), '0');
  return digits;
}

bool constant_time_equal(std::span<const std::uint8_t> first,
                         std::span<const std::uint8_t> second) noexcept {
  if (first.size() != second.size() || first.empty()) return false;
  return CRYPTO_memcmp(first.data(), second.data(), first.size()) == 0;
}

}  // namespace lanaudio::security
