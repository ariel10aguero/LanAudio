#include "lanaudio/security/credential_store.hpp"

#ifdef LANAUDIO_HAS_KEYCHAIN

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <utility>
#include <vector>

namespace lanaudio::security {
namespace {

std::string status_message(OSStatus status, const char* operation) {
  std::string message(operation);
  CFStringRef description = SecCopyErrorMessageString(status, nullptr);
  if (description != nullptr) {
    char buffer[256] = {};
    if (CFStringGetCString(description, buffer, sizeof(buffer),
                           kCFStringEncodingUTF8)) {
      message += ": ";
      message += buffer;
    }
    CFRelease(description);
  }
  message += " (OSStatus " + std::to_string(static_cast<int>(status)) + ")";
  return message;
}

// Owns a CFTypeRef so early returns cannot leak Core Foundation objects.
class CfHandle {
 public:
  explicit CfHandle(CFTypeRef value = nullptr) : value_(value) {}
  CfHandle(const CfHandle&) = delete;
  CfHandle& operator=(const CfHandle&) = delete;
  ~CfHandle() {
    if (value_ != nullptr) CFRelease(value_);
  }
  [[nodiscard]] CFTypeRef get() const noexcept { return value_; }
  CFTypeRef* slot() noexcept { return &value_; }

 private:
  CFTypeRef value_{};
};

CFDictionaryRef make_query(const std::string& service,
                           const std::string& account) {
  CFStringRef service_ref = CFStringCreateWithBytes(
      nullptr, reinterpret_cast<const UInt8*>(service.data()),
      static_cast<CFIndex>(service.size()), kCFStringEncodingUTF8, false);
  CFStringRef account_ref = CFStringCreateWithBytes(
      nullptr, reinterpret_cast<const UInt8*>(account.data()),
      static_cast<CFIndex>(account.size()), kCFStringEncodingUTF8, false);
  if (service_ref == nullptr || account_ref == nullptr) {
    if (service_ref != nullptr) CFRelease(service_ref);
    if (account_ref != nullptr) CFRelease(account_ref);
    return nullptr;
  }
  const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
  const void* values[] = {kSecClassGenericPassword, service_ref, account_ref};
  CFDictionaryRef query = CFDictionaryCreate(
      nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CFRelease(service_ref);
  CFRelease(account_ref);
  return query;
}

class KeychainCredentialStore final : public CredentialStore {
 public:
  explicit KeychainCredentialStore(std::string service)
      : service_(std::move(service)) {}

  bool load(const std::string& account, std::string& secret,
            std::string& error) override {
    CfHandle query(make_query(service_, account));
    if (query.get() == nullptr) {
      error = "unable to build the Keychain query";
      return false;
    }
    auto* mutable_query = CFDictionaryCreateMutableCopy(
        nullptr, 0, static_cast<CFDictionaryRef>(query.get()));
    if (mutable_query == nullptr) {
      error = "unable to build the Keychain query";
      return false;
    }
    CFDictionarySetValue(mutable_query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(mutable_query, kSecMatchLimit, kSecMatchLimitOne);
    CfHandle owned_query(mutable_query);

    CfHandle result;
    const OSStatus status = SecItemCopyMatching(
        static_cast<CFDictionaryRef>(owned_query.get()), result.slot());
    if (status != errSecSuccess) {
      error = status == errSecItemNotFound
                  ? "no stored credential"
                  : status_message(status, "Keychain read failed");
      return false;
    }
    auto data = static_cast<CFDataRef>(result.get());
    if (data == nullptr) {
      error = "Keychain returned no data";
      return false;
    }
    const auto* bytes = CFDataGetBytePtr(data);
    const auto length = CFDataGetLength(data);
    secret.assign(reinterpret_cast<const char*>(bytes),
                  static_cast<std::size_t>(length));
    return true;
  }

  bool save(const std::string& account, const std::string& secret,
            std::string& error) override {
    CfHandle query(make_query(service_, account));
    if (query.get() == nullptr) {
      error = "unable to build the Keychain query";
      return false;
    }
    CFDataRef data = CFDataCreate(
        nullptr, reinterpret_cast<const UInt8*>(secret.data()),
        static_cast<CFIndex>(secret.size()));
    if (data == nullptr) {
      error = "unable to wrap the credential for the Keychain";
      return false;
    }
    CfHandle owned_data(data);

    const void* update_keys[] = {kSecValueData};
    const void* update_values[] = {data};
    CfHandle update(CFDictionaryCreate(nullptr, update_keys, update_values, 1,
                                       &kCFTypeDictionaryKeyCallBacks,
                                       &kCFTypeDictionaryValueCallBacks));
    OSStatus status =
        SecItemUpdate(static_cast<CFDictionaryRef>(query.get()),
                      static_cast<CFDictionaryRef>(update.get()));
    if (status == errSecItemNotFound) {
      auto* insert = CFDictionaryCreateMutableCopy(
          nullptr, 0, static_cast<CFDictionaryRef>(query.get()));
      if (insert == nullptr) {
        error = "unable to build the Keychain insert";
        return false;
      }
      CFDictionarySetValue(insert, kSecValueData, data);
      CFDictionarySetValue(insert, kSecAttrAccessible,
                           kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
      CfHandle owned_insert(insert);
      status = SecItemAdd(static_cast<CFDictionaryRef>(owned_insert.get()),
                          nullptr);
    }
    if (status != errSecSuccess) {
      error = status_message(status, "Keychain write failed");
      return false;
    }
    return true;
  }

  bool erase(const std::string& account, std::string& error) override {
    CfHandle query(make_query(service_, account));
    if (query.get() == nullptr) {
      error = "unable to build the Keychain query";
      return false;
    }
    const OSStatus status =
        SecItemDelete(static_cast<CFDictionaryRef>(query.get()));
    if (status != errSecSuccess && status != errSecItemNotFound) {
      error = status_message(status, "Keychain delete failed");
      return false;
    }
    return true;
  }

  [[nodiscard]] const char* backend_name() const noexcept override {
    return "macOS Keychain";
  }
  [[nodiscard]] bool is_secure() const noexcept override { return true; }

 private:
  std::string service_;
};

}  // namespace

std::shared_ptr<CredentialStore> make_keychain_credential_store(
    std::string service) {
  return std::make_shared<KeychainCredentialStore>(std::move(service));
}

}  // namespace lanaudio::security

#endif  // LANAUDIO_HAS_KEYCHAIN
