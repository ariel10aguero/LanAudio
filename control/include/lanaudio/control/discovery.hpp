#pragma once

#include "lanaudio/security/identity.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lanaudio::control {

inline constexpr const char* kServiceType = "_lanaudio._tcp";
inline constexpr std::size_t kMaxDiscoveredPeers = 64;

// What a device publishes about itself. Every field is advisory: discovery
// only tells the user where to look, and nothing here grants trust.
struct Advertisement {
  std::string device_name;
  std::string application_version;
  std::uint16_t protocol_version{};
  security::Fingerprint fingerprint{};
  std::uint16_t control_port{};
};

struct DiscoveredPeer {
  std::string service_name;
  std::string host;
  std::uint16_t control_port{};
  std::string device_name;
  std::string application_version;
  std::uint16_t protocol_version{};
  std::optional<security::Fingerprint> fingerprint;
  bool trusted{};

  // True when every advertised field passed bounds and format checks.
  bool well_formed{};
};

// Publishes the control service while a receiver is listening.
class ServiceAdvertiser {
 public:
  virtual ~ServiceAdvertiser() = default;
  [[nodiscard]] virtual bool active() const noexcept = 0;
  [[nodiscard]] virtual std::string backend_name() const = 0;
};

[[nodiscard]] std::unique_ptr<ServiceAdvertiser> advertise_service(
    const Advertisement& advertisement, std::string& error);

// Blocking browse with a hard deadline and a bounded result set.
[[nodiscard]] std::vector<DiscoveredPeer> discover_peers(
    std::chrono::milliseconds timeout, std::string& error);

[[nodiscard]] bool discovery_available() noexcept;

}  // namespace lanaudio::control
