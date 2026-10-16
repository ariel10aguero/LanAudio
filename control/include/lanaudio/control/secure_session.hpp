#pragma once

#include "lanaudio/control/control_session.hpp"
#include "lanaudio/control/discovery.hpp"
#include "lanaudio/security/credential_store.hpp"
#include "lanaudio/stream.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace lanaudio::control {

inline constexpr std::uint16_t kDefaultControlPort = 5005;
inline constexpr std::uint16_t kDefaultMediaPort = 5004;

// User-visible lifecycle. The media path only runs in secure_session_ready
// and streaming; every other state keeps the audio buttons disabled.
enum class SessionState {
  disconnected,
  scanning,
  device_found,
  pairing_required,
  confirm_code,
  pairing_complete,
  authenticating,
  secure_session_ready,
  streaming,
  connection_lost,
  trust_revoked,
};

[[nodiscard]] std::string describe(SessionState state);
[[nodiscard]] bool allows_media(SessionState state) noexcept;

struct SessionSnapshot {
  SessionState state{SessionState::disconnected};
  std::string message;
  std::string peer_name;
  std::string peer_fingerprint;  // grouped short form, never a secret
  std::string pending_code;      // six digits while confirming
  std::string media_profile;
  protocol::SessionParameters parameters{};
  bool media_running{};
  // The control channel is healthy but no audio has arrived recently.
  bool media_stalled{};
};

// Blocks the control worker while the user compares the six-digit code.
class PairingGate {
 public:
  // Worker side. Returns the user's decision, or false on timeout.
  // A Codes-match click that arrives slightly before the worker starts
  // waiting is latched so the GUI cannot lose the confirmation.
  [[nodiscard]] bool await_decision(const std::string& code,
                                    std::chrono::milliseconds timeout);
  // UI side.
  void decide(bool accept);
  void cancel();
  [[nodiscard]] std::string pending_code() const;
  [[nodiscard]] bool waiting() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable signal_;
  std::string code_;
  bool waiting_{};
  bool decided_{};
  bool accepted_{};
};

// Shared device state: the persistent identity and the pinned peer list.
class SecurityContext {
 public:
  [[nodiscard]] static std::shared_ptr<SecurityContext> create(
      std::string device_name, std::string application_version,
      std::string& error);
  // Test and headless variant that never touches the platform keychain.
  [[nodiscard]] static std::shared_ptr<SecurityContext> create_ephemeral(
      std::string device_name, std::string application_version,
      std::string& error);

  [[nodiscard]] security::DeviceIdentity& identity() noexcept {
    return *identity_;
  }
  [[nodiscard]] security::TrustStore& trust() noexcept { return *trust_; }
  [[nodiscard]] const LocalInfo& local() const noexcept { return local_; }
  [[nodiscard]] std::string backend_name() const {
    return store_->backend_name();
  }
  [[nodiscard]] bool storage_is_secure() const { return store_->is_secure(); }

 private:
  SecurityContext() = default;

  [[nodiscard]] static std::shared_ptr<SecurityContext> build(
      std::shared_ptr<security::CredentialStore> store,
      std::string device_name, std::string application_version,
      std::string& error);

  std::shared_ptr<security::CredentialStore> store_;
  std::unique_ptr<security::IdentityManager> identities_;
  security::DeviceIdentity* identity_{};  // owned by identities_
  std::unique_ptr<security::TrustStore> trust_;
  LocalInfo local_;
};

// Base worker shared by both directions: state publication, pairing gate,
// heartbeat supervision, and fail-closed teardown.
class SecureEndpoint {
 public:
  virtual ~SecureEndpoint();
  SecureEndpoint(const SecureEndpoint&) = delete;
  SecureEndpoint& operator=(const SecureEndpoint&) = delete;

  [[nodiscard]] SessionSnapshot snapshot() const;
  [[nodiscard]] PairingGate& pairing_gate() noexcept { return gate_; }
  void stop() noexcept;
  void note_media_stalled(bool stalled) { set_media_stalled(stalled); }
  [[nodiscard]] bool running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

 protected:
  explicit SecureEndpoint(std::shared_ptr<SecurityContext> context);

  void publish(SessionState state, std::string message);
  void publish_peer(const PeerInfo& peer);
  void publish_parameters(const protocol::SessionParameters& parameters,
                          std::string profile);
  void set_media_running(bool running);
  void set_media_stalled(bool stalled);
  void attach_channel(TlsChannel& channel) noexcept;
  void detach_channel() noexcept;
  void interrupt_channel() noexcept;
  virtual void interrupt_io() noexcept;

  struct ChannelGuard {
    SecureEndpoint& endpoint;
    ChannelGuard(SecureEndpoint& endpoint, TlsChannel& channel) noexcept;
    ~ChannelGuard();
    ChannelGuard(const ChannelGuard&) = delete;
    ChannelGuard& operator=(const ChannelGuard&) = delete;
  };
  [[nodiscard]] bool should_stop() const noexcept {
    return stop_requested_.load(std::memory_order_acquire);
  }

  // Shared handshake tail: hello, trust evaluation, optional pairing.
  [[nodiscard]] bool authenticate(TlsChannel& channel, bool allow_pairing,
                                  PeerInfo& peer, std::string& error);

  std::shared_ptr<SecurityContext> context_;
  PairingGate gate_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread worker_;

 private:
  mutable std::mutex mutex_;
  SessionSnapshot snapshot_;
  std::mutex channel_mutex_;
  TlsChannel* active_channel_{};
};

class SecureSender final : public SecureEndpoint {
 public:
  struct Options {
    std::string host;
    std::uint16_t control_port{kDefaultControlPort};
    std::uint32_t preferred_sample_rate{protocol::kPreferredSampleRate};
    std::uint16_t preferred_jitter_packets{4};
    std::uint32_t preferred_packet_microseconds{
        protocol::kPreferredPacketMicroseconds};
    bool allow_pairing{true};
  };

  explicit SecureSender(std::shared_ptr<SecurityContext> context,
                        Options options);
  ~SecureSender() override;

  [[nodiscard]] bool start(std::string& error);
  void push_audio(std::span<const float> interleaved) noexcept;
  [[nodiscard]] const StreamMetrics* metrics() const noexcept;
  [[nodiscard]] AudioFormat negotiated_format() const noexcept;

 private:
  void run() noexcept;
  void teardown() noexcept;
  void interrupt_io() noexcept override;

  Options options_;
  mutable std::mutex media_mutex_;
  std::unique_ptr<srtp::SrtpProtector> protector_;
  std::unique_ptr<NetworkSender> sender_;
  std::atomic<AudioFormat> format_{AudioFormat{}};
};

class SecureReceiver final : public SecureEndpoint {
 public:
  struct Options {
    std::uint16_t control_port{kDefaultControlPort};
    std::uint16_t media_port{kDefaultMediaPort};
    std::uint32_t preferred_sample_rate{protocol::kPreferredSampleRate};
    bool allow_pairing{true};
    bool advertise{true};
  };

  explicit SecureReceiver(std::shared_ptr<SecurityContext> context,
                          Options options);
  ~SecureReceiver() override;

  [[nodiscard]] bool start(std::string& error);
  void render_audio(std::span<float> interleaved) noexcept;
  [[nodiscard]] const StreamMetrics* metrics() const noexcept;
  [[nodiscard]] AudioFormat negotiated_format() const noexcept;
  [[nodiscard]] std::uint16_t control_port() const noexcept;

 private:
  void run() noexcept;
  void serve(TlsChannel& channel) noexcept;
  void teardown() noexcept;
  void interrupt_io() noexcept override;

  Options options_;
  std::unique_ptr<TlsListener> listener_;
  std::unique_ptr<ServiceAdvertiser> advertiser_;
  mutable std::mutex media_mutex_;
  std::unique_ptr<srtp::SrtpProtector> protector_;
  std::unique_ptr<NetworkReceiver> receiver_;
  std::atomic<AudioFormat> format_{AudioFormat{}};
};

}  // namespace lanaudio::control
