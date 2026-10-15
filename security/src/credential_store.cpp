#include "lanaudio/security/credential_store.hpp"

#include <map>
#include <mutex>
#include <string_view>

namespace lanaudio::security {
namespace {

constexpr std::string_view kIdentityKeyAccount = "device-identity-key";
constexpr std::string_view kIdentityCertificateAccount =
    "device-identity-certificate";

class MemoryCredentialStore final : public CredentialStore {
 public:
  bool load(const std::string& account, std::string& secret,
            std::string& error) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = entries_.find(account);
    if (entry == entries_.end()) {
      error = "no stored credential";
      return false;
    }
    secret = entry->second;
    return true;
  }

  bool save(const std::string& account, const std::string& secret,
            std::string&) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    entries_[account] = secret;
    return true;
  }

  bool erase(const std::string& account, std::string&) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = entries_.find(account);
    if (entry != entries_.end()) {
      secure_clear(entry->second);
      entries_.erase(entry);
    }
    return true;
  }

  ~MemoryCredentialStore() override {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) secure_clear(entry.second);
  }

  [[nodiscard]] const char* backend_name() const noexcept override {
    return "in-memory (development)";
  }
  [[nodiscard]] bool is_secure() const noexcept override { return false; }

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::string> entries_;
};

}  // namespace

std::shared_ptr<CredentialStore> make_memory_credential_store() {
  return std::make_shared<MemoryCredentialStore>();
}

std::shared_ptr<CredentialStore> make_default_credential_store(
    std::string service) {
#ifdef LANAUDIO_HAS_KEYCHAIN
  return make_keychain_credential_store(std::move(service));
#else
  (void)service;
  return make_memory_credential_store();
#endif
}

IdentityManager::IdentityManager(std::shared_ptr<CredentialStore> store,
                                 std::string device_name)
    : store_(std::move(store)), device_name_(std::move(device_name)) {}

DeviceIdentity* IdentityManager::identity(std::string& error) {
  if (identity_) return identity_.get();

  std::string key_pem;
  std::string certificate_pem;
  std::string load_error;
  const bool have_key = store_->load(std::string(kIdentityKeyAccount), key_pem,
                                     load_error);
  const bool have_certificate = store_->load(
      std::string(kIdentityCertificateAccount), certificate_pem, load_error);

  if (have_key && have_certificate) {
    identity_ = DeviceIdentity::from_pem(key_pem, certificate_pem, error);
    secure_clear(key_pem);
    if (identity_) return identity_.get();
    // A corrupt or mismatched stored identity is replaced rather than
    // leaving the application permanently unable to authenticate.
    error.clear();
  }
  secure_clear(key_pem);

  auto fresh = DeviceIdentity::generate(device_name_, error);
  if (!fresh) return nullptr;

  auto new_key = fresh->private_key_pem();
  const auto new_certificate = fresh->certificate_pem();
  if (new_key.empty() || new_certificate.empty()) {
    secure_clear(new_key);
    error = "unable to serialize the new device identity";
    return nullptr;
  }
  const bool stored =
      store_->save(std::string(kIdentityKeyAccount), new_key, error) &&
      store_->save(std::string(kIdentityCertificateAccount), new_certificate,
                   error);
  secure_clear(new_key);
  if (!stored) return nullptr;

  identity_ = std::move(fresh);
  return identity_.get();
}

bool IdentityManager::reset(std::string& error) {
  identity_.reset();
  const bool key_erased =
      store_->erase(std::string(kIdentityKeyAccount), error);
  const bool certificate_erased =
      store_->erase(std::string(kIdentityCertificateAccount), error);
  return key_erased && certificate_erased;
}

}  // namespace lanaudio::security
