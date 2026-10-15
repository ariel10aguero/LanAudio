#include "lanaudio/core.hpp"

#include <algorithm>

namespace lanaudio {

DriftController::DriftController() noexcept : DriftController(Config{}) {}

DriftController::DriftController(Config config) noexcept : config_(config) {}

double DriftController::update(double occupancy_packets) noexcept {
  if (!initialized_) {
    filtered_occupancy_ = occupancy_packets;
    initialized_ = true;
  } else {
    filtered_occupancy_ +=
        config_.smoothing * (occupancy_packets - filtered_occupancy_);
  }

  // Positive error means excess buffered source audio. A positive correction
  // therefore asks the resampler to consume source samples slightly faster.
  const double error = filtered_occupancy_ - config_.target_packets;
  integral_ = std::clamp(integral_ + error, -config_.max_ppm / config_.ki,
                         config_.max_ppm / config_.ki);
  correction_ppm_ = std::clamp(config_.kp * error + config_.ki * integral_,
                               -config_.max_ppm, config_.max_ppm);
  return 1.0 + correction_ppm_ * 1.0e-6;
}

void DriftController::reset() noexcept {
  filtered_occupancy_ = 0.0;
  integral_ = 0.0;
  correction_ppm_ = 0.0;
  initialized_ = false;
}

}  // namespace lanaudio
