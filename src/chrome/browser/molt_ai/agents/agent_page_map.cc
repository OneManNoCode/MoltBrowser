// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/agent_page_map.h"

#include <algorithm>
#include <tuple>
#include <utility>

#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace molt_ai {
namespace agent_page_map {

namespace {

using agent_actions::ObservedElement;
using voice::ItemKind;
using voice::PageItem;
using voice::PageMap;

// The probe's label cap (agent_safety's probe script slices at 60).
constexpr size_t kMaxProbeLabelChars = 60;

// Map text as one prompt line: cleaned for speech (one line, no control,
// bidi or invisible characters; voice/page_map_text.h), double quotes as
// single ones (so a name can't close its quotes and fake another element),
// at most |max_chars| code points.
std::string PromptText(std::string_view text, size_t max_chars) {
  std::string out = voice::SpeechSafeText(text, max_chars);
  std::replace(out.begin(), out.end(), '"', '\'');
  return out;
}

std::string LowerAscii(std::string_view s, size_t max) {
  std::string out(s.substr(0, max));
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

std::string TrimAscii(std::string s) {
  const auto space = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
  };
  while (!s.empty() && space(s.back())) {
    s.pop_back();
  }
  size_t start = 0;
  while (start < s.size() && space(s[start])) {
    ++start;
  }
  return s.substr(start);
}

bool InTrees(const std::vector<int32_t>& trees, int32_t tree) {
  return std::find(trees.begin(), trees.end(), tree) != trees.end();
}

const std::vector<std::string>* OptionsFor(const PageMapInput& input,
                                           int number) {
  for (const auto& [item, options] : input.select_options) {
    if (item == number) {
      return &options;
    }
  }
  return nullptr;
}

// The options SELECT may choose: as the controller names them, so the one
// chosen is found again by that name (VoicePageController::SelectOption());
// one whose name a prompt line would have to change (a double quote) or that
// is over the cap is left out, and so are repeats.
std::vector<std::string> UsableOptions(const std::vector<std::string>& raw) {
  std::vector<std::string> options;
  size_t chars = 0;
  for (const std::string& option : raw) {
    if (options.size() >= kMaxOptionsPerSelect) {
      break;
    }
    const size_t length = voice::CountCodePoints(option);
    if (option.empty() || length > agent_actions::kMaxOptionChars ||
        PromptText(option, agent_actions::kMaxOptionChars) != option ||
        std::find(options.begin(), options.end(), option) != options.end()) {
      continue;
    }
    if (chars + length > kMaxOptionsTextChars) {
      break;
    }
    chars += length;
    options.push_back(option);
  }
  return options;
}

ObservedElement ElementFor(const PageItem& item, const PageMapInput& input) {
  ObservedElement e;
  e.ref = "e" + std::to_string(item.number);
  e.kind = std::string(KindName(item.kind));
  e.label = PromptText(item.label, agent_actions::kMaxElementLabelChars);
  e.field_kind = FieldKindOf(item.credential);
  e.typeable = item.kind == ItemKind::kTextField && item.writable &&
               !item.disabled &&
               item.credential == voice::CredentialKind::kNone;
  e.item_number = item.number;
  e.node_tree = item.handle.tree;
  e.node_id = item.handle.node;
  // Every listed tree has the page's origin (PageMapInput::
  // same_origin_trees); the action checks the frame's again.
  e.frame_origin = input.origin;
  if (item.kind == ItemKind::kSelect && !item.disabled) {
    if (const std::vector<std::string>* raw = OptionsFor(input, item.number)) {
      e.options = UsableOptions(*raw);
      e.selectable = !e.options.empty();
    }
  }
  return e;
}

// The ELEMENTS line of |e| (map item |item|): the snapshot's description,
// and what the map says of its state.
std::string ElementLine(const ObservedElement& e, const PageItem& item) {
  std::string line = e.ref + " " + agent_actions::DescribeElement(e);
  if ((item.kind == ItemKind::kCheckBox || item.kind == ItemKind::kRadio ||
       item.kind == ItemKind::kSwitch) &&
      item.checked) {
    line += " (checked)";
  }
  if (item.kind == ItemKind::kSelect && !item.value.empty()) {
    line += " (now \"" + PromptText(item.value, 60) + "\")";
  }
  return line + "\n";
}

std::string EntryLine(const voice::PageListEntry& entry,
                      const std::vector<int>& listed_numbers) {
  std::string line = "  " + std::to_string(entry.position) + ".";
  if (entry.number > 0 &&
      std::find(listed_numbers.begin(), listed_numbers.end(), entry.number) !=
          listed_numbers.end()) {
    line += " e" + std::to_string(entry.number);
  }
  const std::string title = PromptText(entry.title, 120);
  if (!title.empty()) {
    line += " \"" + title + "\"";
  }
  std::vector<std::string> fields;
  for (const std::string* field : {&entry.channel, &entry.duration}) {
    std::string text = PromptText(*field, 40);
    if (!text.empty()) {
      fields.push_back(std::move(text));
    }
  }
  for (const voice::EntryCount& count : entry.counts) {
    std::string text = PromptText(count.text, 40);
    if (!text.empty()) {
      fields.push_back(std::move(text));
    }
  }
  if (std::string date = PromptText(entry.date, 40); !date.empty()) {
    fields.push_back(std::move(date));
  }
  // As many fields as fit: the line stays within kMaxEntryChars (each piece
  // was cleaned and capped above).
  for (const std::string& field : fields) {
    if (voice::CountCodePoints(line) + 2 + voice::CountCodePoints(field) >
        kMaxEntryChars) {
      break;
    }
    line += ", " + field;
  }
  return line + "\n";
}

// Appends |line| to |out| if it fits in |budget| code points (which it
// then takes from).
bool AppendWithin(std::string* out, const std::string& line, size_t* budget) {
  const size_t length = voice::CountCodePoints(line);
  if (length > *budget) {
    return false;
  }
  *budget -= length;
  out->append(line);
  return true;
}

// The order elements are chosen in (lower first); see BuildObservation().
int Tier(const PageItem& item, const PageMap& map) {
  const bool own = map.InPage(item.handle);
  if (!own) {
    return item.on_screen ? 6 : 7;
  }
  if (item.kind == ItemKind::kTextField || item.kind == ItemKind::kSelect) {
    return 0;
  }
  if (item.list >= 0 && static_cast<size_t>(item.list) < kMaxLists &&
      static_cast<size_t>(item.list) < map.lists.size() && item.entry >= 0 &&
      static_cast<size_t>(item.entry) < kMaxEntriesPerList &&
      map.InPage(map.lists[item.list].container)) {
    return 1;
  }
  if (item.on_screen) {
    return 2;
  }
  if (item.kind == ItemKind::kMedia) {
    return 3;
  }
  if (item.kind == ItemKind::kButton) {
    return 4;
  }
  return 5;
}

}  // namespace

PageMapInput::PageMapInput() = default;
PageMapInput::~PageMapInput() = default;
PageMapInput::PageMapInput(const PageMapInput&) = default;
PageMapInput& PageMapInput::operator=(const PageMapInput&) = default;

PageMapObservation::PageMapObservation() = default;
PageMapObservation::~PageMapObservation() = default;
PageMapObservation::PageMapObservation(PageMapObservation&&) = default;
PageMapObservation& PageMapObservation::operator=(PageMapObservation&&) =
    default;

std::string_view KindName(ItemKind kind) {
  switch (kind) {
    case ItemKind::kLink:
      return "link";
    case ItemKind::kButton:
      return "button";
    case ItemKind::kTextField:
      return "input";
    case ItemKind::kCheckBox:
      return "checkbox";
    case ItemKind::kRadio:
      return "radio";
    case ItemKind::kSwitch:
      return "switch";
    case ItemKind::kSelect:
      return "select";
    case ItemKind::kOption:
      return "option";
    case ItemKind::kMenuItem:
      return "menuitem";
    case ItemKind::kTab:
      return "tab";
    case ItemKind::kTreeItem:
      return "treeitem";
    case ItemKind::kSlider:
      return "slider";
    case ItemKind::kMedia:
      return "media";
    case ItemKind::kClickable:
      return "clickable";
  }
  return "clickable";
}

agent_safety::FieldKind FieldKindOf(voice::CredentialKind kind) {
  switch (kind) {
    case voice::CredentialKind::kNone:
      return agent_safety::FieldKind::kOrdinary;
    case voice::CredentialKind::kPassword:
      return agent_safety::FieldKind::kPassword;
    case voice::CredentialKind::kPaymentCard:
      return agent_safety::FieldKind::kPaymentCard;
    case voice::CredentialKind::kOneTimeCode:
      return agent_safety::FieldKind::kOneTimeCode;
  }
  return agent_safety::FieldKind::kPassword;
}

PageMapObservation BuildObservation(const PageMap& map,
                                    const PageMapInput& input) {
  PageMapObservation out;
  out.observation.from_page_map = true;
  // Nothing of a map whose page isn't among the trees of its origin.
  const bool page_listed =
      map.page_tree >= 0 && InTrees(input.same_origin_trees, map.page_tree);

  // The section names ("HEADINGS:", "LISTS:") come out of the budget too.
  size_t budget = kMaxTextChars - 17;
  std::string header =
      "URL: " + PromptText(input.url, kMaxUrlChars) +
      "\nTitle: " + PromptText(map.title, kMaxTitleChars) +
      "\nSOURCE: the page's accessibility tree (element ids are its item "
      "numbers)\n";
  budget -= std::min(budget, voice::CountCodePoints(header));

  // ---- Elements -----------------------------------------------------------
  std::vector<std::tuple<int, int, int, const PageItem*>> candidates;
  if (page_listed) {
    for (const PageItem& item : map.items) {
      if (item.disabled || item.number <= 0 ||
          !InTrees(input.same_origin_trees, item.handle.tree)) {
        continue;
      }
      const int tier = Tier(item, map);
      // Entries by list rank, then position; everything else by number.
      const int order = tier == 1 ? item.list * 1000 + item.entry : 0;
      candidates.emplace_back(tier, order, item.number, &item);
    }
  }
  std::sort(candidates.begin(), candidates.end());
  std::vector<std::pair<ObservedElement, const PageItem*>> chosen;
  size_t elements_budget = std::min(kMaxElementsChars, budget);
  for (const auto& candidate : candidates) {
    if (chosen.size() >= agent_actions::kMaxObservedElements) {
      break;
    }
    const PageItem& item = *std::get<3>(candidate);
    ObservedElement element = ElementFor(item, input);
    const size_t length = voice::CountCodePoints(ElementLine(element, item));
    if (length > elements_budget) {
      continue;  // a shorter one may still fit
    }
    elements_budget -= length;
    chosen.emplace_back(std::move(element), &item);
  }
  std::sort(chosen.begin(), chosen.end(), [](const auto& a, const auto& b) {
    return a.first.item_number < b.first.item_number;
  });
  std::string elements = "ELEMENTS:\n";
  std::vector<int> listed_numbers;
  for (auto& [element, item] : chosen) {
    elements += ElementLine(element, *item);
    listed_numbers.push_back(element.item_number);
    out.observation.elements.push_back(std::move(element));
  }
  if (chosen.empty()) {
    elements += "(none)\n";
  }
  budget -= std::min(budget, voice::CountCodePoints(elements));

  // ---- Lists, the page's own (the map puts them first) --------------------
  std::string lists;
  size_t lists_shown = 0;
  bool own_list = false;
  if (page_listed) {
    for (size_t l = 0; l < map.lists.size() && lists_shown < kMaxLists; ++l) {
      const voice::PageList& list = map.lists[l];
      if (!InTrees(input.same_origin_trees, list.container.tree) ||
          list.entries.empty()) {
        continue;
      }
      own_list = own_list || map.InPage(list.container);
      std::string block =
          "List " + std::to_string(lists_shown + 1) +
          (list.label.empty() ? std::string()
                              : " \"" + PromptText(list.label, 80) + "\"") +
          ", " + std::to_string(list.entries.size()) +
          (list.entries.size() == 1 ? " entry" : " entries") +
          (list.in_navigation ? " (navigation)" : "") + ":\n";
      if (!AppendWithin(&lists, block, &budget)) {
        break;
      }
      ++lists_shown;
      size_t shown = 0;
      for (const voice::PageListEntry& entry : list.entries) {
        if (shown >= kMaxEntriesPerList ||
            !AppendWithin(&lists, EntryLine(entry, listed_numbers), &budget)) {
          break;
        }
        ++shown;
      }
      if (shown < list.entries.size()) {
        AppendWithin(
            &lists,
            "  (" + std::to_string(list.entries.size() - shown) + " more)\n",
            &budget);
      }
    }
  }

  // ---- Headings, the page's own -------------------------------------------
  std::string headings;
  size_t headings_shown = 0;
  bool own_heading = false;
  if (page_listed) {
    for (const voice::PageHeading& heading : map.headings) {
      if (headings_shown >= kMaxHeadings) {
        break;
      }
      if (!InTrees(input.same_origin_trees, heading.handle.tree)) {
        continue;
      }
      const std::string text = PromptText(heading.text, kMaxHeadingChars);
      if (text.empty()) {
        continue;
      }
      const int level = std::clamp(heading.level, 1, 6);
      if (!AppendWithin(&headings,
                        "h" + std::to_string(level) + " " + text + "\n",
                        &budget)) {
        break;
      }
      own_heading = own_heading || map.InPage(heading.handle);
      ++headings_shown;
    }
  }

  // ---- The page's text, in what is left -----------------------------------
  std::string text;
  if (page_listed && budget > 8) {
    const std::string page_text =
        PromptText(input.page_text, std::min(kMaxPageTextChars, budget - 8));
    if (!page_text.empty()) {
      AppendWithin(&text, "TEXT:\n" + page_text + "\n", &budget);
    }
  }

  out.text = header;
  if (!headings.empty()) {
    out.text += "HEADINGS:\n" + headings;
  }
  if (!lists.empty()) {
    out.text += "LISTS:\n" + lists;
  }
  out.text += text + elements;
  out.usable = page_listed &&
               (!out.observation.elements.empty() || own_list || own_heading);
  return out;
}

const PageItem* FindLiveItem(const ObservedElement& element,
                             const PageMap& fresh) {
  if (!element.from_page_map()) {
    return nullptr;
  }
  const voice::PageNodeHandle handle{element.node_tree, element.node_id};
  const PageItem* item = fresh.ItemForHandle(handle);
  if (!item || item->disabled || KindName(item->kind) != element.kind ||
      PromptText(item->label, agent_actions::kMaxElementLabelChars) !=
          element.label) {
    return nullptr;
  }
  return item;
}

agent_safety::ElementProbe ProbeNode(const std::vector<voice::PageNode>& chain,
                                     int form_index) {
  agent_safety::ElementProbe probe;
  if (chain.empty()) {
    return probe;
  }
  probe.found = true;
  const voice::PageNode& node = chain[0];
  probe.field_kind = FieldKindOf(voice::ClassifyCredential(node));
  for (const voice::PageNode& link : chain) {
    if (link.role == voice::PageRole::kLink && !link.url.empty()) {
      probe.navigates_to = link.url;
      break;
    }
  }
  for (size_t i = 0; i < chain.size(); ++i) {
    const std::string tag = LowerAscii(chain[i].html_tag, 16);
    if (tag != "button" && tag != "input") {
      continue;
    }
    // As the snapshot's el.closest('button,input'): the nearest decides.
    const std::string type = TrimAscii(LowerAscii(chain[i].input_type, 16));
    const bool submit_input =
        tag == "input" && (type == "submit" || type == "image");
    const bool submit = tag == "button" || submit_input;
    // Outside any form, a submit input can only submit one it is tied to
    // with form="id", which the tree doesn't show.
    probe.submits_form =
        submit && ((form_index >= 0 && static_cast<size_t>(form_index) > i) ||
                   (submit_input && form_index < 0));
    break;
  }
  probe.label = voice::SpeechSafeText(node.name, kMaxProbeLabelChars);
  return probe;
}

ClickQuestion::ClickQuestion() = default;
ClickQuestion::~ClickQuestion() = default;
ClickQuestion::ClickQuestion(ClickQuestion&&) = default;
ClickQuestion& ClickQuestion::operator=(ClickQuestion&&) = default;

ClickQuestion CheckClick(const agent_safety::ElementProbe& probe,
                         const std::vector<url::Origin>& allowed,
                         std::string_view current_origin_text) {
  ClickQuestion question;
  const GURL dest(probe.navigates_to);
  if (agent_safety::IsAllowedNavigationURL(dest)) {
    const url::Origin origin = url::Origin::Create(dest);
    const bool in_scope = std::any_of(
        allowed.begin(), allowed.end(),
        [&](const url::Origin& o) { return o.IsSameOriginWith(origin); });
    if (!in_scope) {
      question.effects.push_back("opens " + origin.Serialize());
      question.new_origin = origin;
    }
  }
  if (probe.submits_form) {
    // Where the form's data goes, when the probe knows it (the element
    // snapshot's reads the form's action) and it is another origin.
    const std::string to = agent_safety::IsAllowedNavigationURL(dest)
                               ? url::Origin::Create(dest).Serialize()
                               : std::string();
    question.effects.push_back(
        !to.empty() && to != current_origin_text
            ? "submits a form to " + to
            : "submits a form on " + std::string(current_origin_text));
  }
  return question;
}

std::string QuotedLabel(std::string_view label) {
  // Quote and dash characters (ASCII and UTF-8) as the plain ones that
  // can't end the quote or start the browser's " — it ...".
  static constexpr std::pair<std::string_view, std::string_view> kSwaps[] = {
      {"\"", "'"},
      {"\xE2\x80\x9C", "'"},  // U+201C
      {"\xE2\x80\x9D", "'"},  // U+201D
      {"\xE2\x80\x9E", "'"},  // U+201E
      {"\xE2\x80\x9F", "'"},  // U+201F
      {"\xE2\x80\xB3", "'"},  // U+2033 double prime
      {"\xC2\xAB", "'"},      // U+00AB
      {"\xC2\xBB", "'"},      // U+00BB
      {"\xEF\xBC\x82", "'"},  // U+FF02 fullwidth quotation mark
      {"\xE2\x80\x92", "-"},  // U+2012 figure dash
      {"\xE2\x80\x93", "-"},  // U+2013 en dash
      {"\xE2\x80\x94", "-"},  // U+2014 em dash
      {"\xE2\x80\x95", "-"},  // U+2015 horizontal bar
      {"\xE2\x88\x92", "-"},  // U+2212 minus sign
  };
  std::string out = "\"";
  size_t i = 0;
  while (i < label.size()) {
    bool swapped = false;
    for (const auto& [from, to] : kSwaps) {
      if (label.substr(i, from.size()) == from) {
        out.append(to);
        i += from.size();
        swapped = true;
        break;
      }
    }
    if (!swapped) {
      out.push_back(label[i]);
      ++i;
    }
  }
  out.push_back('"');
  return out;
}

FrameCheck CheckFrameOrigin(std::string_view observed,
                            const url::Origin& live,
                            const url::Origin& page,
                            const std::vector<url::Origin>& allowed,
                            bool framed_by_own_origin) {
  const bool web = !live.opaque() && (live.scheme() == url::kHttpScheme ||
                                      live.scheme() == url::kHttpsScheme);
  const bool in_scope =
      std::any_of(allowed.begin(), allowed.end(),
                  [&](const url::Origin& o) { return o.IsSameOriginWith(live); });
  // A frame of the page's origin inside another site's frame shows what
  // that site chose (any of the page's URLs), not the page.
  if (!web || !in_scope || !live.IsSameOriginWith(page) ||
      !framed_by_own_origin) {
    return FrameCheck::kRefused;
  }
  return live.Serialize() == observed ? FrameCheck::kOk : FrameCheck::kStale;
}

TypeCheck CheckType(const PageItem& live) {
  switch (live.credential) {
    case voice::CredentialKind::kPassword:
      return TypeCheck::kRefusePassword;
    case voice::CredentialKind::kPaymentCard:
    case voice::CredentialKind::kOneTimeCode:
      return TypeCheck::kRefuseCredential;
    case voice::CredentialKind::kNone:
      break;
  }
  if (live.kind != ItemKind::kTextField || !live.writable || live.disabled) {
    return TypeCheck::kNotTextField;
  }
  return TypeCheck::kType;
}

std::string_view TypeRefusalNote(TypeCheck check) {
  switch (check) {
    case TypeCheck::kType:
      return "";
    case TypeCheck::kRefusePassword:
      return "Refused: the agent never types into password fields. Ask the "
             "user to sign in themselves, then continue.";
    case TypeCheck::kRefuseCredential:
      return "Refused: reading the page through its accessibility tree, the "
             "agent never types into payment card or one-time code fields. "
             "Ask the user to enter it themselves, then continue.";
    case TypeCheck::kNotTextField:
      return "Not typed: the element is no longer a text field the agent "
             "can write.";
  }
  return "";
}

}  // namespace agent_page_map
}  // namespace molt_ai
