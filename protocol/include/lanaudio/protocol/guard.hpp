#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace lanaudio::protocol {

// Token bucket used for control messages, discovery records, and media
// datagrams. Time is supplied by the caller so the behavior is deterministic
// in tests and free of clock calls on hot paths.
class RateLimiter {
 public:
  RateLimiter() noexcept = default;
  RateLimiter(double permits_per_second, double burst) noexcept;

  [[nodiscard]] bool allow(std::chrono::steady_clock::time_point now,
                           double cost = 1.0) noexcept;
  void reset() noexcept;
  [[nodiscard]] double available() const noexcept { return tokens_; }

 private:
  double rate_{1'000.0};
  double burst_{1'000.0};
  double tokens_{1'000.0};
  std::chrono::steady_clock::time_point last_{};
  bool initialized_{};
};

// Fail-closed abuse guard. Repeated invalid input moves a peer into a blocked
// state for a cool-down period instead of being ignored silently.
class AbuseGuard {
 public:
  struct Config {
    std::uint32_t invalid_threshold{32};
    std::chrono::seconds invalid_window{std::chrono::seconds(10)};
    std::chrono::seconds block_duration{std::chrono::seconds(30)};
    double permits_per_second{5'000.0};
    double burst{10'000.0};
  };

  AbuseGuard() noexcept;
  explicit AbuseGuard(Config config) noexcept;

  // Returns false when the datagram or message must be dropped because the
  // peer is blocked or exceeded its rate allowance.
  [[nodiscard]] bool accept(std::chrono::steady_clock::time_point now) noexcept;

  // Records a rejected message. Returns true when the peer became blocked.
  [[nodiscard]] bool record_invalid(
      std::chrono::steady_clock::time_point now) noexcept;

  void record_valid() noexcept;
  void reset() noexcept;

  [[nodiscard]] bool blocked(
      std::chrono::steady_clock::time_point now) const noexcept;
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
  [[nodiscard]] std::uint64_t invalid_total() const noexcept {
    return invalid_total_;
  }

 private:
  Config config_;
  RateLimiter limiter_;
  std::uint32_t invalid_in_window_{};
  std::uint64_t invalid_total_{};
  std::uint64_t dropped_{};
  std::chrono::steady_clock::time_point window_start_{};
  std::chrono::steady_clock::time_point blocked_until_{};
  bool window_open_{};
};

}  // namespace lanaudio::protocol
