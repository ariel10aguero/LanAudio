#include "rtaudio_backend.hpp"

#ifdef LANAUDIO_HAS_SECURITY
#include "lanaudio/control/discovery.hpp"
#include "lanaudio/control/secure_session.hpp"
#include "lanaudio/protocol/negotiation.hpp"
#include "lanaudio/security/identity.hpp"
#endif

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {

void configure_wrapping_label(QLabel* label) {
  label->setWordWrap(true);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
  label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
}

void configure_value_label(QLabel* label) {
  label->setWordWrap(true);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
}

void configure_combo(QComboBox* combo) {
  combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  combo->setMinimumContentsLength(16);
}

QGridLayout* make_form(QWidget* box) {
  box->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  auto* grid = new QGridLayout(box);
  grid->setColumnStretch(1, 1);
  grid->setColumnMinimumWidth(0, 108);
  grid->setHorizontalSpacing(12);
  grid->setVerticalSpacing(8);
  grid->setContentsMargins(12, 14, 12, 12);
  return grid;
}

int add_form_row(QGridLayout* grid, int row, const QString& name, QWidget* field,
                 Qt::Alignment label_align = Qt::AlignRight | Qt::AlignVCenter) {
  auto* label = new QLabel(name);
  label->setAlignment(label_align);
  grid->addWidget(label, row, 0, label_align);
  grid->addWidget(field, row, 1);
  return row + 1;
}

int add_form_span(QGridLayout* grid, int row, QWidget* field) {
  grid->addWidget(field, row, 0, 1, 2);
  return row + 1;
}

QString device_hostname() {
#ifdef Q_OS_UNIX
  char buffer[256] = {};
  if (gethostname(buffer, sizeof(buffer) - 1) == 0 && buffer[0] != '\0') {
    return QString::fromLocal8Bit(buffer);
  }
#endif
  return QStringLiteral("LanAudio device");
}

class MainWindow final : public QMainWindow {
 public:
  void set_mode_index(int index) { mode_.setCurrentIndex(index); }

  MainWindow() {
    setWindowTitle("LanAudio");
    resize(700, 720);
    setMinimumSize(620, 520);
    auto* root = new QWidget;
    auto* root_layout = new QVBoxLayout(root);
    root_layout->setContentsMargins(10, 10, 10, 10);
    root_layout->setSpacing(8);

    auto* session_box = new QGroupBox("Session");
    session_box->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto* session = new QVBoxLayout(session_box);
    session->setSpacing(8);
    session->setContentsMargins(12, 14, 12, 12);

    mode_.addItems({"Send", "Receive"});
    configure_combo(&mode_);
    mode_.setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    mode_.setMinimumWidth(120);
    connect_button_.setText("Send audio");
    connect_button_.setEnabled(false);
    connect_button_.setMinimumWidth(140);
    auto* top_row = new QHBoxLayout;
    top_row->setContentsMargins(0, 0, 0, 0);
    top_row->addWidget(new QLabel("Mode"));
    top_row->addWidget(&mode_);
    top_row->addSpacing(12);
    top_row->addWidget(&connect_button_, 1);
    session->addLayout(top_row);

    configure_wrapping_label(&status_);
    status_.setText("Disconnected");
    session->addWidget(&status_);

    QFont code_font = pairing_code_.font();
    code_font.setPointSize(16);
    code_font.setBold(true);
    pairing_code_.setFont(code_font);
    pairing_code_.setText("Pairing code: —");
    pairing_code_.setTextInteractionFlags(Qt::TextSelectableByMouse);
    pairing_code_.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    confirm_button_.setText("Codes match");
    decline_button_.setText("Codes differ");
    confirm_button_.setEnabled(false);
    decline_button_.setEnabled(false);
    auto* pair_row = new QHBoxLayout;
    pair_row->setContentsMargins(0, 0, 0, 0);
    pair_row->addWidget(&pairing_code_, 1);
    pair_row->addWidget(&confirm_button_);
    pair_row->addWidget(&decline_button_);
    session->addLayout(pair_row);

    input_level_.setRange(0, 1'000);
    input_level_.setTextVisible(false);
    output_level_.setRange(0, 1'000);
    output_level_.setTextVisible(false);
    auto* meters = new QHBoxLayout;
    meters->setContentsMargins(0, 0, 0, 0);
    meters->addWidget(new QLabel("In"));
    meters->addWidget(&input_level_, 1);
    meters->addSpacing(8);
    meters->addWidget(new QLabel("Out"));
    meters->addWidget(&output_level_, 1);
    session->addLayout(meters);

    configure_wrapping_label(&metrics_);
    metrics_.setText("Packets: 0   Loss: 0   Late: 0   Underruns: 0");
    session->addWidget(&metrics_);
    configure_wrapping_label(&security_);
    session->addWidget(&security_);
    root_layout->addWidget(session_box);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 6, 0);
    layout->setSpacing(10);

    auto* identity_box = new QGroupBox("This device");
    auto* identity_layout = make_form(identity_box);
    int identity_row = 0;
    configure_value_label(&device_name_);
    configure_value_label(&fingerprint_);
    configure_value_label(&trust_backend_);
    identity_row = add_form_row(identity_layout, identity_row, "Name",
                                &device_name_);
    identity_row = add_form_row(identity_layout, identity_row, "Fingerprint",
                                &fingerprint_);
    identity_row = add_form_row(identity_layout, identity_row, "Trust store",
                                &trust_backend_);
    refresh_devices_button_.setText("Refresh audio devices");
    auto* refresh_row = new QWidget;
    auto* refresh_layout = new QHBoxLayout(refresh_row);
    refresh_layout->setContentsMargins(0, 0, 0, 0);
    refresh_layout->addWidget(&refresh_devices_button_);
    refresh_layout->addStretch(1);
    (void)add_form_span(identity_layout, identity_row, refresh_row);
    layout->addWidget(identity_box);

    input_box_ = new QGroupBox("Input");
    auto* input_layout = make_form(input_box_);
    int input_row = 0;
    configure_combo(&input_devices_);
    configure_combo(&input_channels_);
    input_row = add_form_row(input_layout, input_row, "Audio interface",
                             &input_devices_);
    input_row = add_form_row(input_layout, input_row, "Interface inputs",
                             &input_channels_);
    configure_wrapping_label(&input_details_);
    input_row = add_form_row(input_layout, input_row, "Capabilities",
                             &input_details_, Qt::AlignRight | Qt::AlignTop);
    input_note_.setText(
        "Sends microphone or line input. macOS system audio capture is not "
        "available.");
    configure_wrapping_label(&input_note_);
    (void)add_form_span(input_layout, input_row, &input_note_);
    layout->addWidget(input_box_);

    connection_box_ = new QGroupBox("Connection");
    auto* connection_layout = make_form(connection_box_);
    int connection_row = 0;
    scan_button_.setText("Scan LAN");
    pair_button_.setText("Connect to selected");
    peers_.setMaximumHeight(72);
    peers_.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    sender_discovery_ = new QWidget;
    auto* discovery_layout = new QVBoxLayout(sender_discovery_);
    discovery_layout->setContentsMargins(0, 0, 0, 0);
    discovery_layout->setSpacing(8);
    auto* peers_row = new QWidget;
    auto* peers_grid = new QGridLayout(peers_row);
    peers_grid->setContentsMargins(0, 0, 0, 0);
    peers_grid->setHorizontalSpacing(12);
    auto* peers_label = new QLabel("Discovered devices");
    peers_label->setAlignment(Qt::AlignRight | Qt::AlignTop);
    peers_grid->addWidget(peers_label, 0, 0, Qt::AlignRight | Qt::AlignTop);
    peers_grid->addWidget(&peers_, 0, 1);
    peers_grid->setColumnStretch(1, 1);
    discovery_layout->addWidget(peers_row);
    auto* scan_row = new QWidget;
    auto* scan_layout = new QHBoxLayout(scan_row);
    scan_layout->setContentsMargins(0, 0, 0, 0);
    scan_layout->addWidget(&scan_button_);
    scan_layout->addWidget(&pair_button_);
    scan_layout->addStretch(1);
    discovery_layout->addWidget(scan_row);
    connection_row =
        add_form_span(connection_layout, connection_row, sender_discovery_);
    host_.setPlaceholderText("Receiver IP or hostname");
    control_port_.setRange(1, 65535);
    control_port_.setValue(5005);
    media_port_.setRange(1, 65535);
    media_port_.setValue(5004);
    configure_combo(&profile_);
    configure_combo(&preferred_rate_);
    configure_combo(&device_buffer_);
    profile_.addItems({"Ultra-low (2 packets)", "Balanced (4 packets)",
                       "Reliable (8 packets)"});
    profile_.setCurrentIndex(1);
    preferred_rate_.addItem("Automatic (prefer 48 kHz)", 48'000);
    preferred_rate_.addItem("44.1 kHz", 44'100);
    preferred_rate_.addItem("48 kHz", 48'000);
    preferred_rate_.addItem("88.2 kHz", 88'200);
    preferred_rate_.addItem("96 kHz", 96'000);
    preferred_rate_.addItem("176.4 kHz", 176'400);
    preferred_rate_.addItem("192 kHz", 192'000);
    device_buffer_.addItem("128 frames (stable default)", 128);
    device_buffer_.addItem("64 frames (lower latency)", 64);
    device_buffer_.addItem("256 frames (more robust)", 256);
    host_label_ = new QLabel("Receiver address");
    host_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connection_layout->addWidget(host_label_, connection_row, 0,
                                 Qt::AlignRight | Qt::AlignVCenter);
    connection_layout->addWidget(&host_, connection_row, 1);
    ++connection_row;
    connection_row = add_form_row(connection_layout, connection_row,
                                  "Control port", &control_port_);
    connection_row = add_form_row(connection_layout, connection_row,
                                  "Media port", &media_port_);
    connection_row = add_form_row(connection_layout, connection_row,
                                  "Sample rate", &preferred_rate_);
    connection_row = add_form_row(connection_layout, connection_row,
                                  "Latency profile", &profile_);
    connection_row = add_form_row(connection_layout, connection_row,
                                  "Device buffer", &device_buffer_);
    format_.setText("Format is negotiated after pairing");
    configure_wrapping_label(&format_);
    connection_row = add_form_row(connection_layout, connection_row,
                                  "Stream format", &format_,
                                  Qt::AlignRight | Qt::AlignTop);
    local_address_.setText("This Mac: " + local_ip_address());
    configure_wrapping_label(&local_address_);
    (void)add_form_span(connection_layout, connection_row, &local_address_);

    auto* trust_box = new QGroupBox("Trusted devices");
    trust_box->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto* trust_layout = new QVBoxLayout(trust_box);
    trust_layout->setContentsMargins(12, 14, 12, 12);
    rename_trust_button_.setText("Rename");
    remove_trust_button_.setText("Remove");
    reset_trust_button_.setText("Reset all");
    trusted_.setMaximumHeight(88);
    trust_layout->addWidget(&trusted_);
    auto* trust_buttons = new QWidget;
    auto* trust_button_layout = new QHBoxLayout(trust_buttons);
    trust_button_layout->setContentsMargins(0, 0, 0, 0);
    trust_button_layout->addWidget(&rename_trust_button_);
    trust_button_layout->addWidget(&remove_trust_button_);
    trust_button_layout->addWidget(&reset_trust_button_);
    trust_button_layout->addStretch(1);
    trust_layout->addWidget(trust_buttons);

    output_box_ = new QGroupBox("Output");
    auto* output_layout = make_form(output_box_);
    int output_row = 0;
    configure_combo(&output_devices_);
    configure_combo(&output_channels_);
    output_row = add_form_row(output_layout, output_row, "Audio interface",
                              &output_devices_);
    output_row = add_form_row(output_layout, output_row, "Interface outputs",
                              &output_channels_);
    configure_wrapping_label(&output_details_);
    output_row = add_form_row(output_layout, output_row, "Capabilities",
                              &output_details_, Qt::AlignRight | Qt::AlignTop);
    mute_.setText("Mute playback");
    (void)add_form_span(output_layout, output_row, &mute_);
    layout->addWidget(output_box_);
    layout->addWidget(connection_box_);
    layout->addWidget(trust_box);
    layout->addStretch();
    scroll->setWidget(page);
    root_layout->addWidget(scroll, 1);
    setCentralWidget(root);

#ifdef LANAUDIO_HAS_SECURITY
    std::string error;
    context_ = lanaudio::control::SecurityContext::create(
        device_hostname().toStdString(), LANAUDIO_VERSION, error);
    if (!context_) {
      security_.setText("Security: " + QString::fromStdString(error));
    } else {
      fingerprint_.setText(QString::fromStdString(
          lanaudio::security::short_fingerprint(
              context_->identity().fingerprint())));
      device_name_.setText(
          QString::fromStdString(context_->local().device_name));
      trust_backend_.setText(
          QString::fromStdString(context_->backend_name()) +
          (context_->storage_is_secure()
               ? QString(" (operating-system protected)")
               : QString(" (in-memory development store)")));
      security_.setText("Encrypted with TLS 1.3 and SRTP.");
      refresh_trust();
    }
#else
    security_.setText(
        "Security: this build has no authenticated control channel. Audio "
        "cannot start.");
    connect_button_.setEnabled(false);
    scan_button_.setEnabled(false);
    pair_button_.setEnabled(false);
#endif

    refresh_devices();
    connect(&connect_button_, &QPushButton::clicked, this, [this] {
      if (session_active_)
        stop();
      else
        start();
    });
    connect(&mode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { update_mode(); });
    connect(&input_devices_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { update_input_device(); });
    connect(&output_devices_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { update_output_device(); });
    connect(&refresh_devices_button_, &QPushButton::clicked, this,
            [this] { refresh_devices(); });
    connect(&scan_button_, &QPushButton::clicked, this, [this] { scan(); });
    connect(&pair_button_, &QPushButton::clicked, this, [this] { pair_selected(); });
    connect(&confirm_button_, &QPushButton::clicked, this,
            [this] { decide_pairing(true); });
    connect(&decline_button_, &QPushButton::clicked, this,
            [this] { decide_pairing(false); });
    connect(&rename_trust_button_, &QPushButton::clicked, this,
            [this] { rename_trusted(); });
    connect(&remove_trust_button_, &QPushButton::clicked, this,
            [this] { remove_trusted(); });
    connect(&reset_trust_button_, &QPushButton::clicked, this,
            [this] { reset_trust(); });
    connect(&peers_, &QListWidget::itemSelectionChanged, this, [this] {
      if (peers_.currentItem() != nullptr) {
        host_.setText(peers_.currentItem()->data(Qt::UserRole).toString());
        control_port_.setValue(
            peers_.currentItem()->data(Qt::UserRole + 1).toInt());
      }
    });
    connect(&mute_, &QCheckBox::toggled, this, [this](bool muted) {
      muted_ = muted;
    });
    connect(&timer_, &QTimer::timeout, this, [this] { poll_session(); });
    timer_.start(250);
    update_mode();
  }

  ~MainWindow() override { stop(); }

 private:
  static QString local_ip_address() {
    for (const auto& address : QNetworkInterface::allAddresses()) {
      if (address.protocol() == QAbstractSocket::IPv4Protocol &&
          !address.isLoopback()) {
        return address.toString();
      }
    }
    return QStringLiteral(
        "unavailable — check System Settings > Network for this Mac's IP");
  }

  const lanaudio::AudioDeviceInfo* selected_device(
      const QComboBox& selector) const {
    if (selector.currentIndex() < 0) return nullptr;
    const auto id = selector.currentData().toUInt();
    for (const auto& device : devices_) {
      if (device.id == id) return &device;
    }
    return nullptr;
  }

  static QString device_details(const lanaudio::AudioDeviceInfo& device,
                                bool input) {
    QStringList rates;
    for (const auto rate : device.sample_rates) {
      rates.append(QString::number(rate / 1'000.0, 'f', 1) + " kHz");
    }
    const auto channel_count =
        input ? device.input_channels : device.output_channels;
    return QString("%1 ch, %2 Hz%3")
        .arg(channel_count)
        .arg(device.current_sample_rate)
        .arg(rates.isEmpty() ? QString()
                             : QString("  (") + rates.join(", ") + ")");
  }

  void refresh_devices() {
    const auto old_input = input_devices_.currentData();
    const auto old_output = output_devices_.currentData();
    input_devices_.clear();
    output_devices_.clear();
    devices_.clear();
    std::string error;
    devices_ = lanaudio::RtAudioBackend::devices(error);
    int default_input = -1;
    int default_output = -1;
    for (const auto& device : devices_) {
      if (device.input_channels >= 1) {
        input_devices_.addItem(QString::fromStdString(device.name), device.id);
        if (device.default_input) default_input = input_devices_.count() - 1;
      }
      if (device.output_channels >= 2) {
        output_devices_.addItem(QString::fromStdString(device.name), device.id);
        if (device.default_output) default_output = output_devices_.count() - 1;
      }
    }
    const auto previous_input = input_devices_.findData(old_input);
    const auto previous_output = output_devices_.findData(old_output);
    input_devices_.setCurrentIndex(
        previous_input >= 0 ? previous_input : default_input);
    output_devices_.setCurrentIndex(
        previous_output >= 0 ? previous_output : default_output);
    if (input_devices_.currentIndex() < 0 && input_devices_.count() > 0) {
      input_devices_.setCurrentIndex(0);
    }
    if (output_devices_.currentIndex() < 0 && output_devices_.count() > 0) {
      output_devices_.setCurrentIndex(0);
    }
    update_input_device();
    update_output_device();
    if (!error.empty()) status_.setText(QString::fromStdString(error));
  }

  void update_input_device() {
    input_channels_.clear();
    const auto* device = selected_device(input_devices_);
    if (!device) {
      input_details_.setText("No input device available");
      return;
    }
    for (std::uint32_t first = 0; first < device->input_channels; first += 2) {
      if (first + 1 < device->input_channels) {
        input_channels_.addItem(
            QString("Inputs %1–%2").arg(first + 1).arg(first + 2), first);
      } else {
        input_channels_.addItem(
            QString("Input %1 (mono duplicated to stereo)").arg(first + 1),
            first);
      }
    }
    input_details_.setText(device_details(*device, true));
  }

  void update_output_device() {
    output_channels_.clear();
    const auto* device = selected_device(output_devices_);
    if (!device) {
      output_details_.setText("No stereo output device available");
      return;
    }
    for (std::uint32_t first = 0; first + 1 < device->output_channels;
         first += 2) {
      output_channels_.addItem(
          QString("Outputs %1–%2").arg(first + 1).arg(first + 2), first);
    }
    output_details_.setText(device_details(*device, false));
  }

  void update_mode() {
    const bool sending = mode_.currentIndex() == 0;
    if (input_box_) input_box_->setVisible(sending);
    if (output_box_) output_box_->setVisible(!sending);
    if (sender_discovery_) sender_discovery_->setVisible(sending);
    if (host_label_) host_label_->setVisible(sending);
    host_.setVisible(sending);
    host_.setEnabled(sending && !session_active_);
    control_port_.setEnabled(!session_active_);
    media_port_.setEnabled(!sending && !session_active_);
    input_devices_.setEnabled(sending && !session_active_);
    input_channels_.setEnabled(sending && !session_active_);
    output_devices_.setEnabled(!sending && !session_active_);
    output_channels_.setEnabled(!sending && !session_active_);
    refresh_devices_button_.setEnabled(!session_active_);
    mode_.setEnabled(!session_active_);
    profile_.setEnabled(!session_active_);
    preferred_rate_.setEnabled(!session_active_);
    device_buffer_.setEnabled(!session_active_);
    mute_.setEnabled(!sending);
    scan_button_.setEnabled(sending && !session_active_);
    pair_button_.setEnabled(sending && !session_active_);
    peers_.setEnabled(sending && !session_active_);
    if (!session_active_) {
      connect_button_.setText(sending ? "Send audio" : "Start receiver");
#ifndef LANAUDIO_HAS_SECURITY
      connect_button_.setEnabled(false);
#else
      connect_button_.setEnabled(context_ != nullptr);
#endif
      status_.setText(
          sending ? "Scan the LAN, select the receiver, then click Send audio. "
                    "If a code appears, click Codes match on both Macs."
                  : "Click Start receiver and leave this window open. When the "
                    "sender connects, confirm the matching code — this Mac "
                    "then plays the incoming audio.");
    }
  }

#ifdef LANAUDIO_HAS_SECURITY
  lanaudio::control::SecureEndpoint* endpoint() {
    if (sender_) return sender_.get();
    if (receiver_) return receiver_.get();
    return nullptr;
  }

  void refresh_trust() {
    trusted_.clear();
    if (!context_) return;
    for (const auto& peer : context_->trust().peers()) {
      auto* item = new QListWidgetItem(
          QString::fromStdString(peer.name) + "  " +
          QString::fromStdString(
              lanaudio::security::short_fingerprint(peer.fingerprint)));
      item->setData(Qt::UserRole,
                    QString::fromStdString(lanaudio::security::to_hex(
                        peer.fingerprint)));
      trusted_.addItem(item);
    }
  }

  void scan() {
    status_.setText("Scanning");
    std::string error;
    const auto found = lanaudio::control::discover_peers(
        std::chrono::milliseconds(2'000), error);
    peers_.clear();
    for (const auto& peer : found) {
      if (!peer.well_formed) continue;
      const auto trusted =
          peer.fingerprint && context_ &&
          context_->trust().contains(*peer.fingerprint);
      auto* item = new QListWidgetItem(
          QString::fromStdString(peer.device_name) + " @ " +
          QString::fromStdString(peer.host) +
          (trusted ? QString(" (trusted)") : QString(" (pairing required)")));
      item->setData(Qt::UserRole, QString::fromStdString(peer.host));
      item->setData(Qt::UserRole + 1, peer.control_port);
      peers_.addItem(item);
    }
    if (peers_.count() == 0) {
      status_.setText(error.empty() ? "No LanAudio receivers were found"
                                    : QString::fromStdString(error));
    } else {
      status_.setText("Device found");
      peers_.setCurrentRow(0);
    }
  }

  void pair_selected() {
    if (peers_.currentItem() != nullptr) {
      host_.setText(peers_.currentItem()->data(Qt::UserRole).toString());
      control_port_.setValue(
          peers_.currentItem()->data(Qt::UserRole + 1).toInt());
    }
    start();
  }

  void decide_pairing(bool accept) {
    pairing_confirmed_locally_ = true;
    if (auto* current = endpoint()) current->pairing_gate().decide(accept);
    confirm_button_.setEnabled(false);
    decline_button_.setEnabled(false);
    status_.setText(accept ? "Code confirmed on this Mac. Waiting for the "
                             "other Mac to click Codes match..."
                           : "Pairing declined on this Mac");
  }

  bool selected_trusted_fingerprint(lanaudio::security::Fingerprint& fingerprint) {
    const auto* item = trusted_.currentItem();
    if (item == nullptr) return false;
    return lanaudio::security::parse_hex_fingerprint(
        item->data(Qt::UserRole).toString().toStdString(), fingerprint);
  }

  void rename_trusted() {
    if (!context_) return;
    lanaudio::security::Fingerprint fingerprint{};
    if (!selected_trusted_fingerprint(fingerprint)) return;
    bool ok = false;
    const auto name = QInputDialog::getText(this, "Rename device",
                                            "Display name", QLineEdit::Normal,
                                            trusted_.currentItem()->text(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    std::string error;
    if (!context_->trust().rename(fingerprint, name.trimmed().toStdString(),
                                  error)) {
      fail(error);
      return;
    }
    refresh_trust();
  }

  void remove_trusted() {
    if (!context_) return;
    lanaudio::security::Fingerprint fingerprint{};
    if (!selected_trusted_fingerprint(fingerprint)) return;
    std::string error;
    if (!context_->trust().remove(fingerprint, error)) {
      fail(error);
      return;
    }
    refresh_trust();
    status_.setText("Trust revoked");
  }

  void reset_trust() {
    if (!context_) return;
    if (QMessageBox::question(this, "LanAudio",
                              "Remove every trusted device?") !=
        QMessageBox::Yes) {
      return;
    }
    std::string error;
    if (!context_->trust().reset(error)) {
      fail(error);
      return;
    }
    refresh_trust();
    status_.setText("Trust revoked");
  }

  static int peak_milli(std::span<const float> samples) {
    float peak = 0.0F;
    for (const float sample : samples) {
      peak = std::max(peak, std::abs(sample));
    }
    return static_cast<int>(std::clamp(peak, 0.0F, 1.0F) * 1'000.0F);
  }

  bool start_sender_capture(std::uint32_t sample_rate, std::string& error) {
    if (audio_open_ && audio_sample_rate_ == sample_rate) return true;
    close_audio();
    audio_ = std::make_unique<lanaudio::RtAudioBackend>();
    std::uint32_t frames = device_buffer_.currentData().toUInt();
    if (!audio_->start_input(
            [this](std::span<const float> samples) {
              preview_input_level_.store(peak_milli(samples),
                                         std::memory_order_relaxed);
              if (media_live_.load(std::memory_order_acquire) && sender_) {
                sender_->push_audio(samples);
              }
            },
            input_devices_.currentData().toUInt(),
            input_channels_.currentData().toUInt(), sample_rate, 2, frames,
            error)) {
      audio_.reset();
      return false;
    }
    audio_open_ = true;
    audio_sample_rate_ = sample_rate;
    return true;
  }

  bool start_receiver_playback(std::uint32_t sample_rate, std::string& error) {
    if (audio_open_ && audio_sample_rate_ == sample_rate) return true;
    close_audio();
    audio_ = std::make_unique<lanaudio::RtAudioBackend>();
    std::uint32_t frames = device_buffer_.currentData().toUInt();
    if (!audio_->start_output(
            [this](std::span<float> samples) {
              if (receiver_) receiver_->render_audio(samples);
              if (muted_) std::fill(samples.begin(), samples.end(), 0.0F);
              preview_output_level_.store(peak_milli(samples),
                                          std::memory_order_relaxed);
            },
            output_devices_.currentData().toUInt(),
            output_channels_.currentData().toUInt(), sample_rate, 2, frames,
            error)) {
      audio_.reset();
      return false;
    }
    audio_open_ = true;
    audio_sample_rate_ = sample_rate;
    return true;
  }

  bool open_audio_after_negotiation() {
    const auto format =
        sender_ ? sender_->negotiated_format() : receiver_->negotiated_format();
    if (!format.valid()) return false;
    std::string error;
    const bool opened =
        sender_ ? start_sender_capture(format.sample_rate, error)
                : start_receiver_playback(format.sample_rate, error);
    if (!opened) {
      if (!audio_error_shown_) {
        audio_error_shown_ = true;
        QMessageBox::warning(
            this, "LanAudio",
            error.empty() ? QString("Unable to open the audio device.")
                          : QString::fromStdString(error));
      }
      return false;
    }
    media_live_.store(true, std::memory_order_release);
    format_.setText(QString("%1 kHz stereo PCM • %2 ms packets • %3")
                        .arg(format.sample_rate / 1'000.0, 0, 'f', 1)
                        .arg(format.frames_per_packet * 1'000.0 /
                                 format.sample_rate,
                             0, 'f', 1)
                        .arg(QString::fromStdString(
                            sender_ ? sender_->snapshot().media_profile
                                    : receiver_->snapshot().media_profile)));
    return true;
  }

  void close_audio() {
    media_live_.store(false, std::memory_order_release);
    if (audio_) audio_->stop();
    audio_.reset();
    audio_open_ = false;
    audio_sample_rate_ = 0;
  }

  void poll_session() {
    auto* current = endpoint();
    if (current == nullptr) {
      update_metrics(nullptr);
      return;
    }
    const auto snapshot = current->snapshot();
    status_.setText(QString::fromStdString(
                        lanaudio::control::describe(snapshot.state)) +
                    (snapshot.message.empty()
                         ? QString()
                         : QString(" — ") +
                               QString::fromStdString(snapshot.message)));
    const bool confirming =
        snapshot.state == lanaudio::control::SessionState::confirm_code;
    confirm_button_.setEnabled(confirming && !pairing_confirmed_locally_);
    decline_button_.setEnabled(confirming && !pairing_confirmed_locally_);
    if (confirming && pairing_confirmed_locally_) {
      status_.setText(
          "Code confirmed on this Mac. Waiting for the other Mac to "
          "click Codes match...");
    }
    pairing_code_.setText(
        snapshot.pending_code.empty()
            ? QString("Pairing code: —")
            : QString("Pairing code: %1")
                  .arg(QString::fromStdString(snapshot.pending_code)));
    if (lanaudio::control::allows_media(snapshot.state)) {
      (void)open_audio_after_negotiation();
      if (!trust_refreshed_for_session_) {
        refresh_trust();
        trust_refreshed_for_session_ = true;
      }
    } else if (audio_open_ &&
               (snapshot.state ==
                    lanaudio::control::SessionState::connection_lost ||
                snapshot.state == lanaudio::control::SessionState::disconnected ||
                snapshot.state == lanaudio::control::SessionState::trust_revoked)) {
      close_audio();
    }
    update_metrics(sender_ ? sender_->metrics()
                           : (receiver_ ? receiver_->metrics() : nullptr));
    if (snapshot.media_stalled) {
      metrics_.setText(metrics_.text() + "  Media stalled");
    }
  }
#endif

  void start() {
#ifndef LANAUDIO_HAS_SECURITY
    fail("This build cannot start audio without the security stack.");
    return;
#else
    if (!context_) {
      fail("The device identity could not be loaded.");
      return;
    }
    const bool sending = mode_.currentIndex() == 0;
    if (sending && input_devices_.count() == 0) {
      fail("No audio input device is available.");
      return;
    }
    if (!sending && output_devices_.count() == 0) {
      fail("No stereo audio output device is available.");
      return;
    }
    if (sending && host_.text().trimmed().isEmpty()) {
      fail("Scan the LAN or enter the receiver address first.");
      return;
    }

    std::string error;
    if (sending) {
      lanaudio::control::SecureSender::Options options;
      options.host = host_.text().trimmed().toStdString();
      options.control_port =
          static_cast<std::uint16_t>(control_port_.value());
      options.preferred_sample_rate =
          static_cast<std::uint32_t>(preferred_rate_.currentData().toUInt());
      options.preferred_jitter_packets = static_cast<std::uint16_t>(
          profile_.currentIndex() == 0 ? 2
                                       : (profile_.currentIndex() == 1 ? 4 : 8));
      sender_ = std::make_unique<lanaudio::control::SecureSender>(context_,
                                                                  options);
      if (!sender_->start(error)) {
        fail(error);
        return;
      }
    } else {
      lanaudio::control::SecureReceiver::Options options;
      options.control_port =
          static_cast<std::uint16_t>(control_port_.value());
      options.media_port = static_cast<std::uint16_t>(media_port_.value());
      options.preferred_sample_rate =
          static_cast<std::uint32_t>(preferred_rate_.currentData().toUInt());
      receiver_ = std::make_unique<lanaudio::control::SecureReceiver>(
          context_, options);
      if (!receiver_->start(error)) {
        fail(error);
        return;
      }
    }
    session_active_ = true;
    connect_button_.setText("Disconnect");
    update_mode();
    if (sending) {
      std::string capture_error;
      auto preview_rate = static_cast<std::uint32_t>(
          preferred_rate_.currentData().toUInt());
      if (preview_rate == 0) preview_rate = 48'000;
      if (!start_sender_capture(preview_rate, capture_error)) {
        const auto* device = selected_device(input_devices_);
        if (device != nullptr && device->current_sample_rate != 0 &&
            device->current_sample_rate != preview_rate) {
          capture_error.clear();
          (void)start_sender_capture(device->current_sample_rate,
                                     capture_error);
        }
      }
      if (!audio_open_) {
        QMessageBox::warning(
            this, "LanAudio",
            capture_error.empty()
                ? QString("Unable to open the microphone. Grant LanAudio "
                          "access in System Settings > Privacy & Security > "
                          "Microphone.")
                : QString::fromStdString(capture_error));
      }
    }
#endif
  }

  void fail(const std::string& error) {
    stop();
    QMessageBox::critical(this, "LanAudio",
                          error.empty() ? "The audio operation failed."
                                        : QString::fromStdString(error));
  }

  void stop() {
#ifdef LANAUDIO_HAS_SECURITY
    media_live_.store(false, std::memory_order_release);
    close_audio();
    if (sender_) sender_->stop();
    if (receiver_) receiver_->stop();
    sender_.reset();
    receiver_.reset();
#endif
    session_active_ = false;
    trust_refreshed_for_session_ = false;
    pairing_confirmed_locally_ = false;
    audio_error_shown_ = false;
    audio_sample_rate_ = 0;
    preview_input_level_.store(0, std::memory_order_relaxed);
    preview_output_level_.store(0, std::memory_order_relaxed);
    confirm_button_.setEnabled(false);
    decline_button_.setEnabled(false);
    pairing_code_.setText("Pairing code: —");
    input_level_.setValue(0);
    output_level_.setValue(0);
    update_mode();
  }

  void update_metrics(const lanaudio::StreamMetrics* value) {
    const int preview_in =
        preview_input_level_.load(std::memory_order_relaxed);
    const int preview_out =
        preview_output_level_.load(std::memory_order_relaxed);
    if (!value) {
      input_level_.setValue(preview_in);
      output_level_.setValue(preview_out);
      metrics_.setText("Packets: 0  Loss: 0  Late: 0  Underruns: 0");
      return;
    }
    input_level_.setValue(std::max(
        preview_in, static_cast<int>(value->input_level_milli.load())));
    output_level_.setValue(std::max(
        preview_out, static_cast<int>(value->output_level_milli.load())));
    metrics_.setText(
        QString("Sent %1  Recv %2  Lost %3  Late %4  "
                "Underruns %5  Overruns %6  Buffer %7  "
                "Drift %8 ppm  Decrypt %9  Replay %10  Foreign %11  "
                "Resyncs %12")
            .arg(value->packets_sent.load())
            .arg(value->packets_received.load())
            .arg(value->lost_packets.load())
            .arg(value->late_packets.load())
            .arg(value->underruns.load())
            .arg(value->overruns.load())
            .arg(value->jitter_buffer_packets.load())
            .arg(value->drift_ppm.load())
            .arg(value->decrypt_failures.load())
            .arg(value->replay_rejections.load())
            .arg(value->foreign_source_rejections.load())
            .arg(value->stream_resyncs.load()));
  }

  QGroupBox* input_box_{};
  QGroupBox* output_box_{};
  QGroupBox* connection_box_{};
  QWidget* sender_discovery_{};
  QLabel* host_label_{};
  QLabel device_name_;
  QLabel fingerprint_;
  QLabel trust_backend_;
  QComboBox input_devices_;
  QComboBox input_channels_;
  QLabel input_details_;
  QProgressBar input_level_;
  QPushButton refresh_devices_button_;
  QLabel input_note_;
  QComboBox mode_;
  QListWidget peers_;
  QPushButton scan_button_;
  QPushButton pair_button_;
  QLineEdit host_;
  QSpinBox control_port_;
  QSpinBox media_port_;
  QComboBox preferred_rate_;
  QComboBox profile_;
  QComboBox device_buffer_;
  QLabel format_;
  QLabel pairing_code_;
  QPushButton confirm_button_;
  QPushButton decline_button_;
  QLabel local_address_;
  QPushButton connect_button_;
  QListWidget trusted_;
  QPushButton rename_trust_button_;
  QPushButton remove_trust_button_;
  QPushButton reset_trust_button_;
  QComboBox output_devices_;
  QComboBox output_channels_;
  QLabel output_details_;
  QProgressBar output_level_;
  QCheckBox mute_;
  QLabel status_;
  QLabel metrics_;
  QLabel security_;
  QTimer timer_;
  bool session_active_{};
  bool audio_open_{};
  bool muted_{};
  bool trust_refreshed_for_session_{};
  bool pairing_confirmed_locally_{};
  bool audio_error_shown_{};
  std::uint32_t audio_sample_rate_{};
  std::atomic<int> preview_input_level_{};
  std::atomic<int> preview_output_level_{};
  std::atomic<bool> media_live_{};
  std::vector<lanaudio::AudioDeviceInfo> devices_;
  std::unique_ptr<lanaudio::RtAudioBackend> audio_;
#ifdef LANAUDIO_HAS_SECURITY
  std::shared_ptr<lanaudio::control::SecurityContext> context_;
  std::unique_ptr<lanaudio::control::SecureSender> sender_;
  std::unique_ptr<lanaudio::control::SecureReceiver> receiver_;
#endif
};

}  // namespace

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  QApplication::setApplicationName("LanAudio");
  QApplication::setApplicationDisplayName("LanAudio");
  QApplication::setApplicationVersion(LANAUDIO_VERSION);
  QApplication::setOrganizationDomain("lanaudio.local");
  if (application.arguments().contains("--smoke-test") ||
      application.arguments().contains("--screenshot")) {
    MainWindow window;
    window.show();
    const bool screenshot = application.arguments().contains("--screenshot");
    QTimer::singleShot(screenshot ? 500 : 0, &application, [&] {
      if (screenshot) {
        window.grab().save("/tmp/lanaudio-ui-send.png");
        window.resize(620, 520);
        window.repaint();
        QApplication::processEvents();
        window.grab().save("/tmp/lanaudio-ui-send-small.png");
        window.resize(700, 720);
        window.set_mode_index(1);
        window.repaint();
        QApplication::processEvents();
        window.grab().save("/tmp/lanaudio-ui-receive.png");
      }
      QCoreApplication::quit();
    });
    return application.exec();
  }
  MainWindow window;
  window.show();
  return application.exec();
}
