#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace lanaudio::security {

// SHA-256 over the DER SubjectPublicKeyInfo. This is the value pinned by the
// trust store and shown to the user; it never contains private material.
using Fingerprint = std::array<std::uint8_t, 32>;

[[nodiscard]] std::string to_hex(const Fingerprint& fingerprint);
[[nodiscard]] bool parse_hex_fingerprint(std::string_view text,
                                         Fingerprint& fingerprint);

// Grouped prefix for display, for example "A1B2-C3D4-E5F6-0718".
[[nodiscard]] std::string short_fingerprint(const Fingerprint& fingerprint);

[[nodiscard]] bool random_bytes(std::span<std::uint8_t> out) noexcept;
[[nodiscard]] std::uint32_t random_ssrc() noexcept;
[[nodiscard]] std::uint64_t random_session_id() noexcept;

// Overwrites a buffer so key material does not linger after use.
void secure_clear(std::span<std::uint8_t> buffer) noexcept;
void secure_clear(std::string& value) noexcept;

// Ed25519 identity with a self-signed certificate used for mutual TLS.
class DeviceIdentity {
 public:
  ~DeviceIdentity();
  DeviceIdentity(const DeviceIdentity&) = delete;
  DeviceIdentity& operator=(const DeviceIdentity&) = delete;

  [[nodiscard]] static std::unique_ptr<DeviceIdentity> generate(
      std::string_view common_name, std::string& error);
  [[nodiscard]] static std::unique_ptr<DeviceIdentity> from_pem(
      std::string_view private_key_pem, std::string_view certificate_pem,
      std::string& error);

  [[nodiscard]] Fingerprint fingerprint() const noexcept { return fingerprint_; }
  [[nodiscard]] std::string certificate_pem() const;
  // Only used to hand the key to the platform credential store.
  [[nodiscard]] std::string private_key_pem() const;

  // Opaque OpenSSL handles (X509*, EVP_PKEY*) for the TLS layer.
  [[nodiscard]] void* certificate_handle() const noexcept { return certificate_; }
  [[nodiscard]] void* key_handle() const noexcept { return key_; }

 private:
  DeviceIdentity(void* key, void* certificate, Fingerprint fingerprint);

  void* key_{};
  void* certificate_{};
  Fingerprint fingerprint_{};
};

// Computes the pinned fingerprint of a peer certificate (an X509*).
[[nodiscard]] bool certificate_fingerprint(void* certificate,
                                           Fingerprint& fingerprint) noexcept;

[[nodiscard]] bool sha256(std::span<const std::uint8_t> input,
                          std::span<std::uint8_t, 32> digest) noexcept;

}  // namespace lanaudio::security
