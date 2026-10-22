// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet: the renderer strings web pages can read, in en-US whatever the
// browser's UI language.
//
// Blink gets the text of validationMessage, the file input's labels, the
// <details> and button defaults, date-field placeholders, the permission
// elements' labels and the like from the embedder
// (Platform::QueryLocalizedString -> ChromeContentClient::GetLocalizedString
// -> the renderer's ui::ResourceBundle), i.e. from the locale pak of the UI
// language. In a MoltNet renderer (kUniform) PinMoltNetLocaleStrings()
// overrides exactly those strings with their en-US text, in the
// ResourceBundle itself, so every Blink caller gets them (worker threads
// too) and nothing else changes: the browser UI, error pages, media
// controls and accessibility names keep the user's language.
//
// Why not --lang=en-US for MoltNet renderers: content appends --lang after
// the embedder's switches (RenderProcessHostImpl::AppendRendererCommandLine),
// Linux renderers are forked from a zygote that loaded its locale pak before
// any profile existed, and every renderer-made text in MoltNet tabs would
// turn English. Overrides are in-process, identical on every OS, and apply
// after the zygote fork.
//
// The table (moltnet_locale_strings_table.cc) is generated from Chromium's
// blink_strings.grd and permission_element_strings.grd at the pin by
// tools/moltnet_locale/generate_tables.py, which also lists the strings left
// localized and why; moltnet_locale_strings_unittest.cc compares it with the
// en-US paks grit builds.

#ifndef CHROME_RENDERER_MOLT_AI_FINGERPRINTING_MOLTNET_LOCALE_STRINGS_H_
#define CHROME_RENDERER_MOLT_AI_FINGERPRINTING_MOLTNET_LOCALE_STRINGS_H_

#include <cstddef>
#include <string_view>

#include "base/containers/span.h"

namespace molt_ai::fingerprinting {

struct MoltNetLocaleString {
  // The grit resource id (IDS_...).
  int id;
  // Its name, for messages.
  std::string_view name;
  // The en-US text, exactly as the en-US pak has it ($1-style placeholders
  // included; Blink fills them in).
  std::u16string_view en_us;
  // False for the few strings pinned only so a picker popup isn't half
  // English (pages can't read the popup).
  bool page_observable;
};

// Every string MoltNet pins, in a fixed order.
base::span<const MoltNetLocaleString> MoltNetLocaleStrings();

// Overrides each string of MoltNetLocaleStrings() in the process's shared
// ResourceBundle. Call once, on the main thread of a kUniform renderer from
// ChromeContentRendererClient::RenderThreadStarted (through
// ConfigureRendererProcess()), before any frame or worker exists:
// ResourceBundle::OverrideLocaleStringResource isn't thread-safe, and no
// other thread reads strings yet. Returns how many it set (0 without a
// shared ResourceBundle).
size_t PinMoltNetLocaleStrings();

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_RENDERER_MOLT_AI_FINGERPRINTING_MOLTNET_LOCALE_STRINGS_H_
