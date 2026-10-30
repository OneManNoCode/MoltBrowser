// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Site skills (docs/design/TOUCHLESS_BROWSING.md §6.7,
// docs/design/VOICE_SKILLS.md): declarative data that names the parts of a
// big site's pages, so "search YouTube for X", "play the fourth one", "skip
// the ad" and "next page" don't depend on guessing. A skill names, by
// accessibility role and name, or by attributes the accessibility tree
// exposes (HTML tag, id, class, link target, input type):
//   - the search box and its submit button;
//   - what one result is (its title; each result is the largest part of
//     the page around one title that holds no other), read with the page
//     map's entry reader (title, channel or site, duration, date, counts);
//   - the player and its controls (skip the ad, captions, full screen);
//   - the next page of results, or that the site loads more as it scrolls;
//   - for reading aloud: where the text is, and what to leave out.
//
// Skills are data, not code: JSON compiled into the browser
// (site_skills_data.cc), versioned ("format", and each skill's "version"),
// and parsed as untrusted with caps (ParseSiteSkills()), so a skill can't
// make the browser do anything but find nodes. A skill only ever names
// nodes of the top-level document (never of a frame it embeds, which may be
// another site's), and only as the kind of item the part needs; everything
// found still goes through the voice loop's checks (agent_safety, the
// confirmations, the page controller's stale-node checks).
//
// Order (BuildPageMapWithSkills()): the built-in skill for the page's
// host, then a learned site hint in the same form (TOUCHLESS_BROWSING.md
// §6.8, the learning side's), then the generic page map, part by part. A
// skill that finds nothing on a page where it expects a part is reported
// (SkillReport), so a broken skill shows in the browser's log.
//
// Std-only (no //base); unit-tested against reduced copies of the sites'
// pages (chrome/test/data/molt_voice/skills/) in molt_ai_unittests, and
// fuzzed over hostile trees (molt_page_reading_fuzzer).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_SITE_SKILLS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_SITE_SKILLS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map.h"

namespace molt_ai {
namespace voice {

// The format version ParseSiteSkills() reads.
inline constexpr int kSiteSkillsFormat = 1;

// Caps on what a skill file holds.
inline constexpr size_t kMaxSkillsJsonBytes = 64 * 1024;
inline constexpr size_t kMaxSkills = 32;
inline constexpr size_t kMaxSkillHosts = 16;
// Alternatives of a part, and of one constraint ("link|button").
inline constexpr size_t kMaxSkillAlternatives = 12;
inline constexpr size_t kMaxSkillChoices = 12;
inline constexpr size_t kMaxSkillStringBytes = 64;
inline constexpr size_t kMaxSkillPages = 8;

// Caps on the work a skill does on a page.
// An "inside" constraint looks this many ancestors up, "has" this many
// nodes down.
inline constexpr size_t kMaxSkillInsideHops = 64;
inline constexpr size_t kMaxSkillHasNodes = 256;
// Results a skill reads, and nodes it leaves out of reading.
inline constexpr size_t kMaxSkillResults = 200;
inline constexpr size_t kMaxSkillSkips = 512;

// One way to find a node: every constraint that is set must hold. Names
// are compared folded (FoldForComparison(): case, punctuation, accents);
// tags, roles and link targets in ASCII lower case; ids and class tokens as
// written.
struct SkillSelector {
  SkillSelector();
  ~SkillSelector();
  SkillSelector(const SkillSelector&);
  SkillSelector& operator=(const SkillSelector&);
  SkillSelector(SkillSelector&&);
  SkillSelector& operator=(SkillSelector&&);

  // PageRole bits (1 << role); 0: any role.
  uint64_t roles = 0;
  // The name is one of these.
  std::vector<std::string> names;
  // The name holds one of these, as whole words.
  std::vector<std::string> name_has;
  std::vector<std::string> ids;
  // One of the class attribute's tokens is one of these.
  std::vector<std::string> classes;
  std::vector<std::string> tags;
  // The link target holds one of these, and none of |url_not|.
  std::vector<std::string> url_has;
  std::vector<std::string> url_not;
  std::vector<std::string> input_types;
  // A heading's level, 0 for any.
  int min_level = 0;
  int max_level = 0;
  // An ancestor (at most kMaxSkillInsideHops up) matches inside[0], and a
  // node of the subtree (at most kMaxSkillHasNodes) matches has[0]. One
  // level only: these have no |inside| or |has| of their own.
  std::vector<SkillSelector> inside;
  std::vector<SkillSelector> has;
};

// A part: the selectors tried in order; the first that finds a node wins.
using SkillPart = std::vector<SkillSelector>;

// The parts, as bits (SkillReport, SiteSkill::Page::expect).
enum SkillPartBit : uint32_t {
  kPartSearch = 1u << 0,
  kPartSearchSubmit = 1u << 1,
  kPartResults = 1u << 2,
  kPartMedia = 1u << 3,
  kPartSkipAd = 1u << 4,
  kPartCaptions = 1u << 5,
  kPartFullscreen = 1u << 6,
  kPartNextPage = 1u << 7,
  kPartContent = 1u << 8,
};

// "search", "results", ...: the part's name in the JSON and in logs.
std::string_view SkillPartName(SkillPartBit part);

struct SiteSkill {
  SiteSkill();
  ~SiteSkill();
  SiteSkill(const SiteSkill&);
  SiteSkill& operator=(const SiteSkill&);
  SiteSkill(SiteSkill&&);
  SiteSkill& operator=(SiteSkill&&);

  // "youtube", and its version: "youtube@1" in logs.
  std::string id;
  int version = 0;
  // As the browser says it: "YouTube".
  std::string name;
  // Where it applies: a host in |hosts| or its www. host, or a host in
  // |domains| or any of its subdomains; never a host in |except_hosts| or
  // one of their subdomains. Lower case.
  std::vector<std::string> hosts;
  std::vector<std::string> domains;
  std::vector<std::string> except_hosts;

  SkillPart search_field;
  SkillPart search_submit;
  // Results: their titles, within the first node |results_within| finds
  // (anywhere when it finds none).
  SkillPart results_within;
  SkillPart result_title;
  std::string results_label;
  SkillPart media;
  SkillPart skip_ad;
  SkillPart captions;
  SkillPart fullscreen;
  SkillPart next_page;
  // The site loads more results as the page scrolls (no next page).
  bool next_page_scrolls = false;
  // Reading aloud: where the text is (the first node found), and subtrees
  // left out (every node found).
  SkillPart content;
  SkillPart reading_skip;

  // What the skill expects to find on pages whose path starts with |path|
  // (the first that fits): missing parts are reported.
  struct Page {
    std::string path;
    uint32_t expect = 0;
  };
  std::vector<Page> pages;

  // "youtube@1".
  std::string Label() const;
  // Whether it applies to |host| (any case).
  bool AppliesTo(std::string_view host) const;
};

struct SkillParseResult {
  SkillParseResult();
  ~SkillParseResult();
  SkillParseResult(const SkillParseResult&);
  SkillParseResult& operator=(const SkillParseResult&);
  SkillParseResult(SkillParseResult&&);
  SkillParseResult& operator=(SkillParseResult&&);

  std::vector<SiteSkill> skills;
  // Why the file, or a skill in it, was refused ("youtube: unknown key
  // 'rol'"): a refused skill is left out, the others are kept.
  std::vector<std::string> errors;
};

// Parses a skill file: strict JSON (bounded_json.h), at most
// kMaxSkillsJsonBytes, format kSiteSkillsFormat. Anything unexpected in a
// skill (an unknown key or role, a string too long, too many alternatives,
// a constraint nested too deep) refuses that skill.
SkillParseResult ParseSiteSkills(std::string_view json);

// The built-in skills (YouTube, Google, DuckDuckGo, Bing, Wikipedia),
// parsed once from BuiltInSiteSkillsJson(); kept for the life of the
// process.
const std::vector<SiteSkill>& BuiltInSiteSkills();
// The skill file compiled into the browser (site_skills_data.cc).
std::string_view BuiltInSiteSkillsJson();
// The built-in skill for |host|, or null.
const SiteSkill* BuiltInSkillForHost(std::string_view host);

// What BuildPageMapWithSkills() found, for logs and tests.
struct SkillReport {
  SkillReport();
  ~SkillReport();
  SkillReport(const SkillReport&);
  SkillReport& operator=(const SkillReport&);
  SkillReport(SkillReport&&);
  SkillReport& operator=(SkillReport&&);

  // The skill's label, or empty when none applied.
  std::string skill;
  // Parts the skill found, the hint found, and parts the skill expects on
  // this page but found nothing for (the generic map was used for them).
  uint32_t from_skill = 0;
  uint32_t from_hint = 0;
  uint32_t missing = 0;
  size_t results = 0;
};

// The page map of |tree| with its site's parts (PageMap::site), each from
// the first of |skill| (the built-in skill for the page, or null) and
// |hint| (a learned site hint in the same form, or null) that finds it,
// else left to the generic map. |path| is the page's URL path (for
// SiteSkill::pages). |report| (optional) says what came from where.
PageMap BuildPageMapWithSkills(const PageTree& tree,
                               const PageMapLimits& limits,
                               const SiteSkill* skill,
                               const SiteSkill* hint,
                               std::string_view path,
                               SkillReport* report = nullptr);

// Where reading aloud takes the page's text from, by a skill: the root
// (invalid when the skill names none on this page) and the subtrees left
// out. Only nodes of the top-level document.
struct SkillReadingScope {
  SkillReadingScope();
  ~SkillReadingScope();
  SkillReadingScope(const SkillReadingScope&);
  SkillReadingScope& operator=(const SkillReadingScope&);
  SkillReadingScope(SkillReadingScope&&);
  SkillReadingScope& operator=(SkillReadingScope&&);

  PageNodeHandle root;
  std::vector<PageNodeHandle> skip;
};
SkillReadingScope ReadingScopeFor(const SiteSkill& skill,
                                  const PageTree& tree,
                                  const PageMapLimits& limits);

// Test-only: while it lives, BuiltInSkillForHost(|host|) is the built-in
// skill |skill_id| (a browser test serves a site's fixture pages from the
// test server). One at a time; not for production code.
class ScopedSkillHostForTesting {
 public:
  ScopedSkillHostForTesting(std::string host, std::string_view skill_id);
  ScopedSkillHostForTesting(const ScopedSkillHostForTesting&) = delete;
  ScopedSkillHostForTesting& operator=(const ScopedSkillHostForTesting&) =
      delete;
  ~ScopedSkillHostForTesting();
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SITE_SKILLS_H_
