#include "lanaudio/core.hpp"

#include <algorithm>
#include <cmath>

namespace lanaudio {

DriftController::DriftController() noexcept : DriftController(Config{}) {}

DriftController::DriftController(Config config) noexcept : config_(config) {}

double DriftController::update(double occupancy_packets,
                               double elapsed_seconds) noexcept {
  const double dt = std::max(elapsed_seconds, 0.0);
  if (!initialized_) {
    filtered_occupancy_ = occupancy_packets;
    initialized_ = true;
  } else {
    const double alpha =
        config_.smoothing_seconds > 0.0
            ? 1.0 - std::exp(-dt / config_.smoothing_seconds)
            : 1.0;
    filtered_occupancy_ += alpha * (occupancy_packets - filtered_occupancy_);
  }

  // Positive error means excess buffered source audio. A positive correction
  // therefore asks the resampler to consume source samples slightly faster.
  const double error = filtered_occupancy_ - config_.target_packets;
  const double integral_limit =
      config_.ki > 0.0 ? config_.max_ppm / config_.ki : 0.0;
  integral_ = std::clamp(integral_ + error * dt, -integral_limit,
                         integral_limit);
  correction_ppm_ = std::clamp(config_.kp * error + config_.ki * integral_,
                               -config_.max_ppm, config_.max_ppm);
  return 1.0 + correction_ppm_ * 1.0e-6;
}

void DriftController::restart_filter() noexcept { initialized_ = false; }

void DriftController::reset() noexcept {
  filtered_occupancy_ = 0.0;
  integral_ = 0.0;
  correction_ppm_ = 0.0;
  initialized_ = false;
}

}  // namespace lanaudio
