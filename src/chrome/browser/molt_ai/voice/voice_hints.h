// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Site hints (docs/design/TOUCHLESS_BROWSING.md §6.8): when the generic page
// map works on a site (the search box "search for X" typed into, the list
// "play the first one" chose from, the player "play" pressed), the browser
// remembers those parts for that site, by their role and name, and nothing
// else of the page. Next time it looks for the hinted part first. A hint
// that fails is dropped.
//
// The order the voice loop finds a site's parts in is one function,
// LocateSitePart():
//   1. a built-in site skill (§6.7; the skills-reading workstream's
//      voice_skills*), passed in as what it found;
//   2. the learned hint;
//   3. the generic page map (FindSearchField(), the most prominent list
//      that isn't navigation, the first player).
//
// Std-only (with the page map): unit-tested; the store's parser
// (voice_learning_store.h) reads hints back as untrusted.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HINTS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HINTS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/services/molt_ai/inference/engine/raw_ptr_exclusion.h"

namespace molt_ai {
namespace voice {

// Sites with hints, at most; the least recently used go first.
inline constexpr size_t kMaxHintedSites = 200;
// A part's name, in code points (the agent's draft keeps labels as long).
inline constexpr size_t kMaxHintNameChars = 60;
// A host, in bytes.
inline constexpr size_t kMaxHintHostBytes = 253;
// A hint unused this long is forgotten (180 days).
inline constexpr int64_t kHintLifetimeSeconds = 180LL * 24 * 3600;

enum class SitePart : uint8_t {
  kSearchField,  // a writable search field of the page itself
  kResultList,   // a list that isn't navigation, with entries
  kPlayer,       // a video or audio player
};

// "search field", "results list", "player": for settings and logs.
std::string_view SitePartName(SitePart part);

struct SiteHint {
  SiteHint();
  ~SiteHint();
  SiteHint(const SiteHint&);
  SiteHint& operator=(const SiteHint&);
  SiteHint(SiteHint&&);
  SiteHint& operator=(SiteHint&&);

  // Lower case.
  std::string host;
  // Each part's accessible name, cleaned and capped (page text, shown and
  // compared, never acted on as text); empty when there is no hint for it.
  std::string search_field;
  std::string result_list;
  std::string player;
  // Unix seconds.
  int64_t last_used = 0;

  const std::string& name(SitePart part) const;
  std::string& name(SitePart part);
  bool empty() const;
};

// Where a part was found.
enum class PartSource : uint8_t {
  kNone,
  kSkill,
  kHint,
  kGeneric,
};

struct LocatedPart {
  PartSource source = PartSource::kNone;
  // One of these, by the part asked for; into the map passed.
  MOLT_RAW_PTR_EXCLUSION const PageItem* field = nullptr;
  MOLT_RAW_PTR_EXCLUSION const PageList* list = nullptr;
  MOLT_RAW_PTR_EXCLUSION const PageMedia* media = nullptr;

  bool found() const { return field || list || media; }
};

// What a turn used of the page, to learn from once its steps worked
// (voice_learning_loop.h), or to drop a hint that failed.
struct SitePartUse {
  SitePartUse();
  ~SitePartUse();
  SitePartUse(const SitePartUse&);
  SitePartUse& operator=(const SitePartUse&);
  SitePartUse(SitePartUse&&);
  SitePartUse& operator=(SitePartUse&&);

  SitePart part = SitePart::kSearchField;
  PartSource source = PartSource::kNone;
  // The site it was on, and the part's name (HintName()).
  std::string host;
  std::string name;
  // A hint for it named something the page doesn't have now (the site
  // changed): the hint goes unless the generic part works.
  bool hint_missed = false;
};

// Finds |part| on |map|: |skill| when it found one (a built-in site skill,
// §6.7), else the part |hint| names (when |hint| is for this part), else
// the generic map's. |hint| and |skill| may be null.
LocatedPart LocateSitePart(SitePart part,
                           const PageMap& map,
                           const LocatedPart* skill,
                           const SiteHint* hint);

// The name a found part is remembered by: its label, cleaned and capped;
// "" when it has none (then it can't be hinted).
std::string HintName(SitePart part, const LocatedPart& found);

// The hints of every site, as kept in the profile.
class SiteHints {
 public:
  SiteHints();
  ~SiteHints();
  SiteHints(const SiteHints&);
  SiteHints& operator=(const SiteHints&);
  SiteHints(SiteHints&&);
  SiteHints& operator=(SiteHints&&);

  // The hint for |host|, or null.
  const SiteHint* Find(std::string_view host) const;
  // |part| named |name| worked on |host| at |now|: remembered (replacing
  // an older name). Nothing for a host or name that isn't usable.
  void Worked(std::string_view host,
              SitePart part,
              std::string_view name,
              int64_t now);
  // The hint for |part| on |host| failed: dropped (the site's entry goes
  // when it has no hint left).
  void Failed(std::string_view host, SitePart part);
  // Forgets |host|'s hints; whether there were any.
  bool Forget(std::string_view host);
  void Clear();
  // Drops hints unused for kHintLifetimeSeconds (or dated a day past
  // |now|), then the least recently used beyond kMaxHintedSites.
  void Age(int64_t now);
  // Adds |hint| as stored (the store's parser), if valid and new.
  bool Restore(SiteHint hint);

  const std::vector<SiteHint>& hints() const { return hints_; }
  bool empty() const { return hints_.empty(); }

 private:
  SiteHint* FindMutable(std::string_view host);
  void Evict();

  std::vector<SiteHint> hints_;
};

// A host a hint may be kept for: a lower-case DNS name (letters, digits,
// '-', '.'), at most kMaxHintHostBytes; or "" when |host| isn't one.
std::string HintHost(std::string_view host);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HINTS_H_
