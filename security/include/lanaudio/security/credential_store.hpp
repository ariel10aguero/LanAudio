#pragma once

#include "lanaudio/security/identity.hpp"

#include <memory>
#include <string>

namespace lanaudio::security {

// Platform credential storage. The macOS backend uses the Keychain; the
// in-memory backend exists for tests and for clearly flagged development
// builds on platforms without a supported secure store.
class CredentialStore {
 public:
  virtual ~CredentialStore() = default;

  [[nodiscard]] virtual bool load(const std::string& account,
                                  std::string& secret,
                                  std::string& error) = 0;
  [[nodiscard]] virtual bool save(const std::string& account,
                                  const std::string& secret,
                                  std::string& error) = 0;
  [[nodiscard]] virtual bool erase(const std::string& account,
                                   std::string& error) = 0;
  [[nodiscard]] virtual const char* backend_name() const noexcept = 0;
  // True when the backend is protected by the operating system rather than
  // being a development stand-in.
  [[nodiscard]] virtual bool is_secure() const noexcept = 0;
};

[[nodiscard]] std::shared_ptr<CredentialStore> make_memory_credential_store();

#ifdef LANAUDIO_HAS_KEYCHAIN
[[nodiscard]] std::shared_ptr<CredentialStore> make_keychain_credential_store(
    std::string service);
#endif

// Keychain on macOS, in-memory elsewhere.
[[nodiscard]] std::shared_ptr<CredentialStore> make_default_credential_store(
    std::string service = "com.lanaudio.desktop");

// Loads the persisted device identity or creates and stores a new one.
class IdentityManager {
 public:
  explicit IdentityManager(std::shared_ptr<CredentialStore> store,
                           std::string device_name);

  // Returns the persistent identity, generating it on first use.
  [[nodiscard]] DeviceIdentity* identity(std::string& error);
  [[nodiscard]] bool reset(std::string& error);
  [[nodiscard]] const CredentialStore& store() const noexcept {
    return *store_;
  }
  [[nodiscard]] const std::string& device_name() const noexcept {
    return device_name_;
  }

 private:
  std::shared_ptr<CredentialStore> store_;
  std::string device_name_;
  std::unique_ptr<DeviceIdentity> identity_;
};

}  // namespace lanaudio::security
