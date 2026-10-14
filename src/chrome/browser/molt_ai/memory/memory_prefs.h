// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Profile prefs for Personal Vector Memory. Memory recording is OPT-IN:
// kEnabled defaults to false for new AND existing profiles. Pages captured
// before memory became opt-in stay on disk until the user clears them; the
// molt://memory page shows a one-time notice explaining the change.
//
// Registered by MemoryServiceFactory::RegisterProfilePrefs (keyed-service
// factories register their prefs when the profile is created), so no edit
// to chrome/browser/prefs/browser_prefs.cc is needed.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_PREFS_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_PREFS_H_

namespace user_prefs {
class PrefRegistrySyncable;
}  // namespace user_prefs

namespace molt_ai {
namespace memory {
namespace prefs {

// bool — the user opted in to recording pages into memory. Default false.
inline constexpr char kEnabled[] = "molt_ai.memory.enabled";
// bool — recording is temporarily paused (memory stays enabled and
// searchable, but no new pages are captured). Default false.
inline constexpr char kPaused[] = "molt_ai.memory.paused";
// int — keep captured pages this many days; 0 means forever. The UI offers
// 30 / 90 / 365 / forever. Default 90.
inline constexpr char kRetentionDays[] = "molt_ai.memory.retention_days";
// bool — the one-time "memory is now opt-in" notice was dismissed.
inline constexpr char kOptInNoticeDismissed[] =
    "molt_ai.memory.opt_in_notice_dismissed";

inline constexpr int kDefaultRetentionDays = 90;

// Returns |days| if it is one of the supported retention choices
// (30, 90, 365, or 0 = forever), else kDefaultRetentionDays.
int SanitizeRetentionDays(int days);

void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);

}  // namespace prefs
}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_PREFS_H_
