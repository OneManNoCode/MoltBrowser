// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/renderer/molt_ai/fingerprinting/molt_fingerprinting_renderer.h"

#include <cstdint>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting.mojom.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_switches.h"
#include "chrome/renderer/molt_ai/fingerprinting/moltnet_locale_strings.h"
#include "content/public/renderer/render_thread.h"
#include "content/public/renderer/render_thread_observer.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/web/web_molt_fingerprinting.h"

namespace molt_ai::fingerprinting {

namespace {

// Receives the browser's configuration messages on the main thread, in
// channel order. The browser opens a new endpoint per message (like
// chrome::mojom::RendererConfiguration), hence the receiver set.
class ConfigurationReceiver : public content::RenderThreadObserver,
                              public mojom::FingerprintingConfiguration {
 public:
  ConfigurationReceiver() = default;
  ConfigurationReceiver(const ConfigurationReceiver&) = delete;
  ConfigurationReceiver& operator=(const ConfigurationReceiver&) = delete;
  ~ConfigurationReceiver() override = default;

  // content::RenderThreadObserver:
  void RegisterMojoInterfaces(
      blink::AssociatedInterfaceRegistry* associated_interfaces) override {
    associated_interfaces->AddInterface<mojom::FingerprintingConfiguration>(
        base::BindRepeating(&ConfigurationReceiver::Bind,
                            base::Unretained(this)));
  }
  void UnregisterMojoInterfaces(
      blink::AssociatedInterfaceRegistry* associated_interfaces) override {
    associated_interfaces->RemoveInterface(
        mojom::FingerprintingConfiguration::Name_);
  }

  // mojom::FingerprintingConfiguration:
  void SetSessionSeed(const std::vector<uint8_t>& seed) override {
    // Blink ignores anything but the first 32-byte seed.
    blink::WebMoltFingerprinting::SetSessionSeed(seed);
  }

 private:
  void Bind(mojo::PendingAssociatedReceiver<mojom::FingerprintingConfiguration>
                receiver) {
    receivers_.Add(this, std::move(receiver));
  }

  mojo::AssociatedReceiverSet<mojom::FingerprintingConfiguration> receivers_;
};

RendererMode ModeFromCommandLine() {
  return ModeFromSwitchValue(
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          switches::kMode));
}

}  // namespace

void ConfigureRendererProcessBeforeBlink() {
  if (ModeFromCommandLine() == RendererMode::kUniform) {
    blink::WebMoltFingerprinting::ConfigureBeforeBlinkInitialization(
        blink::WebMoltFingerprinting::Mode::kUniform);
  }
}

void ConfigureRendererProcess() {
  const RendererMode mode = ModeFromCommandLine();
  if (mode == RendererMode::kOff) {
    return;
  }
  blink::WebMoltFingerprinting::Configure(
      mode == RendererMode::kUniform
          ? blink::WebMoltFingerprinting::Mode::kUniform
          : blink::WebMoltFingerprinting::Mode::kFarble);
  if (mode == RendererMode::kUniform) {
    // MoltNet: the strings pages can read in en-US, like the rest of what
    // kUniform pins (Blink can't reach the ResourceBundle they come from).
    PinMoltNetLocaleStrings();
  }
  // Lives as long as the process; registering with the render thread exposes
  // the interface on the channel before any frame or worker exists.
  static base::NoDestructor<ConfigurationReceiver> receiver;
  content::RenderThread::Get()->AddObserver(receiver.get());
}

}  // namespace molt_ai::fingerprinting
