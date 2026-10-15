#include "lanaudio/srtp/srtp_protector.hpp"

// libsrtp2 exposes this header directly on its include path, whether it was
// fetched as a source dependency or installed system wide.
#include <srtp.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace lanaudio::srtp {
namespace {

std::once_flag g_init_once;
bool g_initialized = false;
srtp_err_status_t g_init_status = srtp_err_status_ok;

void wipe(std::span<std::uint8_t> buffer) noexcept {
  // volatile keeps the compiler from eliding the overwrite.
  volatile std::uint8_t* target = buffer.data();
  for (std::size_t i = 0; i < buffer.size(); ++i) target[i] = 0;
}

}  // namespace

bool initialize_library(std::string& error) {
  std::call_once(g_init_once, [] {
    g_init_status = srtp_init();
    g_initialized = g_init_status == srtp_err_status_ok;
  });
  if (!g_initialized) {
    error = "libsrtp initialization failed with status " +
            std::to_string(static_cast<int>(g_init_status));
    return false;
  }
  return true;
}

SrtpProtector::SrtpProtector(void* session, Direction direction,
                             std::array<std::uint8_t, kKeyWithSaltBytes> key)
    : session_(session), direction_(direction), key_(key) {}

SrtpProtector::~SrtpProtector() {
  if (session_ != nullptr) {
    srtp_dealloc(static_cast<srtp_t>(session_));
    session_ = nullptr;
  }
  wipe(key_);
}

std::unique_ptr<SrtpProtector> SrtpProtector::create(
    Direction direction, bool local_is_client, std::uint32_t ssrc,
    std::span<const std::uint8_t> key_material, std::string& error) {
  if (!initialize_library(error)) return nullptr;
  if (key_material.size() != kKeyMaterialBytes) {
    error = "SRTP key material has an unexpected length";
    return nullptr;
  }
  if (ssrc == 0) {
    error = "refusing to create an SRTP context without a negotiated SSRC";
    return nullptr;
  }

  // The first half of the exporter output keys the client's outbound
  // direction; the second half keys the server's outbound direction.
  const bool use_first_half = (direction == Direction::outbound)
                                  ? local_is_client
                                  : !local_is_client;
  std::array<std::uint8_t, kKeyWithSaltBytes> key{};
  std::memcpy(key.data(),
              key_material.data() + (use_first_half ? 0 : kKeyWithSaltBytes),
              kKeyWithSaltBytes);

  srtp_policy_t policy;
  std::memset(&policy, 0, sizeof(policy));
  srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtp);
  srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy.rtcp);
  // The media stream is one-directional and carries a single negotiated SSRC,
  // so both contexts are bound to that exact value.
  policy.ssrc.type = ssrc_specific;
  policy.ssrc.value = ssrc;
  policy.key = key.data();
  policy.window_size = 1024;  // replay protection window
  policy.allow_repeat_tx = 0;
  policy.next = nullptr;

  srtp_t session = nullptr;
  const auto status = srtp_create(&session, &policy);
  if (status != srtp_err_status_ok || session == nullptr) {
    wipe(key);
    error = "unable to create the SRTP context (status " +
            std::to_string(static_cast<int>(status)) + ")";
    return nullptr;
  }
  auto protector = std::unique_ptr<SrtpProtector>(
      new SrtpProtector(session, direction, key));
  wipe(key);
  return protector;
}

std::size_t SrtpProtector::protect(std::span<std::byte> buffer,
                                   std::size_t size) noexcept {
  last_failure_was_replay_ = false;
  if (session_ == nullptr || direction_ != Direction::outbound) return 0;
  if (size == 0 || size > buffer.size()) return 0;
  if (buffer.size() - size < kMediaProtectionOverheadBytes) return 0;

  auto length = static_cast<int>(size);
  const auto status = srtp_protect(static_cast<srtp_t>(session_),
                                   buffer.data(), &length);
  if (status != srtp_err_status_ok || length <= 0) return 0;
  return static_cast<std::size_t>(length);
}

std::size_t SrtpProtector::unprotect(std::span<std::byte> buffer,
                                     std::size_t size) noexcept {
  last_failure_was_replay_ = false;
  if (session_ == nullptr || direction_ != Direction::inbound) return 0;
  if (size == 0 || size > buffer.size()) return 0;

  auto length = static_cast<int>(size);
  const auto status = srtp_unprotect(static_cast<srtp_t>(session_),
                                     buffer.data(), &length);
  if (status != srtp_err_status_ok || length <= 0) {
    last_failure_was_replay_ = status == srtp_err_status_replay_fail ||
                               status == srtp_err_status_replay_old;
    return 0;
  }
  return static_cast<std::size_t>(length);
}

}  // namespace lanaudio::srtp
