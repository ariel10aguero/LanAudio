#include "lanaudio/security/identity.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstring>

namespace lanaudio::security {
namespace {

constexpr long kCertificateLifetimeSeconds = 10L * 365L * 24L * 60L * 60L;

std::string openssl_error(std::string_view context) {
  const auto code = ERR_get_error();
  char buffer[256] = {};
  if (code != 0) ERR_error_string_n(code, buffer, sizeof(buffer));
  std::string message(context);
  if (buffer[0] != '\0') {
    message += ": ";
    message += buffer;
  }
  return message;
}

std::string bio_to_string(BIO* bio) {
  char* data = nullptr;
  const auto length = BIO_get_mem_data(bio, &data);
  if (data == nullptr || length <= 0) return {};
  return std::string(data, static_cast<std::size_t>(length));
}

bool spki_fingerprint(X509* certificate, Fingerprint& fingerprint) noexcept {
  unsigned char* der = nullptr;
  const int length = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(certificate), &der);
  if (length <= 0 || der == nullptr) return false;
  SHA256(der, static_cast<std::size_t>(length), fingerprint.data());
  OPENSSL_free(der);
  return true;
}

}  // namespace

std::string to_hex(const Fingerprint& fingerprint) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string text;
  text.reserve(fingerprint.size() * 2);
  for (const auto octet : fingerprint) {
    text.push_back(kDigits[octet >> 4U]);
    text.push_back(kDigits[octet & 0x0fU]);
  }
  return text;
}

bool parse_hex_fingerprint(std::string_view text, Fingerprint& fingerprint) {
  std::string compact;
  compact.reserve(text.size());
  for (const char character : text) {
    if (character == '-' || character == ':' || character == ' ') continue;
    compact.push_back(character);
  }
  if (compact.size() != fingerprint.size() * 2) return false;
  auto nibble = [](char character, int& value) {
    if (character >= '0' && character <= '9') {
      value = character - '0';
    } else if (character >= 'a' && character <= 'f') {
      value = character - 'a' + 10;
    } else if (character >= 'A' && character <= 'F') {
      value = character - 'A' + 10;
    } else {
      return false;
    }
    return true;
  };
  for (std::size_t i = 0; i < fingerprint.size(); ++i) {
    int high = 0;
    int low = 0;
    if (!nibble(compact[i * 2], high) || !nibble(compact[i * 2 + 1], low)) {
      return false;
    }
    fingerprint[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

std::string short_fingerprint(const Fingerprint& fingerprint) {
  const auto hex = to_hex(fingerprint);
  std::string grouped;
  for (std::size_t i = 0; i < 16 && i < hex.size(); ++i) {
    if (i != 0 && i % 4 == 0) grouped.push_back('-');
    grouped.push_back(hex[i]);
  }
  return grouped;
}

bool random_bytes(std::span<std::uint8_t> out) noexcept {
  if (out.empty()) return true;
  return RAND_bytes(out.data(), static_cast<int>(out.size())) == 1;
}

std::uint32_t random_ssrc() noexcept {
  std::array<std::uint8_t, 4> raw{};
  if (!random_bytes(raw)) return 0;
  std::uint32_t value = 0;
  for (const auto octet : raw) value = (value << 8U) | octet;
  // 0 is reserved by the media path to mean "not negotiated".
  return value == 0 ? 1U : value;
}

std::uint64_t random_session_id() noexcept {
  std::array<std::uint8_t, 8> raw{};
  if (!random_bytes(raw)) return 0;
  std::uint64_t value = 0;
  for (const auto octet : raw) value = (value << 8U) | octet;
  return value == 0 ? 1ULL : value;
}

void secure_clear(std::span<std::uint8_t> buffer) noexcept {
  if (!buffer.empty()) OPENSSL_cleanse(buffer.data(), buffer.size());
}

void secure_clear(std::string& value) noexcept {
  if (!value.empty()) OPENSSL_cleanse(value.data(), value.size());
  value.clear();
}

bool sha256(std::span<const std::uint8_t> input,
            std::span<std::uint8_t, 32> digest) noexcept {
  SHA256(input.data(), input.size(), digest.data());
  return true;
}

bool certificate_fingerprint(void* certificate,
                             Fingerprint& fingerprint) noexcept {
  if (certificate == nullptr) return false;
  return spki_fingerprint(static_cast<X509*>(certificate), fingerprint);
}

DeviceIdentity::DeviceIdentity(void* key, void* certificate,
                               Fingerprint fingerprint)
    : key_(key), certificate_(certificate), fingerprint_(fingerprint) {}

DeviceIdentity::~DeviceIdentity() {
  if (certificate_ != nullptr) X509_free(static_cast<X509*>(certificate_));
  if (key_ != nullptr) EVP_PKEY_free(static_cast<EVP_PKEY*>(key_));
}

std::unique_ptr<DeviceIdentity> DeviceIdentity::generate(
    std::string_view common_name, std::string& error) {
  EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
  if (key == nullptr) {
    error = openssl_error("unable to generate an Ed25519 device identity");
    return nullptr;
  }

  X509* certificate = X509_new();
  if (certificate == nullptr) {
    EVP_PKEY_free(key);
    error = openssl_error("unable to allocate the identity certificate");
    return nullptr;
  }

  bool ok = X509_set_version(certificate, 2) == 1;
  std::array<std::uint8_t, 16> serial_bytes{};
  ok = ok && random_bytes(serial_bytes);
  if (ok) {
    // Keep the serial positive by clearing the top bit.
    serial_bytes[0] &= 0x7fU;
    BIGNUM* serial = BN_bin2bn(serial_bytes.data(),
                               static_cast<int>(serial_bytes.size()), nullptr);
    ok = serial != nullptr &&
         BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(certificate)) !=
             nullptr;
    if (serial != nullptr) BN_free(serial);
  }
  ok = ok && X509_gmtime_adj(X509_getm_notBefore(certificate), 0) != nullptr;
  ok = ok && X509_gmtime_adj(X509_getm_notAfter(certificate),
                             kCertificateLifetimeSeconds) != nullptr;
  ok = ok && X509_set_pubkey(certificate, key) == 1;

  if (ok) {
    X509_NAME* name = X509_get_subject_name(certificate);
    const std::string subject(common_name.empty() ? "LanAudio device"
                                                  : common_name);
    ok = X509_NAME_add_entry_by_txt(
             name, "CN", MBSTRING_UTF8,
             reinterpret_cast<const unsigned char*>(subject.c_str()),
             static_cast<int>(subject.size()), -1, 0) == 1;
    ok = ok && X509_NAME_add_entry_by_txt(
                   name, "O", MBSTRING_UTF8,
                   reinterpret_cast<const unsigned char*>("LanAudio"), 8, -1,
                   0) == 1;
    ok = ok && X509_set_issuer_name(certificate, name) == 1;
  }

  // Ed25519 certificates are signed with a null digest.
  ok = ok && X509_sign(certificate, key, nullptr) > 0;

  Fingerprint fingerprint{};
  ok = ok && spki_fingerprint(certificate, fingerprint);

  if (!ok) {
    X509_free(certificate);
    EVP_PKEY_free(key);
    error = openssl_error("unable to build the identity certificate");
    return nullptr;
  }
  return std::unique_ptr<DeviceIdentity>(
      new DeviceIdentity(key, certificate, fingerprint));
}

std::unique_ptr<DeviceIdentity> DeviceIdentity::from_pem(
    std::string_view private_key_pem, std::string_view certificate_pem,
    std::string& error) {
  if (private_key_pem.empty() || certificate_pem.empty()) {
    error = "stored identity is incomplete";
    return nullptr;
  }
  BIO* key_bio = BIO_new_mem_buf(private_key_pem.data(),
                                 static_cast<int>(private_key_pem.size()));
  if (key_bio == nullptr) {
    error = "unable to read the stored private key";
    return nullptr;
  }
  EVP_PKEY* key = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
  BIO_free(key_bio);
  if (key == nullptr) {
    error = openssl_error("stored private key could not be parsed");
    return nullptr;
  }

  BIO* certificate_bio = BIO_new_mem_buf(
      certificate_pem.data(), static_cast<int>(certificate_pem.size()));
  if (certificate_bio == nullptr) {
    EVP_PKEY_free(key);
    error = "unable to read the stored certificate";
    return nullptr;
  }
  X509* certificate =
      PEM_read_bio_X509(certificate_bio, nullptr, nullptr, nullptr);
  BIO_free(certificate_bio);
  if (certificate == nullptr) {
    EVP_PKEY_free(key);
    error = openssl_error("stored certificate could not be parsed");
    return nullptr;
  }
  if (X509_check_private_key(certificate, key) != 1) {
    X509_free(certificate);
    EVP_PKEY_free(key);
    error = "stored identity key and certificate do not match";
    return nullptr;
  }

  Fingerprint fingerprint{};
  if (!spki_fingerprint(certificate, fingerprint)) {
    X509_free(certificate);
    EVP_PKEY_free(key);
    error = "unable to compute the identity fingerprint";
    return nullptr;
  }
  return std::unique_ptr<DeviceIdentity>(
      new DeviceIdentity(key, certificate, fingerprint));
}

std::string DeviceIdentity::certificate_pem() const {
  BIO* bio = BIO_new(BIO_s_mem());
  if (bio == nullptr) return {};
  std::string result;
  if (PEM_write_bio_X509(bio, static_cast<X509*>(certificate_)) == 1) {
    result = bio_to_string(bio);
  }
  BIO_free(bio);
  return result;
}

std::string DeviceIdentity::private_key_pem() const {
  BIO* bio = BIO_new(BIO_s_mem());
  if (bio == nullptr) return {};
  std::string result;
  if (PEM_write_bio_PrivateKey(bio, static_cast<EVP_PKEY*>(key_), nullptr,
                               nullptr, 0, nullptr, nullptr) == 1) {
    result = bio_to_string(bio);
  }
  // The BIO buffer held the private key; wipe it before release.
  char* data = nullptr;
  const auto length = BIO_get_mem_data(bio, &data);
  if (data != nullptr && length > 0) {
    OPENSSL_cleanse(data, static_cast<std::size_t>(length));
  }
  BIO_free(bio);
  return result;
}

}  // namespace lanaudio::security
