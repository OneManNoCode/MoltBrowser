// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_window_listening.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/molt_ai/common/molt_settings_file.h"
#include "chrome/browser/molt_ai/voice/voice_browser_actions.h"
#include "chrome/browser/molt_ai/voice/voice_session.h"
#include "chrome/browser/molt_ai/voice/voice_session_settings.h"

namespace molt_ai {
namespace voice {

namespace {

bool Listens(BrowserWindowInterface* browser) {
  VoiceSession* session = VoiceSession::FromBrowser(browser);
  return session && session->listening();
}

void StartListening(BrowserWindowInterface* browser) {
  if (VoiceSession* session = VoiceSession::GetOrCreateForBrowser(browser)) {
    session->Start();
  }
}

void ReadAlwaysListening(base::OnceCallback<void(bool)> done) {
  MoltSettingsFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadVoiceModeSettings),
      base::BindOnce(
          [](base::OnceCallback<void(bool)> done, VoiceModeSettings settings) {
            std::move(done).Run(settings.always_listening);
          },
          std::move(done)));
}

void SetAlwaysListening(bool on, base::OnceCallback<void(bool)> done) {
  // Read, change one setting, write: on settings.json's own sequence, so no
  // other change comes in between (as the settings page does).
  MoltSettingsFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](bool on) {
            VoiceModeSettings settings = ReadVoiceModeSettings();
            settings.always_listening = on;
            const bool ok = WriteVoiceModeSettings(settings);
            return std::make_pair(settings, ok);
          },
          on),
      base::BindOnce(
          [](base::OnceCallback<void(bool)> done,
             std::pair<VoiceModeSettings, bool> written) {
            if (written.second) {
              VoiceSession::OnSettingsChanged(written.first);
            }
            std::move(done).Run(written.second);
          },
          std::move(done)));
}

}  // namespace

void InstallWindowListening() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;
  VoiceBrowserActions::WindowListening listening;
  listening.listening = base::BindRepeating(&Listens);
  listening.start = base::BindRepeating(&StartListening);
  listening.read_always_listening = base::BindRepeating(&ReadAlwaysListening);
  listening.set_always_listening = base::BindRepeating(&SetAlwaysListening);
  VoiceBrowserActions::SetWindowListening(std::move(listening));
}

}  // namespace voice
}  // namespace molt_ai
