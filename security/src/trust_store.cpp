#include "lanaudio/security/trust_store.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

namespace lanaudio::security {
namespace {

constexpr std::size_t kMaxTrustedPeers = 64;
constexpr std::size_t kMaxNameLength = 64;

std::string sanitize_name(const std::string& name) {
  std::string clean;
  clean.reserve(std::min(name.size(), kMaxNameLength));
  for (const char character : name) {
    if (clean.size() >= kMaxNameLength) break;
    const auto octet = static_cast<unsigned char>(character);
    if (octet < 0x20 || octet == 0x7f || character == '\t' ||
        character == '\n') {
      continue;
    }
    clean.push_back(character);
  }
  if (clean.empty()) clean = "Unnamed device";
  return clean;
}

std::uint64_t now_seconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

}  // namespace

TrustStore::TrustStore(std::shared_ptr<CredentialStore> store,
                       std::string account)
    : store_(std::move(store)), account_(std::move(account)) {}

bool TrustStore::load(std::string& error) {
  const std::lock_guard<std::mutex> lock(mutex_);
  peers_.clear();
  loaded_ = true;

  std::string serialized;
  std::string load_error;
  if (!store_->load(account_, serialized, load_error)) {
    // An empty trust store is a valid starting state.
    return true;
  }

  std::istringstream stream(serialized);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty()) continue;
    if (peers_.size() >= kMaxTrustedPeers) break;
    const auto first_tab = line.find('\t');
    if (first_tab == std::string::npos) continue;
    const auto second_tab = line.find('\t', first_tab + 1);
    if (second_tab == std::string::npos) continue;

    TrustedPeer peer;
    if (!parse_hex_fingerprint(line.substr(0, first_tab), peer.fingerprint)) {
      continue;
    }
    try {
      peer.paired_unix_seconds = std::stoull(
          line.substr(first_tab + 1, second_tab - first_tab - 1));
    } catch (...) {
      continue;
    }
    peer.name = sanitize_name(line.substr(second_tab + 1));
    peers_.push_back(std::move(peer));
  }
  (void)error;
  return true;
}

bool TrustStore::persist(std::string& error) {
  std::string serialized;
  for (const auto& peer : peers_) {
    serialized += to_hex(peer.fingerprint);
    serialized += '\t';
    serialized += std::to_string(peer.paired_unix_seconds);
    serialized += '\t';
    serialized += peer.name;
    serialized += '\n';
  }
  return store_->save(account_, serialized, error);
}

bool TrustStore::contains(const Fingerprint& fingerprint) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return std::any_of(peers_.begin(), peers_.end(),
                     [&fingerprint](const TrustedPeer& peer) {
                       return peer.fingerprint == fingerprint;
                     });
}

std::optional<TrustedPeer> TrustStore::find(
    const Fingerprint& fingerprint) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto entry = std::find_if(peers_.begin(), peers_.end(),
                                  [&fingerprint](const TrustedPeer& peer) {
                                    return peer.fingerprint == fingerprint;
                                  });
  if (entry == peers_.end()) return std::nullopt;
  return *entry;
}

std::vector<TrustedPeer> TrustStore::peers() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return peers_;
}

bool TrustStore::add(const Fingerprint& fingerprint, const std::string& name,
                     std::string& error) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) {
    error = "trust store was not loaded";
    return false;
  }
  auto entry = std::find_if(peers_.begin(), peers_.end(),
                            [&fingerprint](const TrustedPeer& peer) {
                              return peer.fingerprint == fingerprint;
                            });
  if (entry != peers_.end()) {
    entry->name = sanitize_name(name);
    entry->paired_unix_seconds = now_seconds();
    return persist(error);
  }
  if (peers_.size() >= kMaxTrustedPeers) {
    error = "the trust store is full; remove a device before pairing another";
    return false;
  }
  peers_.push_back(TrustedPeer{fingerprint, sanitize_name(name),
                               now_seconds()});
  return persist(error);
}

bool TrustStore::rename(const Fingerprint& fingerprint, const std::string& name,
                        std::string& error) {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto entry = std::find_if(peers_.begin(), peers_.end(),
                            [&fingerprint](const TrustedPeer& peer) {
                              return peer.fingerprint == fingerprint;
                            });
  if (entry == peers_.end()) {
    error = "no trusted device with that fingerprint";
    return false;
  }
  entry->name = sanitize_name(name);
  return persist(error);
}

bool TrustStore::remove(const Fingerprint& fingerprint, std::string& error) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto removed = std::remove_if(peers_.begin(), peers_.end(),
                                      [&fingerprint](const TrustedPeer& peer) {
                                        return peer.fingerprint == fingerprint;
                                      });
  if (removed == peers_.end()) {
    error = "no trusted device with that fingerprint";
    return false;
  }
  peers_.erase(removed, peers_.end());
  return persist(error);
}

bool TrustStore::reset(std::string& error) {
  const std::lock_guard<std::mutex> lock(mutex_);
  peers_.clear();
  loaded_ = true;
  return store_->erase(account_, error);
}

}  // namespace lanaudio::security
