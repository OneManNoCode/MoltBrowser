// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Reading aloud in voice mode (docs/design/TOUCHLESS_BROWSING.md §5.4,
// §6.6): carries out the voice loop's kRead steps (voice_controller_
// logic.h) with the voice, a chunk at a time (page_reading.h).
//
//   - "Read the page": the page's text from its start (the site skill's
//     content root, else the main landmark, the first article, the page).
//   - "Read from here": from the focused item when the focus moved since
//     the last reading, else where reading stopped.
//   - "Continue reading" (and "continue" after "pause"): where it stopped.
//   - "Read the selection": the selected text.
//   - "Read the section on history": that heading's section, to its end.
//   - "Next heading", "previous heading": the heading is said, and reading
//     goes on from it when it was reading; "next paragraph" and "previous
//     paragraph" read that paragraph (and go on, when it was reading).
//
// The voice holds the chunk being said and one more (kReadingAhead), so
// "stop", barge-in and a new command stop it at once; stopping keeps the
// place (the first chunk not said to its end). Each chunk is passed to
// Host::Said() as it starts, so the caption shows it, the agent panel lists
// it and the listening session knows it is the browser's own words
// (voice_session_logic.h); chunks are short enough to be said within the
// time the session remembers them (ReadingChunkChars()).
//
// Page text is only ever read out (cleaned and capped in page_reading.h),
// never taken as a command. Std-only (the browser's side is the Host,
// voice_controller.cc), unit-tested in molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_READER_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_READER_H_

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_reading.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"
#include "chrome/services/molt_ai/inference/engine/raw_ptr_exclusion.h"

namespace molt_ai {
namespace voice {

struct SiteSkill;

class VoiceReader {
 public:
  // The browser's side.
  class Host {
   public:
    virtual ~Host() = default;
    // Says |text| at |rate| after what voice mode is saying; the
    // utterance's id (VoiceSpeech::Speak()), 0 if it isn't said.
    virtual uint64_t Speak(const std::string& text, double rate) = 0;
    // |text| starts being said now (a chunk, or the reader's own words).
    virtual void Said(const std::string& text) = 0;
    // The caption bar's status.
    virtual void Status(const std::string& status) = 0;
    // Scrolls |node| into view, so the screen follows the reading.
    virtual void ScrollTo(const PageNodeHandle& node) = 0;
    // The active tab's page as its tree (empty when there is none), and
    // its site's skill (null for none).
    virtual PageTree Tree() = 0;
    virtual const SiteSkill* Skill() = 0;
  };

  // What the caption shows of a chunk, in code points.
  static constexpr size_t kMaxStatusChars = 120;
  // The selection read at most, in code points.
  static constexpr size_t kMaxSelectionChars = 4000;

  // |host| outlives the reader.
  explicit VoiceReader(Host* host);
  VoiceReader(const VoiceReader&) = delete;
  VoiceReader& operator=(const VoiceReader&) = delete;
  ~VoiceReader();

  // A kRead step on the page |page_id|, at speech rate |rate|.
  void Run(ReadOp op, std::string_view section, int64_t page_id, double rate);

  // Reads |text|, page-derived words the browser says about page |page_id|
  // (an answer to a question about the page, voice_page_questions.h), as
  // it reads the selection: in chunks, each said to the Host as it starts,
  // stopped and resumed like a reading. It takes the reading's place.
  void ReadText(std::string_view text, int64_t page_id, double rate);

  // The voice said utterance |id| to its end, or dropped it.
  void OnUtteranceDone(uint64_t id, bool spoken);

  // Speech is being stopped (a new command, "stop"): reading stops and
  // keeps its place. Call before the voice stops.
  void Interrupt();

  // The active tab shows page |page_id| now: a reading of another page
  // isn't fed any more.
  void PageChanged(int64_t page_id);

  // Forgets everything (voice mode is off).
  void Clear();

  // VoiceEnvironment::reading for page |page_id|.
  VoiceEnvironment::Reading StateFor(int64_t page_id) const;

  // Chunks are with the voice.
  bool reading() const { return !ids_.empty(); }
  const ReadingQueue& queue() const { return queue_; }

 private:
  enum class What : uint8_t {
    kPage,
    kSection,
    kParagraph,
    kSelection,
  };

  // The page's document for |page_id|: the one loaded when it is the same
  // page and a page's (not the selection's), else built now (with
  // |locate|). False, having said so, when there's no text.
  bool Load(int64_t page_id, const PageNodeHandle& locate, bool reuse);
  ReadingDocument Build(const PageTree& tree, const PageNodeHandle& locate);
  void ReadFrom(ReadingPosition from, size_t end_block, What what);
  void Move(ReadingUnit unit, bool forward, int64_t page_id);
  void Section(std::string_view name, int64_t page_id);
  // Hands chunks to the voice while it holds fewer than kReadingAhead + 1.
  void Feed();
  // The chunk the voice says now has started.
  void ChunkStarted();
  void Say(const std::string& text);

  // Outlives the reader.
  MOLT_RAW_PTR_EXCLUSION Host* host_;
  ReadingQueue queue_;
  // The page the document is of, and the page shown now.
  int64_t page_id_ = -1;
  int64_t shown_page_id_ = -1;
  What what_ = What::kPage;
  double rate_ = 1.0;
  // The voice's ids of the chunks with it, oldest first.
  std::deque<uint64_t> ids_;
  // It was reading when Interrupt() stopped it (and nothing ran since).
  bool was_reading_ = false;
  // The focused node when reading last started, for "read from here".
  PageNodeHandle last_focus_;
  // The block the last chunk said started (to scroll each block in once).
  int scrolled_block_ = -1;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_READER_H_
