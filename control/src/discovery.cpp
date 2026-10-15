#include "lanaudio/control/discovery.hpp"

#include <algorithm>
#include <cstring>

#ifdef LANAUDIO_HAS_BONJOUR
#include <arpa/inet.h>
#include <dns_sd.h>
#include <poll.h>
#endif

namespace lanaudio::control {
namespace {

constexpr std::size_t kMaxTextValueBytes = 128;

std::string sanitize(const char* value, std::size_t length) {
  std::string clean;
  clean.reserve(std::min(length, kMaxTextValueBytes));
  for (std::size_t i = 0; i < length && clean.size() < kMaxTextValueBytes;
       ++i) {
    const auto octet = static_cast<unsigned char>(value[i]);
    if (octet < 0x20 || octet == 0x7f) continue;
    clean.push_back(value[i]);
  }
  return clean;
}

#ifdef LANAUDIO_HAS_BONJOUR

std::string text_value(const unsigned char* record, std::uint16_t length,
                       const char* key) {
  std::uint8_t value_length = 0;
  const void* value = TXTRecordGetValuePtr(length, record, key,
                                           &value_length);
  if (value == nullptr) return {};
  return sanitize(static_cast<const char*>(value), value_length);
}

bool parse_port(const std::string& text, std::uint16_t& port) {
  if (text.empty() || text.size() > 5) return false;
  std::uint32_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') return false;
    value = value * 10 + static_cast<std::uint32_t>(digit - '0');
    if (value > 65'535) return false;
  }
  if (value == 0) return false;
  port = static_cast<std::uint16_t>(value);
  return true;
}

// Runs the DNS-SD socket until the deadline or until the operation is done.
bool pump(DNSServiceRef service, std::chrono::steady_clock::time_point deadline,
          const bool& done) {
  const int descriptor = DNSServiceRefSockFD(service);
  if (descriptor < 0) return false;
  while (!done) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return true;
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd descriptors{descriptor, POLLIN, 0};
    const int ready =
        poll(&descriptors, 1, static_cast<int>(remaining.count()));
    if (ready == 0) return true;
    if (ready < 0) return false;
    if (DNSServiceProcessResult(service) != kDNSServiceErr_NoError) {
      return false;
    }
  }
  return true;
}

class BonjourAdvertiser final : public ServiceAdvertiser {
 public:
  explicit BonjourAdvertiser(DNSServiceRef service) : service_(service) {}
  ~BonjourAdvertiser() override {
    if (service_ != nullptr) DNSServiceRefDeallocate(service_);
  }
  [[nodiscard]] bool active() const noexcept override {
    return service_ != nullptr;
  }
  [[nodiscard]] std::string backend_name() const override {
    return "Bonjour/DNS-SD";
  }

 private:
  DNSServiceRef service_{};
};

struct BrowseContext {
  std::vector<DiscoveredPeer> peers;
  bool more_coming{};
  bool failed{};
};

struct ResolveContext {
  DiscoveredPeer* peer{};
  bool done{};
};

void DNSSD_API on_resolve(DNSServiceRef /*service*/, DNSServiceFlags /*flags*/,
                          std::uint32_t /*interface_index*/,
                          DNSServiceErrorType error, const char* /*full_name*/,
                          const char* host_target, std::uint16_t port,
                          std::uint16_t text_length,
                          const unsigned char* text_record, void* context) {
  auto* resolve = static_cast<ResolveContext*>(context);
  resolve->done = true;
  if (error != kDNSServiceErr_NoError || resolve->peer == nullptr) return;

  resolve->peer->host = sanitize(host_target, std::strlen(host_target));
  resolve->peer->control_port = ntohs(port);

  const auto name = text_value(text_record, text_length, "name");
  const auto version = text_value(text_record, text_length, "ver");
  const auto protocol = text_value(text_record, text_length, "proto");
  const auto fingerprint = text_value(text_record, text_length, "fp");
  const auto control = text_value(text_record, text_length, "cport");

  resolve->peer->device_name = name;
  resolve->peer->application_version = version;
  std::uint16_t parsed_protocol = 0;
  if (parse_port(protocol, parsed_protocol)) {
    resolve->peer->protocol_version = parsed_protocol;
  }
  std::uint16_t advertised_control = 0;
  if (parse_port(control, advertised_control)) {
    resolve->peer->control_port = advertised_control;
  }
  security::Fingerprint digest{};
  if (security::parse_hex_fingerprint(fingerprint, digest)) {
    resolve->peer->fingerprint = digest;
  }

  // An advertisement is only usable when it is complete and in range. It is
  // still not trusted; the TLS handshake decides that.
  resolve->peer->well_formed = !resolve->peer->host.empty() &&
                               resolve->peer->control_port != 0 &&
                               !resolve->peer->device_name.empty() &&
                               resolve->peer->fingerprint.has_value();
}

void DNSSD_API on_browse(DNSServiceRef /*service*/, DNSServiceFlags flags,
                         std::uint32_t interface_index,
                         DNSServiceErrorType error, const char* service_name,
                         const char* registration_type, const char* domain,
                         void* context) {
  auto* browse = static_cast<BrowseContext*>(context);
  browse->more_coming = (flags & kDNSServiceFlagsMoreComing) != 0;
  if (error != kDNSServiceErr_NoError) {
    browse->failed = true;
    return;
  }
  if ((flags & kDNSServiceFlagsAdd) == 0) return;
  if (browse->peers.size() >= kMaxDiscoveredPeers) return;

  DiscoveredPeer peer;
  peer.service_name = sanitize(service_name, std::strlen(service_name));
  if (peer.service_name.empty()) return;

  ResolveContext resolve;
  resolve.peer = &peer;
  DNSServiceRef resolver = nullptr;
  if (DNSServiceResolve(&resolver, 0, interface_index, service_name,
                        registration_type, domain, on_resolve,
                        &resolve) != kDNSServiceErr_NoError) {
    return;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1'500);
  (void)pump(resolver, deadline, resolve.done);
  DNSServiceRefDeallocate(resolver);

  if (resolve.done) browse->peers.push_back(std::move(peer));
}

#endif  // LANAUDIO_HAS_BONJOUR

}  // namespace

bool discovery_available() noexcept {
#ifdef LANAUDIO_HAS_BONJOUR
  return true;
#else
  return false;
#endif
}

std::unique_ptr<ServiceAdvertiser> advertise_service(
    const Advertisement& advertisement, std::string& error) {
#ifdef LANAUDIO_HAS_BONJOUR
  if (advertisement.control_port == 0) {
    error = "cannot advertise a service without a control port";
    return nullptr;
  }
  const auto name = sanitize(advertisement.device_name.c_str(),
                             advertisement.device_name.size());
  const auto version = sanitize(advertisement.application_version.c_str(),
                                advertisement.application_version.size());
  const auto fingerprint = security::to_hex(advertisement.fingerprint);
  const auto protocol = std::to_string(advertisement.protocol_version);
  const auto control = std::to_string(advertisement.control_port);

  TXTRecordRef record;
  TXTRecordCreate(&record, 0, nullptr);
  const auto set = [&record](const char* key, const std::string& value) {
    return TXTRecordSetValue(&record, key,
                             static_cast<std::uint8_t>(value.size()),
                             value.c_str()) == kDNSServiceErr_NoError;
  };
  const bool populated = set("name", name) && set("ver", version) &&
                         set("proto", protocol) && set("fp", fingerprint) &&
                         set("cport", control);
  if (!populated) {
    TXTRecordDeallocate(&record);
    error = "unable to build the discovery metadata record";
    return nullptr;
  }

  DNSServiceRef service = nullptr;
  const auto status = DNSServiceRegister(
      &service, 0, 0, name.empty() ? nullptr : name.c_str(), kServiceType,
      nullptr, nullptr, htons(advertisement.control_port),
      TXTRecordGetLength(&record), TXTRecordGetBytesPtr(&record), nullptr,
      nullptr);
  TXTRecordDeallocate(&record);
  if (status != kDNSServiceErr_NoError) {
    error = "Bonjour registration failed with status " +
            std::to_string(static_cast<int>(status));
    return nullptr;
  }
  return std::make_unique<BonjourAdvertiser>(service);
#else
  (void)advertisement;
  error = "service discovery is not available on this platform";
  return nullptr;
#endif
}

std::vector<DiscoveredPeer> discover_peers(std::chrono::milliseconds timeout,
                                           std::string& error) {
#ifdef LANAUDIO_HAS_BONJOUR
  BrowseContext context;
  DNSServiceRef service = nullptr;
  const auto status = DNSServiceBrowse(&service, 0, 0, kServiceType, nullptr,
                                       on_browse, &context);
  if (status != kDNSServiceErr_NoError) {
    error = "Bonjour browse failed with status " +
            std::to_string(static_cast<int>(status));
    return {};
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  bool never = false;
  (void)pump(service, deadline, never);
  DNSServiceRefDeallocate(service);
  if (context.failed && context.peers.empty()) {
    error = "service discovery reported an error";
  }
  return std::move(context.peers);
#else
  (void)timeout;
  error = "service discovery is not available on this platform";
  return {};
#endif
}

}  // namespace lanaudio::control
