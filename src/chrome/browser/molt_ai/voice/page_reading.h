// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Reading a page aloud (docs/design/TOUCHLESS_BROWSING.md §5.4, §6.6): the
// page's text as blocks (headings, paragraphs, list items), split into
// sentences and spoken in bounded chunks, with a reading position that
// "pause", "stop", "continue reading", "next heading" and "next paragraph"
// move or keep.
//
// Where the text comes from: the accessibility tree's own structure, the
// way a screen reader's "say all" reads it, not Chromium's reader-mode
// distiller (VOICE_SKILLS.md says why):
//   - the site skill's content root when it names one (site_skills.h),
//     else the page's main landmark, else its first article, else the
//     whole page;
//   - inside it, what isn't text to read is left out: navigation, search,
//     forms and their controls, menus, dialogs, tables, media, frames (a
//     frame may be another site's), and, when the whole page is read, its
//     banner, footer and asides; plus the subtrees the skill leaves out
//     (edit links, reference marks, infoboxes);
//   - only the top-level document: the frames a page embeds are never read.
//
// Page text is hostile. Every text here went through SpeechSafeText()
// (no control, bidi or invisible characters) and is capped: a block, a
// sentence, the document's blocks and bytes, the nodes walked. It is only
// ever read out: the voice loop never takes it as a command (and
// voice_session_logic.h ignores what the browser heard itself say, which
// the reader keeps it able to do by sizing chunks, below). A credential
// field's value is never read, selected or not.
//
// Std-only (no //base); unit-tested in molt_ai_unittests and fuzzed by
// molt_page_reading_fuzzer.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_PAGE_READING_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_PAGE_READING_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map.h"

namespace molt_ai {
namespace voice {

// Caps on a block of text and on one sentence, in code points.
inline constexpr size_t kMaxReadingBlockChars = 4000;
inline constexpr size_t kMaxReadingSentenceChars = 300;
// Chunks handed to the voice ahead of the one it speaks.
inline constexpr size_t kReadingAhead = 1;

enum class ReadingBlockKind : uint8_t {
  kHeading,
  kParagraph,
  kListItem,
  kOther,  // text in another container (a caption, a quote, a div)
};

struct ReadingBlock {
  ReadingBlock();
  ~ReadingBlock();
  ReadingBlock(const ReadingBlock&);
  ReadingBlock& operator=(const ReadingBlock&);
  ReadingBlock(ReadingBlock&&);
  ReadingBlock& operator=(ReadingBlock&&);

  ReadingBlockKind kind = ReadingBlockKind::kParagraph;
  // A heading's level (1-6), 0 otherwise.
  int level = 0;
  // Speech-safe, at most kMaxReadingBlockChars.
  std::string text;
  // Where each sentence starts in |text|, in bytes (the first is 0).
  std::vector<uint32_t> sentences;
  // The block's node (to scroll it into view), and the first link in it
  // (the overlay can ring an item, not a paragraph).
  PageNodeHandle node;
  PageNodeHandle first_link;
};

// Where the text was taken from.
enum class ReadingSource : uint8_t {
  kSkill,    // the site skill's content root
  kMain,     // the main landmark
  kArticle,  // the first article
  kPage,     // the whole page
};

std::string_view ReadingSourceName(ReadingSource source);

struct ReadingDocument {
  ReadingDocument();
  ~ReadingDocument();
  ReadingDocument(const ReadingDocument&);
  ReadingDocument& operator=(const ReadingDocument&);
  ReadingDocument(ReadingDocument&&);
  ReadingDocument& operator=(ReadingDocument&&);

  std::vector<ReadingBlock> blocks;
  ReadingSource source = ReadingSource::kPage;
  // The page's title, speech-safe.
  std::string title;
  // A cap cut the document short (the rest of the page isn't read).
  bool capped = false;
  // The block that holds ReadingOptions::locate, else the first block
  // after it in reading order; -1 when it isn't in the document.
  int located = -1;
};

struct ReadingOptions {
  ReadingOptions();
  ~ReadingOptions();
  ReadingOptions(const ReadingOptions&);
  ReadingOptions& operator=(const ReadingOptions&);
  ReadingOptions(ReadingOptions&&);
  ReadingOptions& operator=(ReadingOptions&&);

  // A site skill's content root and the subtrees it leaves out
  // (SkillReadingScope); invalid and empty for the generic choice.
  PageNodeHandle root;
  std::vector<PageNodeHandle> skip;
  // A node to find the block of (the focused item, the selection, the
  // block read last).
  PageNodeHandle locate;
};

struct ReadingLimits {
  size_t max_nodes = 20000;
  size_t max_depth = 256;
  size_t max_blocks = 4000;
  // The document's text, in bytes.
  size_t max_bytes = 256 * 1024;
};

// |tree|'s text to read aloud, as blocks in reading order.
ReadingDocument BuildReadingDocument(const PageTree& tree,
                                     const ReadingOptions& options,
                                     const ReadingLimits& limits);

// A document of |text| alone (the selection, read as one paragraph):
// cleaned, capped at kMaxReadingBlockChars, split into sentences.
ReadingDocument TextDocument(std::string_view text);

// The selected text (PageTree::selection), speech-safe, at most
// |max_chars|; nullopt when nothing is selected, the selection's ends
// aren't in the tree, or it is in a credential field.
std::optional<std::string> SelectedText(const PageTree& tree,
                                        size_t max_chars,
                                        const ReadingLimits& limits);

// ---------------------------------------------------------------------------
// Positions and moves
// ---------------------------------------------------------------------------

// A place in the document: a block, and a byte offset into its text (a
// sentence's start, or a word's in a sentence too long for one chunk).
struct ReadingPosition {
  size_t block = 0;
  size_t offset = 0;

  friend bool operator==(const ReadingPosition&,
                         const ReadingPosition&) = default;
};

// What a chunk of speech is: whole sentences of one block.
struct ReadingChunk {
  ReadingChunk();
  ~ReadingChunk();
  ReadingChunk(const ReadingChunk&);
  ReadingChunk& operator=(const ReadingChunk&);
  ReadingChunk(ReadingChunk&&);
  ReadingChunk& operator=(ReadingChunk&&);

  std::string text;
  // Where it starts, and where the chunk after it starts (block ==
  // blocks.size() at the end).
  ReadingPosition at;
  ReadingPosition next;
};

// The longest chunk at speech rate |rate| (1 is the voice's normal pace),
// in code points: about 13 seconds of speech, at least 80 code points (a
// sentence is cut past that) and at most 300. The session forgets what
// the browser said after kSpokenMemoryMs (voice_session_logic.h, 20 s); a
// chunk is noted as it starts, so one said within that time is still
// known if it comes back through the microphone.
size_t ReadingChunkChars(double rate);

// The chunk starting at |at|: as many whole sentences of its block as fit
// in |max_chars| (one sentence longer than that is cut at a word), and
// where the next one starts. A heading or list item ends with a full stop
// so voices pause after it. nullopt at the end of the document or past
// |end_block|.
std::optional<ReadingChunk> ChunkAt(const ReadingDocument& document,
                                    ReadingPosition at,
                                    size_t max_chars,
                                    size_t end_block);

// The block a move goes to from block |from|: the next or previous heading,
// or paragraph (any block that isn't a heading); nullopt when there is
// none that way.
enum class ReadingUnit : uint8_t { kHeading, kParagraph };
std::optional<size_t> MoveBlock(const ReadingDocument& document,
                                size_t from,
                                ReadingUnit unit,
                                bool forward);

// The section the user named ("the section on history", "early years"):
// the heading block whose text matches |spoken| as voice mode matches
// spoken names (spoken_choice.h). nullopt when none does; |close| then
// holds the headings equally close (several), if any.
std::optional<size_t> FindSection(const ReadingDocument& document,
                                  std::string_view spoken,
                                  std::vector<size_t>* close);

// The block past heading |heading|'s section: the next heading of the
// same or a higher level, or the end.
size_t SectionEnd(const ReadingDocument& document, size_t heading);

// What the browser says when it moves to block |block| ("Heading level 2:
// History." / the paragraph's first sentence).
std::string MoveSpeech(const ReadingDocument& document, size_t block);

// ---------------------------------------------------------------------------
// The reading queue
// ---------------------------------------------------------------------------

// Reading in progress: the chunks handed to the voice (at most
// kReadingAhead ahead of the one being said), and where reading resumes.
// Stopping (barge-in, "stop", "pause") keeps the position at the first
// chunk not said to its end; "continue reading" starts there again.
class ReadingQueue {
 public:
  ReadingQueue();
  ~ReadingQueue();
  ReadingQueue(const ReadingQueue&) = delete;
  ReadingQueue& operator=(const ReadingQueue&) = delete;

  // Reads |document| from |from| up to |end_block| (exclusive; past the
  // last block for "to the end").
  void Start(ReadingDocument document, ReadingPosition from, size_t end_block);
  // The same document again, from |from| to |end_block|.
  void Restart(ReadingPosition from, size_t end_block);
  // The next chunk to hand to the voice, while fewer than kReadingAhead + 1
  // are with it and there is more; null otherwise. The chunk counts as
  // handed.
  std::optional<ReadingChunk> Next(double rate);
  // The oldest chunk handed was said to its end.
  void Spoken();
  // The voice stopped: what was handed and not said is dropped, and
  // reading would resume at the first of it.
  void Stop();
  // Forgets the document (another page).
  void Clear();

  bool loaded() const { return loaded_; }
  // Chunks are with the voice.
  bool speaking() const { return !handed_.empty(); }
  // Everything up to the end was said.
  bool finished() const;
  const ReadingDocument& document() const { return document_; }
  // Where reading resumes (the first chunk not said to its end).
  ReadingPosition position() const;
  void set_position(ReadingPosition position);
  size_t end_block() const { return end_block_; }
  // The chunk being said now, if any.
  const ReadingChunk* current() const {
    return handed_.empty() ? nullptr : &handed_.front();
  }
  // The block read last (said to its end or moved to), -1 for none: where
  // "next paragraph" and "next heading" move from.
  int current_block() const { return current_block_; }
  void set_current_block(int block) { current_block_ = block; }

 private:
  ReadingDocument document_;
  bool loaded_ = false;
  size_t end_block_ = 0;
  // The next chunk to hand starts here.
  ReadingPosition next_;
  std::deque<ReadingChunk> handed_;
  int current_block_ = -1;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_PAGE_READING_H_
