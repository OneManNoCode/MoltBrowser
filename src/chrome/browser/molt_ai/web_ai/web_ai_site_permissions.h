// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What MoltBrowser remembers, per profile, about each site and the built-in
// AI APIs (LanguageModel, Summarizer, Writer, Rewriter):
//  * the user's answer to the consent prompt, Allow or Block, keyed by the
//    top-level origin, until the user removes it (molt://ai-settings), clears
//    "Site settings" or resets the profile;
//  * how often the prompt was closed without an answer. After
//    kDismissalsBeforeEmbargo closes the prompt is paused for the site for
//    kEmbargo (an embargo, as Chromium's PermissionDecisionAutoBlocker does
//    for permission prompts); the site then sees what a blocked site sees.
//
// Standard library only, so molt_ai_unittests tests it without a browser.
// WebAISitePermissions (web_ai_site_permissions_service.h) keeps a table in
// the regular profile's prefs, or in memory only for Incognito and Guest.
// Design: docs/design/WEB_AI_APIS.md §4.
//
// Times are milliseconds since the Unix epoch, as base::Time gives them.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SITE_PERMISSIONS_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SITE_PERMISSIONS_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"

namespace molt_ai::web_ai {

// Closing the prompt (its close button, not Allow or Block) this many times
// pauses it for the site. Chromium's number for permission prompts: one or
// two stray closes don't silence a site, and a site can't put the bar up
// more than three times before a pause. That matters here because Blink
// only asks for *sticky* activation before create() reaches the browser, so
// after one click a page could otherwise raise the bar again as soon as it
// is closed.
inline constexpr uint32_t kDismissalsBeforeEmbargo = 3;

// How long the pause lasts (Chromium's 7 days): long enough that a site
// that keeps asking stops being a nuisance, short enough that a site the
// user later wants can ask again without a trip to settings. Once a site
// has been paused, each further close pauses it again for as long (the
// count isn't reset when a pause ends), as in Chromium.
inline constexpr int64_t kEmbargoMs = int64_t{7} * 24 * 60 * 60 * 1000;

// The prompt ignores Allow and Block for this long after it appears, as
// Chromium's permission prompts ignore clicks that come too soon after they
// show (views::InputEventActivationProtector, the double-click interval): a
// page raises the bar on a click, so it could have the user's next click
// (the second of a double click) land where the bar's buttons appear.
inline constexpr int64_t kPromptInputProtectionMs = 500;

// Whether Allow or Block, |elapsed_ms| after the prompt appeared (on a
// monotonic clock), is the user's answer. Too soon, the click does nothing
// and the prompt stays.
bool IsTimelyPromptAnswer(int64_t elapsed_ms);

// Close counts are stored up to this; only ">= kDismissalsBeforeEmbargo"
// matters.
inline constexpr uint32_t kMaxStoredDismissals = 100;

// The latest time accepted from storage (2^53 ms, where doubles stop being
// exact): stored times are doubles in the pref.
inline constexpr int64_t kMaxStoredTimeMs = int64_t{1} << 53;

// Site keys are serialized http(s) origins ("https://example.com",
// "http://[::1]:8080"); anything longer isn't one.
inline constexpr size_t kMaxSiteKeyBytes = 1024;

// True for strings shaped like a serialized http(s) origin: the scheme, a
// non-empty host[:port] of printable ASCII without '/', '?', '#', '@' or
// '\\', at most kMaxSiteKeyBytes. Linear; no parsing beyond that. The
// browser side also checks that the key is the canonical serialization of
// an origin before storing or loading it.
bool IsPlausibleSiteKey(std::string_view site);

// One site's state.
struct SiteRecord {
  // kAllowed or kBlocked once the user answered; kAsk while only the
  // prompt's history is kept.
  SiteConsent answer = SiteConsent::kAsk;
  // When the user answered.
  int64_t answered_ms = 0;
  // Times the prompt was closed without an answer since the last answer.
  // Always 0 while there is an answer.
  uint32_t dismissals = 0;
  // When it was last closed without an answer.
  int64_t last_dismissal_ms = 0;
};

// The prompt is paused for the site: closed kDismissalsBeforeEmbargo times
// or more, the last time less than kEmbargoMs ago. A clock set back never
// stretches a pause beyond kEmbargoMs from the stored time either way.
bool IsUnderEmbargo(const SiteRecord& record, int64_t now_ms);

// What applies to the site now: the user's answer; kBlocked while the
// prompt is paused, so a paused site can't be told apart from a blocked one;
// otherwise kAsk.
SiteConsent EffectiveConsent(const SiteRecord& record, int64_t now_ms);

// A row on the settings page.
struct SiteListEntry {
  enum class Kind {
    kAllowed,
    kBlocked,
    // The prompt is paused (IsUnderEmbargo).
    kPaused,
  };
  std::string site;
  Kind kind = Kind::kAllowed;
  // When the user answered, or (kPaused) last closed the prompt.
  int64_t since_ms = 0;
  // kPaused: when the site may ask again.
  int64_t until_ms = 0;
};

// One profile's sites. Bounded: past kMaxSites the oldest prompt history
// goes first, then the oldest answer. Not thread-safe (UI thread).
class SitePermissionTable {
 public:
  static constexpr size_t kMaxSites = 512;

  SitePermissionTable();
  ~SitePermissionTable();
  SitePermissionTable(SitePermissionTable&&);
  SitePermissionTable& operator=(SitePermissionTable&&);
  SitePermissionTable(const SitePermissionTable&) = delete;
  SitePermissionTable& operator=(const SitePermissionTable&) = delete;

  // EffectiveConsent() for |site| (kAsk for unknown sites).
  SiteConsent Get(std::string_view site, int64_t now_ms) const;
  // Null for unknown sites.
  const SiteRecord* Find(std::string_view site) const;

  // The user answered the prompt. kAllowed / kBlocked replace any earlier
  // answer and clear the prompt's history; kAsk forgets the site. False,
  // and nothing changes, for a key that isn't IsPlausibleSiteKey().
  bool SetAnswer(const std::string& site, SiteConsent answer, int64_t now_ms);

  // The prompt was closed without an answer. Ignored for a site with an
  // answer (no prompt is shown for it). True if the site is now paused.
  bool RecordDismissal(const std::string& site, int64_t now_ms);

  // The settings page: forgets one site (its answer and the prompt's
  // history). Only an exact key matches. False if there was nothing to
  // forget.
  bool Remove(std::string_view site);
  // Forgets everything. False if the table was already empty.
  bool Clear();

  // Clear browsing data, "Site settings": answers given in
  // [begin_ms, end_ms), for the sites in |sites|.
  size_t RemoveAnswersBetween(int64_t begin_ms,
                              int64_t end_ms,
                              const std::vector<std::string>& sites);
  // Clear browsing data, history and site usage data: the prompt's history
  // (closes and pauses) for the sites in |sites|, whenever recorded (as
  // Chromium's PermissionDecisionAutoBlocker::RemoveEmbargoAndResetCounts).
  size_t RemovePromptHistory(const std::vector<std::string>& sites);

  // Every key, in order (for filters that need the URL).
  std::vector<std::string> Sites() const;
  // The rows for the settings page: allowed, then blocked, then paused
  // sites, each by key. Prompt history that isn't a pause isn't shown.
  std::vector<SiteListEntry> List(int64_t now_ms) const;

  // Loading: adds a record that RecordFromStored() accepted, making room as
  // past kMaxSites. False for an implausible key or an empty record.
  bool Insert(const std::string& site, const SiteRecord& record);

  size_t size() const { return records_.size(); }
  const std::map<std::string, SiteRecord, std::less<>>& records() const {
    return records_;
  }

 private:
  // Drops one record if the table is full (see the class comment).
  void MakeRoom();

  std::map<std::string, SiteRecord, std::less<>> records_;
};

// ---------------------------------------------------------------------------
// The stored form
// ---------------------------------------------------------------------------
//
// The profile pref is a dictionary:
//   {"version": 1,
//    "sites": {"https://example.com": {"answer": "allow", "answered": <ms>},
//              "https://other.example": {"dismissals": 3,
//                                        "last_dismissal": <ms>}}}
// Times are doubles (milliseconds since the Unix epoch). Format 1 is the
// first persisted one: before it, answers were kept in memory for the
// browser session only, so there is nothing older to migrate. A later
// format may add fields; it must not change what these mean. Reading
// tolerates anything: an entry that doesn't make sense is dropped, never
// trusted, and the table is written back in format 1 on the next change.

inline constexpr int kStoredFormatVersion = 1;
inline constexpr char kStoredVersionKey[] = "version";
inline constexpr char kStoredSitesKey[] = "sites";
inline constexpr char kStoredAnswerKey[] = "answer";
inline constexpr char kStoredAnsweredKey[] = "answered";
inline constexpr char kStoredDismissalsKey[] = "dismissals";
inline constexpr char kStoredLastDismissalKey[] = "last_dismissal";
inline constexpr char kStoredAllow[] = "allow";
inline constexpr char kStoredBlock[] = "block";

// How to read the stored dictionary, by its "version" (nullopt: missing or
// not an integer).
enum class StoredFormat {
  // Nothing usable (no version, or below 1): start empty. Format 1 has
  // always written its version, so this is a damaged or foreign value.
  kDiscard,
  // Format 1, or a later one (read the fields this code knows).
  kRead,
};
StoredFormat ClassifyStoredVersion(std::optional<int> version);

// One stored site, field by field as found (nullopt: missing or of the
// wrong type).
struct StoredSite {
  std::optional<std::string> answer;
  std::optional<double> answered_ms;
  std::optional<int> dismissals;
  std::optional<double> last_dismissal_ms;
};

// The record a stored site stands for, or nullopt to drop it:
//  * "answer" "allow" / "block" makes an answer; its time is clamped into
//    [0, now] (a missing or bad time counts as now, so Clear browsing data
//    for "the last hour" removes it rather than keeping it forever);
//    any prompt history stored beside an answer is dropped;
//  * otherwise "dismissals" >= 1 with a valid "last_dismissal" makes prompt
//    history (count clamped to kMaxStoredDismissals);
//  * anything else is dropped.
std::optional<SiteRecord> RecordFromStored(const StoredSite& stored,
                                           int64_t now_ms);

// The stored form of |record| (the inverse of RecordFromStored()).
StoredSite StoredFromRecord(const SiteRecord& record);

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SITE_PERMISSIONS_H_
