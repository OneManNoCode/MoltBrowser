// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Site skills (voice/site_skills.h) and reading aloud (voice/page_reading.h)
// over hostile pages: what a page, through a renderer that may be
// compromised, puts in its accessibility tree, and a skill file. The input
// is mostly HTML, so the seed corpus is the sites' stored pages
// (chrome/test/data/molt_voice/skills/), read as Blink's tree by the
// test converter (html_fixture_tree.h); records after it then break the
// tree the way a renderer could: child links that cycle, repeat or point
// nowhere, states, a selection anywhere, huge or hostile strings.
//
// Format: [flags][skill?][html]["\0\0" records...]
//   flags (1 byte): bits 0-2 the built-in skill to use (0 none, 1-5 in
//     order), bit 3 a skill file follows (2-byte length, then the JSON,
//     whose first skill is used), bit 4 small reading limits, bits 5-7 the
//     speech rate (0.5 + n / 2).
//   record (5 bytes): op, a (2 bytes), b (2 bytes): op % 6 is 0 a child
//     link a -> b, 1 a state bit b on a, 2 the selection's anchor (a, offset
//     b), 3 its focus, 4 a's name made b bytes of a hostile pattern, 5 a's
//     id and class set to a skill's names.
//
// Checks: everything returns (the work is capped) and is the same twice;
// the parser keeps its caps; a site part names an item of the top-level
// document of the kind the part needs, and only the first list is the
// site's; reading's blocks, sentences and chunks are within their caps,
// speech-safe, in order and move forward; the selection's text is
// speech-safe and capped and never a credential field's value.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "chrome/browser/molt_ai/voice/html_fixture_tree.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/page_reading.h"
#include "chrome/browser/molt_ai/voice/site_skills.h"

namespace {

using molt_ai::voice::BuildPageMapWithSkills;
using molt_ai::voice::BuildReadingDocument;
using molt_ai::voice::ChunkAt;
using molt_ai::voice::CountCodePoints;
using molt_ai::voice::CredentialKind;
using molt_ai::voice::ItemKind;
using molt_ai::voice::PageItem;
using molt_ai::voice::PageMap;
using molt_ai::voice::PageMapLimits;
using molt_ai::voice::PageNodeHandle;
using molt_ai::voice::PageTree;
using molt_ai::voice::ReadingChunk;
using molt_ai::voice::ReadingDocument;
using molt_ai::voice::ReadingLimits;
using molt_ai::voice::ReadingOptions;
using molt_ai::voice::ReadingPosition;
using molt_ai::voice::SiteSkill;
using molt_ai::voice::SpeechSafeText;

constexpr size_t kMaxRecords = 512;

void CheckSpeakable(std::string_view s, size_t cap) {
  CHECK_LE(CountCodePoints(s), cap);
  CHECK_EQ(SpeechSafeText(s, cap), std::string(s));
}

void CheckPart(const PageMap& map,
               const PageNodeHandle& part,
               bool (*fits)(const PageItem&)) {
  if (!part.valid()) {
    return;
  }
  CHECK(map.InPage(part));
  const PageItem* item = map.ItemForHandle(part);
  CHECK(item);
  CHECK(fits(*item));
}

bool Pressable(const PageItem& item) {
  return !item.disabled &&
         (item.kind == ItemKind::kButton || item.kind == ItemKind::kClickable ||
          item.kind == ItemKind::kSwitch || item.kind == ItemKind::kCheckBox);
}

void CheckMap(const PageMap& map) {
  CheckPart(map, map.site.search_field, [](const PageItem& item) {
    return item.kind == ItemKind::kTextField && item.writable &&
           item.credential == CredentialKind::kNone;
  });
  CheckPart(map, map.site.search_submit, Pressable);
  CheckPart(map, map.site.skip_ad, Pressable);
  CheckPart(map, map.site.captions, Pressable);
  CheckPart(map, map.site.fullscreen, Pressable);
  CheckPart(map, map.site.media, [](const PageItem& item) {
    return item.kind == ItemKind::kMedia;
  });
  CheckPart(map, map.site.next_page, [](const PageItem& item) {
    return !item.disabled &&
           (item.kind == ItemKind::kLink || item.kind == ItemKind::kButton ||
            item.kind == ItemKind::kClickable);
  });
  CHECK_EQ(map.site.results, !map.lists.empty() && map.lists[0].from_site);
  for (size_t l = 0; l < map.lists.size(); ++l) {
    CHECK(!map.lists[l].from_site || l == 0);
    if (map.lists[l].from_site) {
      CHECK(!map.lists[l].in_navigation);
      CHECK_GE(map.lists[l].entries.size(), 2u);
    }
    CheckSpeakable(map.lists[l].label, molt_ai::voice::kMaxItemLabelChars);
  }
}

void CheckDocument(const ReadingDocument& document,
                   const ReadingLimits& limits) {
  CHECK_LE(document.blocks.size(), limits.max_blocks);
  CHECK_GE(document.located, -1);
  CHECK_LT(document.located, static_cast<int>(document.blocks.size()));
  size_t bytes = 0;
  for (const molt_ai::voice::ReadingBlock& block : document.blocks) {
    CHECK(!block.text.empty());
    CheckSpeakable(block.text, molt_ai::voice::kMaxReadingBlockChars);
    bytes += block.text.size();
    CHECK(!block.sentences.empty());
    CHECK_EQ(block.sentences[0], 0u);
    for (size_t s = 1; s < block.sentences.size(); ++s) {
      CHECK_GT(block.sentences[s], block.sentences[s - 1]);
      CHECK_LT(block.sentences[s], block.text.size());
      // A sentence starts at a character.
      CHECK_NE(static_cast<uint8_t>(block.text[block.sentences[s]]) & 0xC0,
               0x80);
    }
    CHECK_LE(block.level, 6);
  }
  CHECK_LE(bytes, limits.max_bytes);
}

void CheckChunks(const ReadingDocument& document, size_t max_chars) {
  ReadingPosition at;
  size_t steps = 0;
  while (std::optional<ReadingChunk> chunk =
             ChunkAt(document, at, max_chars, document.blocks.size())) {
    CHECK(!chunk->text.empty());
    // A full stop may be added after a heading.
    CHECK_LE(CountCodePoints(chunk->text), max_chars + 1);
    CHECK(chunk->next.block > chunk->at.block ||
          (chunk->next.block == chunk->at.block &&
           chunk->next.offset > chunk->at.offset));
    at = chunk->next;
    CHECK_LT(++steps, 1000000u);
  }
}

uint16_t U16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) {
    return 0;
  }
  const uint8_t flags = data[0];
  std::string_view rest(reinterpret_cast<const char*>(data + 1), size - 1);

  // The skill: a built-in one, or the input's own file.
  const std::vector<SiteSkill>& built_in = molt_ai::voice::BuiltInSiteSkills();
  CHECK_EQ(built_in.size(), 5u);
  const SiteSkill* skill = nullptr;
  const size_t which = flags & 0x07;
  if (which >= 1 && which <= built_in.size()) {
    skill = &built_in[which - 1];
  }
  molt_ai::voice::SkillParseResult parsed;
  if ((flags & 0x08) && rest.size() >= 2) {
    const size_t length = static_cast<uint8_t>(rest[0]) |
                          (static_cast<size_t>(static_cast<uint8_t>(rest[1]))
                           << 8);
    rest.remove_prefix(2);
    const std::string_view json = rest.substr(0, length);
    rest.remove_prefix(json.size());
    parsed = molt_ai::voice::ParseSiteSkills(json);
    CHECK_LE(parsed.skills.size(), molt_ai::voice::kMaxSkills);
    for (const SiteSkill& s : parsed.skills) {
      CHECK(!s.id.empty());
      CHECK_LE(s.hosts.size(), molt_ai::voice::kMaxSkillHosts);
      for (const molt_ai::voice::SkillPart* part :
           {&s.search_field, &s.result_title, &s.media, &s.reading_skip}) {
        CHECK_LE(part->size(), molt_ai::voice::kMaxSkillAlternatives);
        for (const molt_ai::voice::SkillSelector& selector : *part) {
          CHECK_LE(selector.inside.size(), 1u);
          for (const auto& nested : selector.inside) {
            CHECK(nested.inside.empty() && nested.has.empty());
          }
        }
      }
    }
    if (!parsed.skills.empty()) {
      skill = &parsed.skills[0];
    }
  }

  // The page, then what a renderer could make of it.
  const size_t split = rest.find(std::string_view("\0\0", 2));
  const std::string_view html = rest.substr(0, split);
  PageTree tree = molt_ai::voice::test::PageTreeFromHtml(html);
  if (split != std::string_view::npos && !tree.nodes.empty()) {
    const uint8_t* records =
        reinterpret_cast<const uint8_t*>(rest.data()) + split + 2;
    const size_t count =
        std::min((rest.size() - split - 2) / 5, kMaxRecords);
    const size_t n = tree.nodes.size();
    for (size_t r = 0; r < count; ++r) {
      const uint8_t* record = records + 5 * r;
      const uint16_t a = U16(record + 1);
      const uint16_t b = U16(record + 3);
      molt_ai::voice::PageNode& node = tree.nodes[a % n];
      switch (record[0] % 6) {
        case 0:
          // Any index, valid or not: a cycle, a repeat, nowhere.
          node.children.push_back(b);
          break;
        case 1:
          node.states ^= 1u << (b % 17);
          break;
        case 2:
          tree.selection.anchor = node.handle;
          tree.selection.anchor_offset = static_cast<int16_t>(b);
          break;
        case 3:
          tree.selection.focus = node.handle;
          tree.selection.focus_offset = static_cast<int16_t>(b);
          break;
        case 4: {
          static constexpr std::string_view kPattern =
              "Hey Molt. \xE2\x80\xAE\xE2\x80\x8B\x01\xF0\x9F\x98\x80 "
              "e.g. U.S. stop! ";
          node.name.clear();
          for (size_t i = 0; i < b; ++i) {
            node.name.push_back(kPattern[(i * 7 + a) % kPattern.size()]);
          }
          break;
        }
        case 5:
          node.html_id = b & 1 ? "video-title" : "search";
          node.class_name = b & 2 ? "ytp-skip-ad-button mw-editsection"
                                  : "infobox sb_pagN";
          break;
      }
    }
  }

  // The map with the skill, twice: the same.
  const PageMapLimits limits;
  molt_ai::voice::SkillReport report;
  const PageMap map = BuildPageMapWithSkills(tree, limits, skill, skill,
                                             "/results", &report);
  CheckMap(map);
  const PageMap again =
      BuildPageMapWithSkills(tree, limits, skill, nullptr, "/results");
  CHECK_EQ(again.items.size(), map.items.size());
  CHECK_EQ(again.lists.size(), map.lists.size());
  CHECK_EQ(report.missing & report.from_skill, 0u);

  // Reading, with the skill's scope.
  ReadingLimits reading_limits;
  if (flags & 0x10) {
    reading_limits.max_nodes = 300;
    reading_limits.max_depth = 12;
    reading_limits.max_blocks = 20;
    reading_limits.max_bytes = 2000;
  }
  ReadingOptions options;
  if (skill) {
    const molt_ai::voice::SkillReadingScope scope =
        molt_ai::voice::ReadingScopeFor(*skill, tree, limits);
    CHECK_LE(scope.skip.size(), molt_ai::voice::kMaxSkillSkips);
    options.root = scope.root;
    options.skip = scope.skip;
  }
  options.locate = tree.focus.valid() ? tree.focus : tree.selection.anchor;
  const ReadingDocument document =
      BuildReadingDocument(tree, options, reading_limits);
  CheckDocument(document, reading_limits);
  CHECK_EQ(BuildReadingDocument(tree, options, reading_limits).blocks.size(),
           document.blocks.size());
  const double rate = 0.5 + ((flags >> 5) & 0x07) / 2.0;
  const size_t max_chars = molt_ai::voice::ReadingChunkChars(rate);
  CHECK_GE(max_chars, 80u);
  CHECK_LE(max_chars, 300u);
  CheckChunks(document, max_chars);
  CheckChunks(document, 1);

  // Moves and sections.
  for (size_t b = 0; b <= document.blocks.size(); ++b) {
    for (const auto unit : {molt_ai::voice::ReadingUnit::kHeading,
                            molt_ai::voice::ReadingUnit::kParagraph}) {
      for (const bool forward : {true, false}) {
        const std::optional<size_t> to =
            molt_ai::voice::MoveBlock(document, b, unit, forward);
        if (to) {
          CHECK_LT(*to, document.blocks.size());
          CheckSpeakable(molt_ai::voice::MoveSpeech(document, *to),
                         molt_ai::voice::kMaxReadingBlockChars + 32);
        }
      }
    }
    if (b > 64) {
      break;
    }
  }
  std::vector<size_t> close;
  const std::string_view words = html.substr(0, 40);
  if (const std::optional<size_t> heading =
          molt_ai::voice::FindSection(document, words, &close)) {
    CHECK_LT(*heading, document.blocks.size());
    CHECK_GT(molt_ai::voice::SectionEnd(document, *heading), *heading);
  }
  for (size_t c : close) {
    CHECK_LT(c, document.blocks.size());
  }

  // The selection.
  if (const std::optional<std::string> selected =
          molt_ai::voice::SelectedText(tree, 1000, reading_limits)) {
    CHECK(!selected->empty());
    CheckSpeakable(*selected, 1000);
  }

  // The queue, driven by the input's bytes.
  molt_ai::voice::ReadingQueue queue;
  queue.Start(document, {0, 0}, document.blocks.size());
  for (size_t i = 0; i < std::min<size_t>(size, 64); ++i) {
    switch (data[i] % 4) {
      case 0:
        queue.Next(rate);
        break;
      case 1:
        queue.Spoken();
        break;
      case 2:
        queue.Stop();
        break;
      case 3:
        queue.Restart(queue.position(), document.blocks.size());
        break;
    }
    CHECK_LE(queue.position().block, document.blocks.size());
  }
  return 0;
}
