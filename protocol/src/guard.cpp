#include "lanaudio/protocol/guard.hpp"

#include <algorithm>

namespace lanaudio::protocol {
namespace {

double seconds_between(std::chrono::steady_clock::time_point from,
                       std::chrono::steady_clock::time_point to) noexcept {
  if (to <= from) return 0.0;
  return std::chrono::duration<double>(to - from).count();
}

}  // namespace

RateLimiter::RateLimiter(double permits_per_second, double burst) noexcept
    : rate_(permits_per_second > 0.0 ? permits_per_second : 1.0),
      burst_(burst > 0.0 ? burst : 1.0),
      tokens_(burst > 0.0 ? burst : 1.0) {}

bool RateLimiter::allow(std::chrono::steady_clock::time_point now,
                        double cost) noexcept {
  if (!initialized_) {
    last_ = now;
    initialized_ = true;
  }
  tokens_ = std::min(burst_, tokens_ + seconds_between(last_, now) * rate_);
  last_ = now;
  if (tokens_ < cost) return false;
  tokens_ -= cost;
  return true;
}

void RateLimiter::reset() noexcept {
  tokens_ = burst_;
  initialized_ = false;
}

AbuseGuard::AbuseGuard() noexcept : AbuseGuard(Config{}) {}

AbuseGuard::AbuseGuard(Config config) noexcept
    : config_(config),
      limiter_(config.permits_per_second, config.burst) {}

bool AbuseGuard::blocked(
    std::chrono::steady_clock::time_point now) const noexcept {
  return now < blocked_until_;
}

bool AbuseGuard::accept(std::chrono::steady_clock::time_point now) noexcept {
  if (blocked(now)) {
    ++dropped_;
    return false;
  }
  if (!limiter_.allow(now)) {
    ++dropped_;
    return false;
  }
  return true;
}

bool AbuseGuard::record_invalid(
    std::chrono::steady_clock::time_point now) noexcept {
  ++invalid_total_;
  if (!window_open_ || now - window_start_ > config_.invalid_window) {
    window_start_ = now;
    invalid_in_window_ = 0;
    window_open_ = true;
  }
  ++invalid_in_window_;
  if (invalid_in_window_ >= config_.invalid_threshold) {
    blocked_until_ = now + config_.block_duration;
    invalid_in_window_ = 0;
    window_open_ = false;
    limiter_.reset();
    return true;
  }
  return false;
}

void AbuseGuard::record_valid() noexcept {
  if (invalid_in_window_ > 0) --invalid_in_window_;
}

void AbuseGuard::reset() noexcept {
  limiter_.reset();
  invalid_in_window_ = 0;
  window_open_ = false;
  blocked_until_ = {};
}

}  // namespace lanaudio::protocol
