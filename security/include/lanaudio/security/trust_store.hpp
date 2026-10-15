#pragma once

#include "lanaudio/security/credential_store.hpp"
#include "lanaudio/security/identity.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lanaudio::security {

struct TrustedPeer {
  Fingerprint fingerprint{};
  std::string name;
  std::uint64_t paired_unix_seconds{};
};

// Pinned peer identities. Backed by the platform credential store so trust
// cannot be modified by editing an ordinary file.
class TrustStore {
 public:
  explicit TrustStore(std::shared_ptr<CredentialStore> store,
                      std::string account = "trusted-peers");

  [[nodiscard]] bool load(std::string& error);
  [[nodiscard]] bool contains(const Fingerprint& fingerprint) const;
  [[nodiscard]] std::optional<TrustedPeer> find(
      const Fingerprint& fingerprint) const;
  [[nodiscard]] std::vector<TrustedPeer> peers() const;

  [[nodiscard]] bool add(const Fingerprint& fingerprint, const std::string& name,
                         std::string& error);
  [[nodiscard]] bool rename(const Fingerprint& fingerprint,
                            const std::string& name, std::string& error);
  [[nodiscard]] bool remove(const Fingerprint& fingerprint, std::string& error);
  [[nodiscard]] bool reset(std::string& error);

 private:
  [[nodiscard]] bool persist(std::string& error);

  std::shared_ptr<CredentialStore> store_;
  std::string account_;
  mutable std::mutex mutex_;
  std::vector<TrustedPeer> peers_;
  bool loaded_{};
};

}  // namespace lanaudio::security
