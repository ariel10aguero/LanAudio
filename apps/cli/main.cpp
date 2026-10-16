#include "rtaudio_backend.hpp"

#ifdef LANAUDIO_HAS_SECURITY
#include "lanaudio/control/discovery.hpp"
#include "lanaudio/control/secure_session.hpp"
#include "lanaudio/protocol/negotiation.hpp"
#include "lanaudio/security/identity.hpp"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#ifdef LANAUDIO_HAS_SECURITY
#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif
#endif

namespace {

std::atomic<bool> running{true};

void signal_handler(int) { running.store(false); }

std::string option(int argc, char** argv, std::string_view name,
                   std::string fallback = {}) {
  for (int i = 2; i + 1 < argc; ++i) {
    if (argv[i] == name) return argv[i + 1];
  }
  return fallback;
}

bool has_option(int argc, char** argv, std::string_view name) {
  for (int i = 2; i < argc; ++i) {
    if (argv[i] == name) return true;
  }
  return false;
}

// Rejects empty, non-numeric, out-of-range, and leftover-suffix values before
// any narrowing conversion. Defaults are used only when the flag is absent.
std::optional<std::uint64_t> parse_u64(std::string_view text,
                                       std::uint64_t maximum) {
  if (text.empty()) return std::nullopt;
  std::uint64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') return std::nullopt;
    const std::uint64_t next = value * 10 + static_cast<unsigned>(digit - '0');
    if (next / 10 != value) return std::nullopt;  // overflow
    if (next > maximum) return std::nullopt;
    value = next;
  }
  return value;
}

std::optional<std::uint32_t> parse_u32_option(int argc, char** argv,
                                              std::string_view name,
                                              std::uint32_t fallback,
                                              std::uint32_t maximum,
                                              std::string& error) {
  const auto text = option(argc, argv, name);
  if (text.empty()) return fallback;
  const auto value = parse_u64(text, maximum);
  if (!value) {
    error = std::string(name) + " must be an integer between 0 and " +
            std::to_string(maximum);
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(*value);
}

std::optional<std::uint16_t> parse_port(int argc, char** argv,
                                        std::string_view name,
                                        std::uint16_t fallback,
                                        std::string& error) {
  const auto value = parse_u32_option(argc, argv, name, fallback, 65'535, error);
  if (!value) return std::nullopt;
  if (*value == 0) {
    error = std::string(name) + " must be a UDP/TCP port from 1 to 65535";
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(*value);
}

void install_signals() {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
}

void print_metrics(const lanaudio::StreamMetrics& metrics) {
  std::cout << "sent=" << metrics.packets_sent.load()
            << " received=" << metrics.packets_received.load()
            << " malformed=" << metrics.malformed_packets.load()
            << " lost=" << metrics.lost_packets.load()
            << " late=" << metrics.late_packets.load()
            << " underruns=" << metrics.underruns.load()
            << " overruns=" << metrics.overruns.load()
            << " jitter_buffer=" << metrics.jitter_buffer_packets.load()
            << " drift_ppm=" << metrics.drift_ppm.load()
            << " decrypt_fail=" << metrics.decrypt_failures.load()
            << " replay=" << metrics.replay_rejections.load()
            << " foreign=" << metrics.foreign_source_rejections.load()
            << " resyncs=" << metrics.stream_resyncs.load()
            << " input_level=" << metrics.input_level_milli.load() / 1000.0
            << " output_level=" << metrics.output_level_milli.load() / 1000.0
            << '\n';
}

int devices() {
  std::string error;
  const auto devices = lanaudio::RtAudioBackend::devices(error);
  for (const auto& device : devices) {
    std::cout << device.id << "\t" << device.name
              << "\tinputs=" << device.input_channels
              << "\toutputs=" << device.output_channels
              << "\tcurrent-rate=" << device.current_sample_rate
              << "\tpreferred-rate=" << device.preferred_sample_rate
              << (device.default_input ? "\tdefault-input" : "")
              << (device.default_output ? "\tdefault-output" : "");
    if (!device.sample_rates.empty()) {
      std::cout << "\trates=";
      for (std::size_t i = 0; i < device.sample_rates.size(); ++i) {
        if (i != 0) std::cout << ',';
        std::cout << device.sample_rates[i];
      }
    }
    std::cout << '\n';
  }
  if (!error.empty()) {
    std::cerr << error << '\n';
    return devices.empty() ? 1 : 0;
  }
  return 0;
}

void usage() {
  std::cout
      << "LanAudio " LANAUDIO_VERSION "\n"
      << "  lanaudio devices\n"
#ifdef LANAUDIO_HAS_SECURITY
      << "  lanaudio discover [--timeout-ms 2500]\n"
      << "  lanaudio pair --host ADDRESS [--control-port 5005]\n"
      << "  lanaudio list-trusted\n"
      << "  lanaudio remove-trusted FINGERPRINT\n"
      << "  lanaudio reset-trust\n"
      << "  lanaudio send --host ADDRESS [--control-port 5005]\n"
      << "      [--input ID] [--input-channel 1] [--buffer-frames 128]\n"
      << "      [--preferred-rate 48000] [--jitter-packets 4]\n"
      << "  lanaudio receive [--control-port 5005] [--media-port 5004]\n"
      << "      [--output ID] [--output-channel 1] [--buffer-frames 128]\n"
      << "      [--preferred-rate 48000] [--stats-interval-ms 0]\n"
#else
      << "  lanaudio send / receive  (unavailable: this build has no security "
         "stack)\n"
#endif
      << "  lanaudio diagnostics [--export FILE]\n";
}

#ifdef LANAUDIO_HAS_SECURITY

std::string hostname() {
  char buffer[256] = {};
#ifdef _WIN32
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return "LanAudio device";
#endif
  const int result = gethostname(buffer, sizeof(buffer) - 1);
#ifdef _WIN32
  WSACleanup();
#endif
  if (result != 0 || buffer[0] == '\0') return "LanAudio device";
  return buffer;
}

std::shared_ptr<lanaudio::control::SecurityContext> load_context(
    std::string& error) {
  return lanaudio::control::SecurityContext::create(hostname(), LANAUDIO_VERSION,
                                                    error);
}

bool wait_until_ready(lanaudio::control::SecureEndpoint& endpoint,
                      std::chrono::milliseconds limit, std::string& error) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline && running.load()) {
    const auto snapshot = endpoint.snapshot();
    if (snapshot.state == lanaudio::control::SessionState::confirm_code &&
        endpoint.pairing_gate().waiting()) {
      std::cout << "Confirm matching code: " << snapshot.pending_code
                << "\nType yes to pair, or no to abort: " << std::flush;
      std::string answer;
      if (!std::getline(std::cin, answer)) {
        endpoint.pairing_gate().decide(false);
        error = "pairing confirmation was interrupted";
        return false;
      }
      const bool accept = answer == "yes" || answer == "y" || answer == "YES";
      endpoint.pairing_gate().decide(accept);
      continue;
    }
    if (lanaudio::control::allows_media(snapshot.state)) return true;
    if (snapshot.state == lanaudio::control::SessionState::connection_lost ||
        snapshot.state == lanaudio::control::SessionState::trust_revoked ||
        snapshot.state == lanaudio::control::SessionState::disconnected) {
      if (!endpoint.running()) {
        error = snapshot.message.empty() ? lanaudio::control::describe(snapshot.state)
                                         : snapshot.message;
        return false;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  error = "the secure session did not become ready in time";
  return false;
}

int discover(int argc, char** argv) {
  std::string error;
  const auto timeout_ms =
      parse_u32_option(argc, argv, "--timeout-ms", 2'500, 30'000, error);
  if (!timeout_ms) {
    std::cerr << error << '\n';
    return 2;
  }
  auto peers = lanaudio::control::discover_peers(
      std::chrono::milliseconds(*timeout_ms), error);
  if (peers.empty() && !error.empty()) {
    std::cerr << error << '\n';
    return 1;
  }
  auto context = load_context(error);
  for (const auto& peer : peers) {
    std::cout << peer.host << ':' << peer.control_port << '\t'
              << peer.device_name;
    if (peer.fingerprint) {
      std::cout << '\t' << lanaudio::security::to_hex(*peer.fingerprint);
      if (context && context->trust().contains(*peer.fingerprint)) {
        std::cout << "\ttrusted";
      }
    }
    if (!peer.well_formed) std::cout << "\tincomplete-advertisement";
    std::cout << '\n';
  }
  return 0;
}

int pair_command(int argc, char** argv) {
  const auto host = option(argc, argv, "--host");
  if (host.empty()) {
    std::cerr << "pair requires --host ADDRESS\n";
    return 2;
  }
  std::string error;
  const auto port =
      parse_port(argc, argv, "--control-port",
                 lanaudio::control::kDefaultControlPort, error);
  if (!port) {
    std::cerr << error << '\n';
    return 2;
  }
  auto context = load_context(error);
  if (!context) {
    std::cerr << error << '\n';
    return 1;
  }
  lanaudio::control::SecureSender::Options options;
  options.host = host;
  options.control_port = *port;
  options.allow_pairing = true;
  lanaudio::control::SecureSender sender(context, options);
  if (!sender.start(error)) {
    std::cerr << error << '\n';
    return 1;
  }
  install_signals();
  if (!wait_until_ready(sender, std::chrono::seconds(180), error)) {
    std::cerr << error << '\n';
    sender.stop();
    return 1;
  }
  const auto snapshot = sender.snapshot();
  std::cout << "paired with " << snapshot.peer_name << " "
            << snapshot.peer_fingerprint << '\n';
  sender.stop();
  return 0;
}

int list_trusted() {
  std::string error;
  auto context = load_context(error);
  if (!context) {
    std::cerr << error << '\n';
    return 1;
  }
  for (const auto& peer : context->trust().peers()) {
    std::cout << lanaudio::security::to_hex(peer.fingerprint) << '\t'
              << peer.name << '\t' << peer.paired_unix_seconds << '\n';
  }
  return 0;
}

int remove_trusted(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "remove-trusted requires a 64-character hex fingerprint\n";
    return 2;
  }
  lanaudio::security::Fingerprint fingerprint{};
  if (!lanaudio::security::parse_hex_fingerprint(argv[2], fingerprint)) {
    std::cerr << "fingerprint must be 64 hexadecimal characters\n";
    return 2;
  }
  std::string error;
  auto context = load_context(error);
  if (!context) {
    std::cerr << error << '\n';
    return 1;
  }
  if (!context->trust().remove(fingerprint, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "removed " << argv[2] << '\n';
  return 0;
}

int reset_trust() {
  std::string error;
  auto context = load_context(error);
  if (!context) {
    std::cerr << error << '\n';
    return 1;
  }
  if (!context->trust().reset(error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "all trusted devices were removed\n";
  return 0;
}

int send(int argc, char** argv) {
  const auto host = option(argc, argv, "--host");
  if (host.empty()) {
    std::cerr << "send requires --host ADDRESS\n";
    return 2;
  }
  std::string error;
  const auto control_port =
      parse_port(argc, argv, "--control-port",
                 lanaudio::control::kDefaultControlPort, error);
  const auto input =
      parse_u32_option(argc, argv, "--input", 0, 1'000'000, error);
  const auto input_channel =
      parse_u32_option(argc, argv, "--input-channel", 1, 64, error);
  const auto buffer_frames =
      parse_u32_option(argc, argv, "--buffer-frames", 128, 4'096, error);
  const auto preferred_rate =
      parse_u32_option(argc, argv, "--preferred-rate", 48'000, 192'000, error);
  const auto jitter_packets =
      parse_u32_option(argc, argv, "--jitter-packets", 4, 32, error);
  if (!control_port || !input || !input_channel || !buffer_frames ||
      !preferred_rate || !jitter_packets) {
    std::cerr << error << '\n';
    return 2;
  }
  if (*input_channel == 0) {
    std::cerr << "--input-channel is one-based and must be at least 1\n";
    return 2;
  }

  auto context = load_context(error);
  if (!context) {
    std::cerr << error << '\n';
    return 1;
  }
  lanaudio::control::SecureSender::Options options;
  options.host = host;
  options.control_port = *control_port;
  options.preferred_sample_rate = *preferred_rate;
  options.preferred_jitter_packets = static_cast<std::uint16_t>(*jitter_packets);
  lanaudio::control::SecureSender sender(context, options);
  if (!sender.start(error)) {
    std::cerr << error << '\n';
    return 1;
  }
  install_signals();
  if (!wait_until_ready(sender, std::chrono::seconds(180), error)) {
    std::cerr << error << '\n';
    sender.stop();
    return 1;
  }

  const auto format = sender.negotiated_format();
  lanaudio::RtAudioBackend audio;
  auto frames = *buffer_frames;
  if (!audio.start_input(
          [&sender](std::span<const float> samples) { sender.push_audio(samples); },
          *input, *input_channel - 1, format.sample_rate, format.channels,
          frames, error)) {
    std::cerr << error << '\n';
    sender.stop();
    return 1;
  }
  const auto snapshot = sender.snapshot();
  std::cout << "secure send to " << snapshot.peer_name << " at "
            << format.sample_rate << " Hz, " << snapshot.media_profile
            << "; Ctrl-C to stop\n";
  while (running.load() && sender.running()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  audio.stop();
  if (const auto* metrics = sender.metrics()) print_metrics(*metrics);
  sender.stop();
  return 0;
}

int receive(int argc, char** argv) {
  std::string error;
  const auto control_port =
      parse_port(argc, argv, "--control-port",
                 lanaudio::control::kDefaultControlPort, error);
  const auto media_port = parse_port(
      argc, argv, "--media-port", lanaudio::control::kDefaultMediaPort, error);
  const auto output =
      parse_u32_option(argc, argv, "--output", 0, 1'000'000, error);
  const auto output_channel =
      parse_u32_option(argc, argv, "--output-channel", 1, 64, error);
  const auto buffer_frames =
      parse_u32_option(argc, argv, "--buffer-frames", 128, 4'096, error);
  const auto preferred_rate =
      parse_u32_option(argc, argv, "--preferred-rate", 48'000, 192'000, error);
  const auto stats_interval_ms =
      parse_u32_option(argc, argv, "--stats-interval-ms", 0, 3'600'000, error);
  if (!control_port || !media_port || !output || !output_channel ||
      !buffer_frames || !preferred_rate || !stats_interval_ms) {
    std::cerr << error << '\n';
    return 2;
  }
  if (*output_channel == 0) {
    std::cerr << "--output-channel is one-based and must be at least 1\n";
    return 2;
  }

  auto context = load_context(error);
  if (!context) {
    std::cerr << error << '\n';
    return 1;
  }
  lanaudio::control::SecureReceiver::Options options;
  options.control_port = *control_port;
  options.media_port = *media_port;
  options.preferred_sample_rate = *preferred_rate;
  lanaudio::control::SecureReceiver receiver(context, options);
  if (!receiver.start(error)) {
    std::cerr << error << '\n';
    return 1;
  }

  std::unique_ptr<lanaudio::RtAudioBackend> audio;
  bool opened = false;
  std::uint32_t frames = *buffer_frames;
  auto last_stats = std::chrono::steady_clock::now();
  install_signals();
  std::cout << "secure receive on control port " << receiver.control_port()
            << "; waiting for a paired sender. Ctrl-C to stop\n";
  while (running.load() && receiver.running()) {
    const auto snapshot = receiver.snapshot();
    if (snapshot.state == lanaudio::control::SessionState::confirm_code &&
        receiver.pairing_gate().waiting()) {
      std::cout << "Confirm matching code: " << snapshot.pending_code
                << "\nType yes to pair, or no to abort: " << std::flush;
      std::string answer;
      if (!std::getline(std::cin, answer)) {
        receiver.pairing_gate().decide(false);
        break;
      }
      receiver.pairing_gate().decide(answer == "yes" || answer == "y" ||
                                     answer == "YES");
    }
    if (lanaudio::control::allows_media(snapshot.state) && !opened) {
      const auto format = receiver.negotiated_format();
      audio = std::make_unique<lanaudio::RtAudioBackend>();
      if (!audio->start_output(
              [&receiver](std::span<float> samples) {
                receiver.render_audio(samples);
              },
              *output, *output_channel - 1, format.sample_rate, format.channels,
              frames, error)) {
        std::cerr << error << '\n';
        receiver.stop();
        return 1;
      }
      opened = true;
      std::cout << "streaming from " << snapshot.peer_name << " at "
                << format.sample_rate << " Hz, " << snapshot.media_profile
                << '\n';
    }
    if (opened && !lanaudio::control::allows_media(snapshot.state)) {
      audio->stop();
      audio.reset();
      opened = false;
      std::cout << snapshot.message << '\n';
    }
    if (opened && *stats_interval_ms != 0) {
      const auto now = std::chrono::steady_clock::now();
      if (now - last_stats >= std::chrono::milliseconds(*stats_interval_ms)) {
        last_stats = now;
        if (const auto* metrics = receiver.metrics()) print_metrics(*metrics);
        std::cout << std::flush;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (audio) audio->stop();
  if (const auto* metrics = receiver.metrics()) print_metrics(*metrics);
  receiver.stop();
  return 0;
}

#endif  // LANAUDIO_HAS_SECURITY

int diagnostics(int argc, char** argv) {
#ifdef LANAUDIO_HAS_RTAUDIO
  constexpr const char* backend = "RtAudio";
#else
  constexpr const char* backend = "unavailable";
#endif
#ifdef LANAUDIO_HAS_SECURITY
  constexpr const char* media = "SRTP AEAD_AES_128_GCM over RTP/UDP L16";
  constexpr const char* encryption = "TLS 1.3 control + libsrtp2 AEAD_AES_128_GCM";
  constexpr bool security = true;
#else
  constexpr const char* media = "unavailable";
  constexpr const char* encryption = "disabled";
  constexpr bool security = false;
#endif
  const std::string report =
      std::string("{\n") + "  \"version\": \"" LANAUDIO_VERSION "\",\n" +
      "  \"audio_backend\": \"" + backend + "\",\n" +
      "  \"media\": \"" + media + "\",\n" +
      "  \"encryption\": \"" + encryption + "\",\n" +
      "  \"security_enabled\": " + (security ? "true" : "false") + ",\n" +
      "  \"contains_secrets\": false\n}\n";
  const auto export_path = option(argc, argv, "--export");
  if (!export_path.empty()) {
    std::ofstream output(export_path, std::ios::binary);
    if (!output) {
      std::cerr << "unable to open diagnostic export path\n";
      return 1;
    }
    output << report;
    if (!output) {
      std::cerr << "unable to write diagnostic export\n";
      return 1;
    }
    std::cout << "diagnostics exported to " << export_path << '\n';
    return 0;
  }
  std::cout << "audio_backend=" << backend << "\nmedia=" << media
            << "\nencryption=" << encryption << "\n";
  return 0;
}

int require_security(const char* command) {
  std::cerr << command
            << " is unavailable because this build has no authenticated "
               "control channel. Rebuild with LANAUDIO_ENABLE_SECURITY=ON.\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string_view command(argv[1]);
  if (command == "--help" || command == "-h" ||
      has_option(argc, argv, "--help")) {
    usage();
    return 0;
  }
  if (command == "devices") return devices();
  if (command == "diagnostics") return diagnostics(argc, argv);
#ifdef LANAUDIO_HAS_SECURITY
  if (command == "discover") return discover(argc, argv);
  if (command == "pair") return pair_command(argc, argv);
  if (command == "list-trusted") return list_trusted();
  if (command == "remove-trusted") return remove_trusted(argc, argv);
  if (command == "reset-trust") return reset_trust();
  if (command == "send") return send(argc, argv);
  if (command == "receive") return receive(argc, argv);
#else
  if (command == "discover" || command == "pair" ||
      command == "list-trusted" || command == "remove-trusted" ||
      command == "reset-trust" || command == "send" || command == "receive") {
    return require_security(argv[1]);
  }
#endif
  usage();
  return 2;
}
