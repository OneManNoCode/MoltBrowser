// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_reader.h"

#include <optional>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/site_skills.h"
#include "chrome/browser/molt_ai/voice/voice_speech_text.h"

namespace molt_ai {
namespace voice {

namespace {

// Headings listed when no section has the name asked for.
constexpr size_t kMaxSectionsListed = 8;

}  // namespace

VoiceReader::VoiceReader(Host* host) : host_(host) {}

VoiceReader::~VoiceReader() = default;

void VoiceReader::Run(ReadOp op,
                      std::string_view section,
                      int64_t page_id,
                      double rate) {
  rate_ = rate;
  shown_page_id_ = page_id;
  const bool was_reading = was_reading_;
  was_reading_ = false;
  const bool same_page = queue_.loaded() && page_id_ == page_id;
  switch (op) {
    case ReadOp::kPage:
      if (Load(page_id, PageNodeHandle(), /*reuse=*/false)) {
        ReadFrom({0, 0}, queue_.document().blocks.size(), What::kPage);
      }
      return;

    case ReadOp::kFromHere: {
      // From the focused item when the focus moved since the reading
      // started; else where it stopped.
      const PageTree tree = host_->Tree();
      const bool moved = tree.focus.valid() && !(tree.focus == last_focus_);
      if (same_page && what_ != What::kSelection && !moved &&
          !queue_.finished()) {
        ReadFrom(queue_.position(), queue_.document().blocks.size(),
                 What::kPage);
        return;
      }
      if (tree.nodes.empty()) {
        Say("There's no text to read on this page.");
        return;
      }
      ReadingDocument document = Build(tree, tree.focus);
      if (document.blocks.empty()) {
        Say("There's no text to read on this page.");
        return;
      }
      const size_t from =
          document.located >= 0 ? static_cast<size_t>(document.located) : 0;
      const size_t end = document.blocks.size();
      queue_.Start(std::move(document), {from, 0}, end);
      queue_.set_current_block(-1);
      page_id_ = page_id;
      last_focus_ = tree.focus;
      ReadFrom({from, 0}, end, What::kPage);
      return;
    }

    case ReadOp::kResume:
      if (!same_page) {
        Say("There's nothing to go on with here. Say read the page.");
        return;
      }
      if (queue_.finished()) {
        Say("That was the end. Say read the page to hear it again.");
        return;
      }
      ReadFrom(queue_.position(), queue_.end_block(), what_);
      return;

    case ReadOp::kSelection: {
      const PageTree tree = host_->Tree();
      const std::optional<std::string> text =
          SelectedText(tree, kMaxSelectionChars, ReadingLimits());
      if (!text) {
        Say("Nothing is selected. Select some text, or say read the page.");
        return;
      }
      queue_.Start(TextDocument(*text), {0, 0}, 1);
      page_id_ = page_id;
      ReadFrom({0, 0}, 1, What::kSelection);
      return;
    }

    case ReadOp::kSection:
      Section(section, page_id);
      return;

    case ReadOp::kNextHeading:
    case ReadOp::kPreviousHeading:
    case ReadOp::kNextParagraph:
    case ReadOp::kPreviousParagraph: {
      const bool heading = op == ReadOp::kNextHeading ||
                           op == ReadOp::kPreviousHeading;
      const bool forward =
          op == ReadOp::kNextHeading || op == ReadOp::kNextParagraph;
      was_reading_ = was_reading;
      Move(heading ? ReadingUnit::kHeading : ReadingUnit::kParagraph, forward,
           page_id);
      was_reading_ = false;
      return;
    }
  }
}

bool VoiceReader::Load(int64_t page_id,
                       const PageNodeHandle& locate,
                       bool reuse) {
  if (reuse && queue_.loaded() && page_id_ == page_id &&
      what_ != What::kSelection) {
    return true;
  }
  const PageTree tree = host_->Tree();
  ReadingDocument document = Build(tree, locate);
  if (document.blocks.empty()) {
    Say("There's no text to read on this page.");
    return false;
  }
  const size_t end = document.blocks.size();
  queue_.Start(std::move(document), {0, 0}, end);
  queue_.set_current_block(-1);
  page_id_ = page_id;
  last_focus_ = tree.focus;
  what_ = What::kPage;
  return true;
}

ReadingDocument VoiceReader::Build(const PageTree& tree,
                                   const PageNodeHandle& locate) {
  ReadingOptions options;
  if (const SiteSkill* skill = host_->Skill()) {
    const SkillReadingScope scope =
        ReadingScopeFor(*skill, tree, PageMapLimits());
    options.root = scope.root;
    options.skip = scope.skip;
  }
  options.locate = locate;
  return BuildReadingDocument(tree, options, ReadingLimits());
}

void VoiceReader::ReadFrom(ReadingPosition from, size_t end_block, What what) {
  queue_.Restart(from, end_block);
  what_ = what;
  ids_.clear();
  scrolled_block_ = -1;
  Feed();
  if (ids_.empty()) {
    Say(what == What::kSection ? "That's the end of the section."
                               : "That's the end of the page.");
  }
}

void VoiceReader::Move(ReadingUnit unit, bool forward, int64_t page_id) {
  if (!Load(page_id, PageNodeHandle(), /*reuse=*/true)) {
    return;
  }
  const ReadingDocument& document = queue_.document();
  const size_t n = document.blocks.size();
  // From the block read last; nothing read yet: from the top.
  size_t from = queue_.current_block() >= 0
                    ? static_cast<size_t>(queue_.current_block())
                    : n;
  if (!forward && from == n) {
    from = 0;
  }
  const std::optional<size_t> to = MoveBlock(document, from, unit, forward);
  const bool heading = unit == ReadingUnit::kHeading;
  if (!to) {
    bool any = false;
    for (const ReadingBlock& block : document.blocks) {
      any = any || (block.kind == ReadingBlockKind::kHeading) == heading;
    }
    Say(!any ? (heading ? "This page has no headings."
                        : "This page has no paragraphs.")
        : forward ? (heading ? "That's the last heading."
                             : "That's the last paragraph.")
                  : (heading ? "That's the first heading."
                             : "That's the first paragraph."));
    return;
  }
  queue_.set_current_block(static_cast<int>(*to));
  host_->ScrollTo(document.blocks[*to].node);
  const bool reading = was_reading_;
  if (heading) {
    Say(MoveSpeech(document, *to));
    // The place is past the heading: reading goes on with what follows.
    const ReadingPosition after{*to, document.blocks[*to].text.size()};
    if (reading) {
      ReadFrom(after, n, What::kPage);
    } else {
      queue_.Restart(after, n);
      what_ = What::kPage;
    }
    return;
  }
  // A paragraph is read; on to the end when it was reading.
  ReadFrom({*to, 0}, reading ? n : *to + 1,
           reading ? What::kPage : What::kParagraph);
}

void VoiceReader::Section(std::string_view name, int64_t page_id) {
  if (!Load(page_id, PageNodeHandle(), /*reuse=*/true)) {
    return;
  }
  const ReadingDocument& document = queue_.document();
  std::vector<size_t> close;
  const std::optional<size_t> heading = FindSection(document, name, &close);
  if (heading) {
    host_->ScrollTo(document.blocks[*heading].node);
    queue_.set_current_block(static_cast<int>(*heading));
    ReadFrom({*heading, 0}, SectionEnd(document, *heading), What::kSection);
    return;
  }
  std::vector<size_t> listed = close;
  std::string said;
  if (!close.empty()) {
    said = "More than one section fits: ";
  } else {
    for (size_t b = 0; b < document.blocks.size() &&
                       listed.size() < kMaxSectionsListed;
         ++b) {
      if (document.blocks[b].kind == ReadingBlockKind::kHeading) {
        listed.push_back(b);
      }
    }
    if (listed.empty()) {
      Say("This page has no sections.");
      return;
    }
    said = "There's no section called " +
           SpeechSafeText(name, kMaxSpokenPageTextChars) + ". The sections are: ";
  }
  for (size_t i = 0; i < listed.size(); ++i) {
    said += (i ? ", " : "") +
            SpeechSafeText(document.blocks[listed[i]].text,
                           kMaxSpokenPageTextChars);
  }
  Say(said + ". Say read the section on, then its name.");
}

void VoiceReader::Feed() {
  if (page_id_ != shown_page_id_) {
    return;  // another page is shown: not this one's text
  }
  while (std::optional<ReadingChunk> chunk = queue_.Next(rate_)) {
    const uint64_t id = host_->Speak(chunk->text, rate_);
    if (!id) {
      queue_.Stop();
      ids_.clear();
      return;
    }
    ids_.push_back(id);
    if (ids_.size() == 1) {
      ChunkStarted();
    }
  }
}

void VoiceReader::ChunkStarted() {
  const ReadingChunk* chunk = queue_.current();
  if (!chunk) {
    return;
  }
  host_->Said(chunk->text);
  host_->Status("Reading: " + SpeechSafeText(chunk->text, kMaxStatusChars));
  const int block = static_cast<int>(chunk->at.block);
  if (block != scrolled_block_ && what_ != What::kSelection &&
      chunk->at.block < queue_.document().blocks.size()) {
    scrolled_block_ = block;
    host_->ScrollTo(queue_.document().blocks[chunk->at.block].node);
  }
}

void VoiceReader::OnUtteranceDone(uint64_t id, bool spoken) {
  if (ids_.empty() || ids_.front() != id) {
    return;  // not a chunk (an answer), or one dropped already
  }
  if (!spoken) {
    // Something else stopped the voice: the place is kept.
    queue_.Stop();
    ids_.clear();
    return;
  }
  ids_.pop_front();
  queue_.Spoken();
  if (page_id_ != shown_page_id_) {
    queue_.Stop();
    ids_.clear();
    return;
  }
  if (!ids_.empty()) {
    ChunkStarted();
  }
  Feed();
  if (ids_.empty() && queue_.finished()) {
    host_->Status("Done reading");
    switch (what_) {
      case What::kPage:
        Say("That's the end of the page.");
        break;
      case What::kSection:
        Say("That's the end of the section.");
        break;
      case What::kParagraph:
      case What::kSelection:
        break;
    }
    // After a section or a paragraph, "continue reading" goes on with the
    // rest of the page.
    if (what_ == What::kSection || what_ == What::kParagraph) {
      queue_.Restart(queue_.position(), queue_.document().blocks.size());
      what_ = What::kPage;
    }
  }
}

void VoiceReader::Interrupt() {
  was_reading_ = !ids_.empty();
  queue_.Stop();
  ids_.clear();
}

void VoiceReader::PageChanged(int64_t page_id) {
  shown_page_id_ = page_id;
}

void VoiceReader::Clear() {
  queue_.Clear();
  ids_.clear();
  page_id_ = -1;
  was_reading_ = false;
  last_focus_ = PageNodeHandle();
  scrolled_block_ = -1;
}

VoiceEnvironment::Reading VoiceReader::StateFor(int64_t page_id) const {
  using Reading = VoiceEnvironment::Reading;
  if (!queue_.loaded() || page_id != page_id_) {
    return Reading::kNone;
  }
  if (was_reading_ || !ids_.empty()) {
    return Reading::kReading;
  }
  return queue_.finished() ? Reading::kNone : Reading::kStopped;
}

void VoiceReader::Say(const std::string& text) {
  const std::string said = SpeechSafeText(text, 2 * kMaxSpeechChunkChars);
  if (said.empty()) {
    return;
  }
  host_->Said(said);
  host_->Speak(said, rate_);
}

}  // namespace voice
}  // namespace molt_ai
