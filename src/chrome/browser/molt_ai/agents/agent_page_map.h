// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The web agent's view of a page through the accessibility page map
// (voice/page_map.h; docs/design/AGENT_ACTIONS.md §2.10,
// docs/design/TOUCHLESS_BROWSING.md §6.3): the map voice mode reads pages
// through, turned into the agent's observation, and the checks the agent
// makes on the live tree before it acts.
//
//   - The observation: the page's title and URL, its headings, its lists
//     with their entries, its text (the DOM reader's, in what is left of
//     the budget) and the elements the model may act on, each with the
//     map's number as its id ("e12" is the item numbered 12, the number
//     voice mode shows on screen), its kind and name, and the credential
//     fields marked. The page's own content comes first, as the map orders
//     it, then the frames of the page's own origin that only frames of
//     that origin embed; another site's frames (ads, widgets), and what
//     they embed, are left out, as the element snapshot leaves out every
//     frame.
//   - Acting: an element names its node (PageNodeHandle). Before an action
//     the browser maps the page again and finds the node there, still of
//     its kind and name (FindLiveItem()); a node that is gone or changed is
//     stale, and the page is observed again, never guessed at. What the
//     action would touch is read from the node and its ancestors as they
//     are now (ProbeNode()): the same probe the element snapshot gives, so
//     the same questions are asked.
//   - Credential fields (password, payment card, one-time code, and fields
//     whose label names a secret) are never typed into on this path: they
//     are listed, marked, but not text fields (agent_actions::
//     ObservedElement::typeable), and the page controller refuses them
//     again.
//
// Everything the map holds is page text and untrusted. Its strings were
// cleaned for speech (page_map_text.h: no control, bidi or invisible
// characters); here they are also made one line each, with no double
// quotes, and capped, and agent_prompt fences the whole observation as it
// fences the element snapshot's.
//
// Pure logic (no content/), unit-tested in molt_ai_unittests and fuzzed by
// molt_agent_page_map_fuzzer.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_PAGE_MAP_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_PAGE_MAP_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "url/origin.h"

namespace molt_ai {
namespace agent_page_map {

// The observation's text, in code points: about what the element
// snapshot's observation takes (3,000 characters of page text and 40
// element lines).
inline constexpr size_t kMaxTextChars = 7000;
// Of that: the ELEMENTS list, the headings, the lists and their entries,
// and the page's text.
inline constexpr size_t kMaxElementsChars = 4500;
inline constexpr size_t kMaxHeadings = 12;
inline constexpr size_t kMaxLists = 3;
inline constexpr size_t kMaxEntriesPerList = 10;
inline constexpr size_t kMaxEntryChars = 200;
inline constexpr size_t kMaxHeadingChars = 100;
inline constexpr size_t kMaxPageTextChars = 1500;
inline constexpr size_t kMaxUrlChars = 300;
inline constexpr size_t kMaxTitleChars = 160;
// A select's options listed (and SELECT may choose), and their text.
inline constexpr size_t kMaxOptionsPerSelect = 12;
inline constexpr size_t kMaxOptionsTextChars = 400;

// What the browser read for one observation.
struct PageMapInput {
  PageMapInput();
  ~PageMapInput();
  PageMapInput(const PageMapInput&);
  PageMapInput& operator=(const PageMapInput&);

  // The top-level document's committed URL and origin (url::Origin::
  // Serialize()).
  std::string url;
  std::string origin;
  // The trees (voice::PageNodeHandle::tree) of the frames whose committed
  // origin is |origin|: the page's own (map->page_tree, which must be one
  // of them for anything to be listed) and its frames of the same origin
  // whose every ancestor frame has it too (not one inside another site's
  // frame). Items of other trees are left out.
  std::vector<int32_t> same_origin_trees;
  // The options of the map's selects, by item number
  // (VoicePageController::OptionsOf(): page text, cleaned for speech).
  std::vector<std::pair<int, std::vector<std::string>>> select_options;
  // The page's text as the DOM reader has it (StructuredPage::
  // main_content), shown in what is left of the budget.
  std::string page_text;
};

struct PageMapObservation {
  PageMapObservation();
  ~PageMapObservation();
  PageMapObservation(PageMapObservation&&);
  PageMapObservation& operator=(PageMapObservation&&);

  // The elements (from_page_map; at most agent_actions::
  // kMaxObservedElements, in number order).
  agent_actions::Observation observation;
  // The CURRENT PAGE text (agent_prompt fences it): "URL:", "Title:", the
  // source line, HEADINGS, LISTS, TEXT and ELEMENTS. At most kMaxTextChars
  // code points.
  std::string text;
  // The map gave the agent something: an element, a list of the page's or
  // a heading. When false the caller observes through the element snapshot
  // instead (the map is empty, or holds only what the agent can't use).
  bool usable = false;
};

// The observation of |map|, the map of the tab's primary page
// (voice::BuildPageMap(), through VoicePageController::BuildMap()), read
// with |input|. Elements are chosen in this order until
// kMaxObservedElements are, or their lines reach kMaxElementsChars, and
// listed by number: the page's own text fields and selects, the primary
// actions of the first entries of its first lists (as the map ranks them),
// its items on screen, its media, its other buttons, its other items, then
// its same-origin frames' items (on screen first). Disabled items are left
// out.
PageMapObservation BuildObservation(const voice::PageMap& map,
                                    const PageMapInput& input);

// The element snapshot's kind word for a map item ("link", "button",
// "input", "select", "checkbox", ...).
std::string_view KindName(voice::ItemKind kind);

// The page map's credential kind as agent_safety's.
agent_safety::FieldKind FieldKindOf(voice::CredentialKind kind);

// |element|'s item in |fresh|, a map made right before acting: the item of
// its node, still of the kind and with the name the model was shown
// (cleaned as the observation cleans it), and not disabled. Null when the
// node is gone or changed: the element is stale.
const voice::PageItem* FindLiveItem(
    const agent_actions::ObservedElement& element,
    const voice::PageMap& fresh);

// What acting on a node touches, from the node and its ancestors as they
// are now (|chain|: the node first; VoicePageController::Inspect()) and
// the index in it of the nearest form (-1 if none), as the element
// snapshot's probe reports it (agent_safety::ElementProbe):
//   - field_kind: the node's credential kind (voice::ClassifyCredential():
//     agent_safety's classification, and labels that name a secret);
//   - navigates_to: the link a click follows: the node's own, or its
//     nearest link ancestor's (a link's target only: an image's address is
//     its source);
//   - submits_form: the nearest <button> or <input> in the chain is inside
//     the form and submits it: an <input type=submit> or type=image, or a
//     <button> (whose type the tree doesn't give: taken as a submit, the
//     HTML default);
//   - label: the node's name, cleaned and capped as the probe's.
// An empty chain is "not found".
agent_safety::ElementProbe ProbeNode(const std::vector<voice::PageNode>& chain,
                                     int form_index);

// The question a click needs before it runs, as the web agent asks it on
// either path (the element snapshot's probe or ProbeNode()): a click that
// follows a link to an http(s) origin outside the run's |allowed| set
// "opens" it (and the origin joins the set once the user allows it), and
// one that submits a form "submits a form on <current_origin_text>". No
// effects: no question.
struct ClickQuestion {
  ClickQuestion();
  ~ClickQuestion();
  ClickQuestion(ClickQuestion&&);
  ClickQuestion& operator=(ClickQuestion&&);

  std::vector<std::string> effects;
  std::optional<url::Origin> new_origin;
};
ClickQuestion CheckClick(const agent_safety::ElementProbe& probe,
                         const std::vector<url::Origin>& allowed,
                         std::string_view current_origin_text);

// Origins per frame, at action time: the element's frame must have the
// origin it was observed with (|observed|, as ObservedElement::frame_origin
// holds it), which must be the page's (|page|, the tab's committed origin)
// and an http(s) origin in the run's |allowed| set, and every frame between
// it and the top-level document must have that origin too
// (|framed_by_own_origin|, VoicePageController::TargetFacts). A frame that
// changed origin is stale (observe again); one that is another site's, one
// of the page's origin that another site's frame embeds (which chose what
// it shows), or one that isn't a web page, is refused.
enum class FrameCheck { kOk, kStale, kRefused };
FrameCheck CheckFrameOrigin(std::string_view observed,
                            const url::Origin& live,
                            const url::Origin& page,
                            const std::vector<url::Origin>& allowed,
                            bool framed_by_own_origin);

// What typing into a field found now as |live| needs: the page-map path
// types into no credential field (|kRefuse|, with a note for the model),
// and into nothing that isn't a writable text field.
enum class TypeCheck {
  kType,
  kRefusePassword,    // a password field: the user signs in themselves
  kRefuseCredential,  // a payment card or one-time code field
  kNotTextField,
};
TypeCheck CheckType(const voice::PageItem& live);

// The step notes for the refusals above (fixed text).
std::string_view TypeRefusalNote(TypeCheck check);

}  // namespace agent_page_map
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_PAGE_MAP_H_
