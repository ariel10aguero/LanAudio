#include "lanaudio/control/secure_session.hpp"

#include <algorithm>
#include <utility>

namespace lanaudio::control {
namespace {

constexpr std::chrono::milliseconds kAcceptPoll{250};
constexpr std::chrono::milliseconds kHeartbeatInterval{1'000};
constexpr std::chrono::milliseconds kHeartbeatTimeout{6'000};
constexpr std::chrono::milliseconds kConfirmWindow{120'000};
constexpr std::chrono::milliseconds kConnectTimeout{5'000};
constexpr std::chrono::milliseconds kMediaStallTimeout{3'000};

// Marks the snapshot when media has not moved for several seconds while the
// control channel is still authenticated. Silence is not treated as a stall
// until at least one packet has actually been exchanged.
void observe_media(SecureEndpoint& endpoint, const StreamMetrics* metrics,
                   std::chrono::steady_clock::time_point now) {
  if (metrics == nullptr) {
    endpoint.note_media_stalled(false);
    return;
  }
  const auto last = metrics->last_media_micros.load(std::memory_order_relaxed);
  if (last == 0) {
    endpoint.note_media_stalled(false);
    return;
  }
  const auto last_time = std::chrono::steady_clock::time_point(
      std::chrono::microseconds(last));
  endpoint.note_media_stalled(now - last_time > kMediaStallTimeout);
}

AudioFormat to_format(const protocol::SessionParameters& parameters) {
  AudioFormat format;
  format.sample_rate = parameters.sample_rate;
  format.channels = parameters.channels;
  format.frames_per_packet = parameters.frames_per_packet;
  return format;
}

}  // namespace

std::string describe(SessionState state) {
  switch (state) {
    case SessionState::disconnected:
      return "Disconnected";
    case SessionState::scanning:
      return "Scanning";
    case SessionState::device_found:
      return "Device found";
    case SessionState::pairing_required:
      return "Pairing required";
    case SessionState::confirm_code:
      return "Confirm matching code";
    case SessionState::pairing_complete:
      return "Pairing complete";
    case SessionState::authenticating:
      return "Authenticating";
    case SessionState::secure_session_ready:
      return "Secure session ready";
    case SessionState::streaming:
      return "Streaming";
    case SessionState::connection_lost:
      return "Connection lost";
    case SessionState::trust_revoked:
      return "Trust revoked";
  }
  return "Unknown";
}

bool allows_media(SessionState state) noexcept {
  return state == SessionState::secure_session_ready ||
         state == SessionState::streaming;
}

bool PairingGate::await_decision(const std::string& code,
                                 std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  code_ = code;
  waiting_ = true;
  // Keep an already-latched Codes-match click; only wait if the UI has not
  // confirmed yet.
  if (!decided_) {
    signal_.wait_for(lock, timeout, [this] { return decided_; });
  }
  waiting_ = false;
  code_.clear();
  const bool accepted = decided_ && accepted_;
  decided_ = false;
  accepted_ = false;
  return accepted;
}

void PairingGate::decide(bool accept) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    accepted_ = accept;
    decided_ = true;
  }
  signal_.notify_all();
}

void PairingGate::cancel() { decide(false); }

std::string PairingGate::pending_code() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return code_;
}

bool PairingGate::waiting() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return waiting_;
}

std::shared_ptr<SecurityContext> SecurityContext::create(
    std::string device_name, std::string application_version,
    std::string& error) {
  return build(security::make_default_credential_store(),
               std::move(device_name), std::move(application_version), error);
}

std::shared_ptr<SecurityContext> SecurityContext::create_ephemeral(
    std::string device_name, std::string application_version,
    std::string& error) {
  return build(security::make_memory_credential_store(),
               std::move(device_name), std::move(application_version), error);
}

std::shared_ptr<SecurityContext> SecurityContext::build(
    std::shared_ptr<security::CredentialStore> store, std::string device_name,
    std::string application_version, std::string& error) {
  auto context = std::shared_ptr<SecurityContext>(new SecurityContext());

  context->store_ = std::move(store);
  context->identities_ =
      std::make_unique<security::IdentityManager>(context->store_,
                                                  device_name);
  context->identity_ = context->identities_->identity(error);
  if (context->identity_ == nullptr) return nullptr;

  context->trust_ = std::make_unique<security::TrustStore>(context->store_);
  if (!context->trust_->load(error)) return nullptr;

  context->local_.device_name = std::move(device_name);
  context->local_.application_version = std::move(application_version);
  context->local_.fingerprint = context->identity_->fingerprint();
  return context;
}

SecureEndpoint::SecureEndpoint(std::shared_ptr<SecurityContext> context)
    : context_(std::move(context)) {}

SecureEndpoint::~SecureEndpoint() = default;

SessionSnapshot SecureEndpoint::snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

void SecureEndpoint::publish(SessionState state, std::string message) {
  const std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.state = state;
  snapshot_.message = std::move(message);
  if (state == SessionState::confirm_code) {
    snapshot_.pending_code = gate_.pending_code();
  } else {
    snapshot_.pending_code.clear();
  }
  if (!allows_media(state)) {
    snapshot_.media_running = false;
  }
}

void SecureEndpoint::publish_peer(const PeerInfo& peer) {
  const std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.peer_name = peer.device_name;
  snapshot_.peer_fingerprint = security::short_fingerprint(peer.fingerprint);
}

void SecureEndpoint::publish_parameters(
    const protocol::SessionParameters& parameters, std::string profile) {
  const std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.parameters = parameters;
  snapshot_.media_profile = std::move(profile);
}

void SecureEndpoint::set_media_running(bool running) {
  const std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.media_running = running;
  if (!running) snapshot_.media_stalled = false;
}

void SecureEndpoint::set_media_stalled(bool stalled) {
  const std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.media_stalled = stalled;
}

void SecureEndpoint::attach_channel(TlsChannel& channel) noexcept {
  const std::lock_guard<std::mutex> lock(channel_mutex_);
  active_channel_ = &channel;
}

void SecureEndpoint::detach_channel() noexcept {
  const std::lock_guard<std::mutex> lock(channel_mutex_);
  active_channel_ = nullptr;
}

void SecureEndpoint::interrupt_channel() noexcept {
  const std::lock_guard<std::mutex> lock(channel_mutex_);
  if (active_channel_ != nullptr) active_channel_->interrupt();
}

void SecureEndpoint::interrupt_io() noexcept { interrupt_channel(); }

SecureEndpoint::ChannelGuard::ChannelGuard(SecureEndpoint& endpoint,
                                           TlsChannel& channel) noexcept
    : endpoint(endpoint) {
  endpoint.attach_channel(channel);
}

SecureEndpoint::ChannelGuard::~ChannelGuard() { endpoint.detach_channel(); }

void SecureEndpoint::stop() noexcept {
  stop_requested_.store(true, std::memory_order_release);
  gate_.cancel();
  interrupt_io();
  if (worker_.joinable()) worker_.join();
  running_.store(false, std::memory_order_release);
}

bool SecureEndpoint::authenticate(TlsChannel& channel, bool allow_pairing,
                                  PeerInfo& peer, std::string& error) {
  publish(SessionState::authenticating,
          "Verifying the peer identity over TLS " +
              channel.negotiated_version());

  const auto status =
      exchange_hello(channel, context_->local(), peer, error);
  if (status != HandshakeStatus::ok) {
    if (error.empty()) error = describe(status);
    publish(SessionState::connection_lost, error);
    return false;
  }
  publish_peer(peer);

  if (context_->trust().contains(peer.fingerprint)) return true;

  if (!allow_pairing) {
    error = "device " + peer.device_name +
            " is not paired; enable pairing to trust it";
    publish(SessionState::trust_revoked, error);
    return false;
  }

  publish(SessionState::pairing_required,
          "Pairing required with " + peer.device_name);
  const ConfirmCodeCallback confirm = [this](const std::string& code,
                                             const PeerInfo&) {
    publish(SessionState::confirm_code,
            "Confirm that both devices show " + code);
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      snapshot_.pending_code = code;
    }
    return gate_.await_decision(code, kConfirmWindow);
  };

  const auto outcome = run_pairing(channel, context_->identity(), peer,
                                   confirm, context_->trust(), error);
  if (!outcome.trusted) {
    if (error.empty()) error = "pairing did not complete";
    publish(SessionState::trust_revoked, error);
    return false;
  }
  publish(SessionState::pairing_complete, outcome.detail);
  return true;
}

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------

SecureSender::SecureSender(std::shared_ptr<SecurityContext> context,
                           Options options)
    : SecureEndpoint(std::move(context)), options_(std::move(options)) {}

SecureSender::~SecureSender() {
  stop();
  teardown();
}

bool SecureSender::start(std::string& error) {
  if (running_.load(std::memory_order_acquire)) {
    error = "the sender is already running";
    return false;
  }
  if (options_.host.empty()) {
    error = "no receiver address was provided";
    return false;
  }
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  publish(SessionState::device_found, "Connecting to " + options_.host);
  worker_ = std::thread([this] { run(); });
  return true;
}

void SecureSender::teardown() noexcept {
  const std::lock_guard<std::mutex> lock(media_mutex_);
  if (sender_) {
    sender_->stop();
    sender_.reset();
  }
  // The protector outlives the stream thread so no packet can reference a
  // destroyed SRTP context.
  protector_.reset();
  set_media_running(false);
}

void SecureSender::interrupt_io() noexcept { interrupt_channel(); }

void SecureSender::run() noexcept {
  std::string error;
  auto channel = TlsChannel::connect(options_.host, options_.control_port,
                                     context_->identity(), kConnectTimeout,
                                     error);
  if (!channel) {
    publish(SessionState::connection_lost, error);
    running_.store(false, std::memory_order_release);
    return;
  }
  ChannelGuard attached(*this, *channel);

  PeerInfo peer;
  if (!authenticate(*channel, options_.allow_pairing, peer, error)) {
    running_.store(false, std::memory_order_release);
    return;
  }
  if (should_stop()) {
    publish(SessionState::disconnected, "Stopped");
    running_.store(false, std::memory_order_release);
    return;
  }

  NegotiatedSession session;
  const auto negotiated = negotiate_as_sender(
      *channel, local_capabilities(options_.preferred_sample_rate), session,
      error, options_.preferred_jitter_packets);
  if (negotiated != HandshakeStatus::ok) {
    if (error.empty()) error = describe(negotiated);
    publish(SessionState::connection_lost, error);
    running_.store(false, std::memory_order_release);
    return;
  }
  session.peer = peer;

  auto protector = srtp::SrtpProtector::create(
      srtp::Direction::outbound, session.local_is_client,
      session.parameters.ssrc, session.key_material, error);
  if (!protector) {
    publish(SessionState::connection_lost, error);
    running_.store(false, std::memory_order_release);
    return;
  }

  StreamConfig config;
  config.format = to_format(session.parameters);
  config.host = options_.host;
  config.port = session.parameters.media_port;
  config.ssrc = session.parameters.ssrc;
  config.payload_type = session.parameters.payload_type;
  config.session_id = session.parameters.session_id;
  config.jitter_packets = session.parameters.jitter_packets;
  config.require_protection = true;
  config.protector = protector.get();

  auto stream = std::make_unique<NetworkSender>(config);
  if (!stream->start(error)) {
    publish(SessionState::connection_lost, error);
    running_.store(false, std::memory_order_release);
    return;
  }

  {
    const std::lock_guard<std::mutex> lock(media_mutex_);
    protector_ = std::move(protector);
    sender_ = std::move(stream);
  }
  format_.store(config.format, std::memory_order_release);
  publish_parameters(session.parameters, srtp::kProfileName);
  publish(SessionState::secure_session_ready,
          "Secure session with " + peer.device_name + " at " +
              std::to_string(session.parameters.sample_rate) + " Hz");
  set_media_running(true);
  publish(SessionState::streaming,
          "Streaming to " + peer.device_name + " at " +
              std::to_string(session.parameters.sample_rate) + " Hz, " +
              std::to_string(session.parameters.packet_microseconds / 1000.0) +
              " ms packets");
  set_media_running(true);

  // Heartbeats keep the control channel authenticated. Losing it tears the
  // media down rather than letting unauthenticated audio continue.
  std::uint64_t counter = 0;
  auto last_seen = std::chrono::steady_clock::now();
  while (!should_stop()) {
    protocol::HeartbeatMessage beat;
    beat.counter = ++counter;
    beat.monotonic_micros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    std::vector<std::byte> frame;
    if (!protocol::encode(beat, frame) ||
        !channel->write_frame(frame, error)) {
      break;
    }

    std::vector<std::byte> response;
    bool timed_out = false;
    if (channel->read_frame(response, kHeartbeatInterval, timed_out, error)) {
      protocol::ControlMessageType type{};
      if (protocol::frame_type(response, type) == protocol::CodecError::none &&
          type == protocol::ControlMessageType::goodbye) {
        error = "the receiver closed the session";
        break;
      }
      last_seen = std::chrono::steady_clock::now();
    } else if (!timed_out) {
      break;
    }
    if (std::chrono::steady_clock::now() - last_seen > kHeartbeatTimeout) {
      error = "the receiver stopped responding";
      break;
    }
    observe_media(*this, metrics(), std::chrono::steady_clock::now());
  }

  teardown();
  if (should_stop()) {
    publish(SessionState::disconnected, "Stopped");
  } else {
    publish(SessionState::connection_lost,
            error.empty() ? "The secure session ended" : error);
  }
  running_.store(false, std::memory_order_release);
}

void SecureSender::push_audio(std::span<const float> interleaved) noexcept {
  // Audio callbacks must never block on the control worker, so this is a
  // try-lock: if a teardown is in progress the block is simply dropped.
  std::unique_lock<std::mutex> lock(media_mutex_, std::try_to_lock);
  if (!lock.owns_lock() || !sender_) return;
  sender_->push_audio(interleaved);
}

const StreamMetrics* SecureSender::metrics() const noexcept {
  std::unique_lock<std::mutex> lock(media_mutex_, std::try_to_lock);
  if (!lock.owns_lock() || !sender_) return nullptr;
  return &sender_->metrics();
}

AudioFormat SecureSender::negotiated_format() const noexcept {
  return format_.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Receiver
// ---------------------------------------------------------------------------

SecureReceiver::SecureReceiver(std::shared_ptr<SecurityContext> context,
                               Options options)
    : SecureEndpoint(std::move(context)), options_(std::move(options)) {}

SecureReceiver::~SecureReceiver() {
  stop();
  teardown();
}

std::uint16_t SecureReceiver::control_port() const noexcept {
  return listener_ ? listener_->port() : options_.control_port;
}

bool SecureReceiver::start(std::string& error) {
  if (running_.load(std::memory_order_acquire)) {
    error = "the receiver is already running";
    return false;
  }
  listener_ = TlsListener::bind(options_.control_port, error);
  if (!listener_) return false;

  if (options_.advertise && discovery_available()) {
    Advertisement advertisement;
    advertisement.device_name = context_->local().device_name;
    advertisement.application_version = context_->local().application_version;
    advertisement.protocol_version = protocol::kControlProtocolVersion;
    advertisement.fingerprint = context_->local().fingerprint;
    advertisement.control_port = listener_->port();
    std::string advertise_error;
    advertiser_ = advertise_service(advertisement, advertise_error);
    // Discovery is a convenience; the session is still reachable by address.
  }

  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  publish(SessionState::scanning,
          "Waiting for a sender on control port " +
              std::to_string(listener_->port()));
  worker_ = std::thread([this] { run(); });
  return true;
}

void SecureReceiver::teardown() noexcept {
  const std::lock_guard<std::mutex> lock(media_mutex_);
  if (receiver_) {
    receiver_->expire_session();
    receiver_->stop();
    receiver_.reset();
  }
  protector_.reset();
  set_media_running(false);
}

void SecureReceiver::interrupt_io() noexcept {
  if (advertiser_) advertiser_.reset();
  if (listener_) listener_->close();
  interrupt_channel();
}

void SecureReceiver::run() noexcept {
  while (!should_stop()) {
    std::string error;
    auto channel =
        listener_->accept(context_->identity(), kAcceptPoll, error);
    if (!channel) {
      if (!error.empty()) {
        publish(SessionState::connection_lost, error);
      }
      continue;
    }
    publish(SessionState::device_found,
            "Incoming connection from " + channel->peer_endpoint().to_string());
    ChannelGuard attached(*this, *channel);
    serve(*channel);
    teardown();
    if (!should_stop()) {
      publish(SessionState::scanning, "Waiting for a sender");
    }
  }
  publish(SessionState::disconnected, "Stopped");
  running_.store(false, std::memory_order_release);
}

void SecureReceiver::serve(TlsChannel& channel) noexcept {
  std::string error;
  PeerInfo peer;
  if (!authenticate(channel, options_.allow_pairing, peer, error)) return;
  if (should_stop()) return;

  NegotiatedSession session;
  const auto negotiated = negotiate_as_receiver(
      channel, local_capabilities(options_.preferred_sample_rate),
      options_.media_port, session, error);
  if (negotiated != HandshakeStatus::ok) {
    if (error.empty()) error = describe(negotiated);
    publish(SessionState::connection_lost, error);
    return;
  }
  session.peer = peer;

  auto protector = srtp::SrtpProtector::create(
      srtp::Direction::inbound, session.local_is_client,
      session.parameters.ssrc, session.key_material, error);
  if (!protector) {
    publish(SessionState::connection_lost, error);
    return;
  }

  StreamConfig config;
  config.format = to_format(session.parameters);
  config.port = session.parameters.media_port;
  config.ssrc = session.parameters.ssrc;
  config.payload_type = session.parameters.payload_type;
  config.session_id = session.parameters.session_id;
  config.jitter_packets = session.parameters.jitter_packets;
  config.require_protection = true;
  config.protector = protector.get();
  // Media is accepted only from the address that completed the handshake.
  config.expected_peer = channel.peer_endpoint();
  config.restrict_to_peer = true;

  auto stream = std::make_unique<NetworkReceiver>(config);
  if (!stream->start(error)) {
    publish(SessionState::connection_lost, error);
    return;
  }

  {
    const std::lock_guard<std::mutex> lock(media_mutex_);
    protector_ = std::move(protector);
    receiver_ = std::move(stream);
  }
  format_.store(config.format, std::memory_order_release);
  publish_parameters(session.parameters, srtp::kProfileName);
  publish(SessionState::secure_session_ready,
          "Secure session with " + peer.device_name);
  set_media_running(true);
  publish(SessionState::streaming,
          "Receiving from " + peer.device_name + " at " +
              std::to_string(session.parameters.sample_rate) + " Hz");
  set_media_running(true);

  auto last_seen = std::chrono::steady_clock::now();
  while (!should_stop()) {
    std::vector<std::byte> frame;
    bool timed_out = false;
    if (channel.read_frame(frame, kHeartbeatInterval, timed_out, error)) {
      protocol::ControlMessageType type{};
      if (protocol::frame_type(frame, type) != protocol::CodecError::none) {
        error = "the sender sent a malformed control frame";
        break;
      }
      if (type == protocol::ControlMessageType::goodbye) {
        error = "the sender ended the session";
        break;
      }
      if (type == protocol::ControlMessageType::heartbeat) {
        protocol::HeartbeatMessage beat;
        if (protocol::decode(frame, beat) != protocol::CodecError::none) {
          error = "the sender sent a malformed heartbeat";
          break;
        }
        std::vector<std::byte> response;
        if (!protocol::encode(beat, response) ||
            !channel.write_frame(response, error)) {
          break;
        }
      }
      last_seen = std::chrono::steady_clock::now();
    } else if (!timed_out) {
      break;
    }
    if (std::chrono::steady_clock::now() - last_seen > kHeartbeatTimeout) {
      error = "the sender stopped responding";
      break;
    }
    observe_media(*this, metrics(), std::chrono::steady_clock::now());
  }

  // Fail closed: expire before any further audio can be rendered.
  {
    const std::lock_guard<std::mutex> lock(media_mutex_);
    if (receiver_) receiver_->expire_session();
  }
  publish(SessionState::connection_lost,
          error.empty() ? "The secure session ended" : error);
}

void SecureReceiver::render_audio(std::span<float> interleaved) noexcept {
  std::unique_lock<std::mutex> lock(media_mutex_, std::try_to_lock);
  if (!lock.owns_lock() || !receiver_) {
    std::fill(interleaved.begin(), interleaved.end(), 0.0F);
    return;
  }
  receiver_->render_audio(interleaved);
}

const StreamMetrics* SecureReceiver::metrics() const noexcept {
  std::unique_lock<std::mutex> lock(media_mutex_, std::try_to_lock);
  if (!lock.owns_lock() || !receiver_) return nullptr;
  return &receiver_->metrics();
}

AudioFormat SecureReceiver::negotiated_format() const noexcept {
  return format_.load(std::memory_order_acquire);
}

}  // namespace lanaudio::control
