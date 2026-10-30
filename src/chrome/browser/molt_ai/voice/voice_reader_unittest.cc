// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_reader.h"

#include <string>
#include <string_view>
#include <vector>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "chrome/browser/molt_ai/voice/html_fixture_tree.h"
#include "chrome/browser/molt_ai/voice/site_skills.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

using Reading = VoiceEnvironment::Reading;

PageTree Fixture(std::string_view name) {
  base::FilePath root;
  EXPECT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root));
  std::string html;
  EXPECT_TRUE(base::ReadFileToString(root.AppendASCII("chrome")
                                         .AppendASCII("test")
                                         .AppendASCII("data")
                                         .AppendASCII("molt_voice")
                                         .AppendASCII("skills")
                                         .AppendASCII(name),
                                     &html))
      << name;
  return test::PageTreeFromHtml(html);
}

// A voice that says each utterance when told to, and remembers them.
class FakeHost : public VoiceReader::Host {
 public:
  explicit FakeHost(PageTree tree) : tree_(std::move(tree)) {}

  uint64_t Speak(const std::string& text, double rate) override {
    queued_.push_back({++next_id_, text});
    rate_ = rate;
    return next_id_;
  }
  void Said(const std::string& text) override { said_.push_back(text); }
  void Status(const std::string& status) override { status_ = status; }
  void ScrollTo(const PageNodeHandle& node) override {
    scrolled_.push_back(node);
  }
  PageTree Tree() override { return tree_; }
  const SiteSkill* Skill() override {
    return BuiltInSkillForHost("en.wikipedia.org");
  }

  // The voice finishes what it says now (|spoken|) or drops everything.
  bool FinishOne(VoiceReader& reader) {
    if (queued_.empty()) {
      return false;
    }
    const uint64_t id = queued_.front().first;
    queued_.erase(queued_.begin());
    reader.OnUtteranceDone(id, true);
    return true;
  }
  void FinishAll(VoiceReader& reader) {
    for (int i = 0; i < 100 && FinishOne(reader); ++i) {
    }
  }
  void DropAll(VoiceReader& reader) {
    const auto queued = queued_;
    queued_.clear();
    for (const auto& [id, text] : queued) {
      reader.OnUtteranceDone(id, false);
    }
  }
  size_t queued() const { return queued_.size(); }
  std::string TakeSaid() {
    std::string all;
    for (const std::string& text : said_) {
      all += (all.empty() ? "" : "|") + text;
    }
    said_.clear();
    return all;
  }
  PageTree& tree() { return tree_; }
  const std::string& status() const { return status_; }
  const std::vector<PageNodeHandle>& scrolled() const { return scrolled_; }
  double rate() const { return rate_; }

 private:
  PageTree tree_;
  uint64_t next_id_ = 0;
  std::vector<std::pair<uint64_t, std::string>> queued_;
  std::vector<std::string> said_;
  std::string status_;
  std::vector<PageNodeHandle> scrolled_;
  double rate_ = 0;
};

constexpr int64_t kPage = 7;

TEST(VoiceReaderTest, ReadsThePageAChunkAhead) {
  FakeHost host(Fixture("wikipedia_article.html"));
  VoiceReader reader(&host);
  EXPECT_EQ(reader.StateFor(kPage), Reading::kNone);
  reader.Run(ReadOp::kPage, "", kPage, 1.0);
  // The chunk being said and one more.
  EXPECT_EQ(host.queued(), 2u);
  EXPECT_TRUE(reader.reading());
  EXPECT_EQ(reader.StateFor(kPage), Reading::kReading);
  // Each chunk is noted as it starts.
  EXPECT_EQ(host.TakeSaid(), "Queen (band).");
  EXPECT_EQ(host.status(), "Reading: Queen (band).");
  ASSERT_FALSE(host.scrolled().empty());
  host.FinishOne(reader);
  EXPECT_EQ(host.TakeSaid(), "From Wikipedia, the free encyclopedia.");
  EXPECT_EQ(host.queued(), 2u);
  host.FinishAll(reader);
  const std::string said = host.TakeSaid();
  EXPECT_NE(said.find("The history section starts with this sentence."),
            std::string::npos);
  EXPECT_EQ(said.substr(said.size() - 27), "That's the end of the page.");
  EXPECT_EQ(reader.StateFor(kPage), Reading::kNone);
  EXPECT_EQ(host.status(), "Done reading");
}

TEST(VoiceReaderTest, StopKeepsThePlaceAndContinueGoesOn) {
  FakeHost host(Fixture("wikipedia_article.html"));
  VoiceReader reader(&host);
  reader.Run(ReadOp::kPage, "", kPage, 1.0);
  host.FinishOne(reader);
  host.FinishOne(reader);
  host.TakeSaid();
  // Barge-in: the controller interrupts, then the voice drops the chunks.
  reader.Interrupt();
  host.DropAll(reader);
  EXPECT_FALSE(reader.reading());
  EXPECT_EQ(reader.StateFor(kPage), Reading::kReading);
  // The next command's interrupt: it was not reading any more.
  reader.Interrupt();
  EXPECT_EQ(reader.StateFor(kPage), Reading::kStopped);
  // "Continue reading": the chunk that was cut short, again.
  reader.Run(ReadOp::kResume, "", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid(), "This is a stand-in for a hatnote.");
  // Another page: nothing to go on with there.
  reader.Interrupt();
  host.DropAll(reader);
  EXPECT_EQ(reader.StateFor(kPage + 1), Reading::kNone);
  reader.Run(ReadOp::kResume, "", kPage + 1, 1.0);
  EXPECT_EQ(host.TakeSaid(),
            "There's nothing to go on with here. Say read the page.");
}

TEST(VoiceReaderTest, SomethingElseStoppingTheVoiceKeepsThePlace) {
  FakeHost host(Fixture("wikipedia_article.html"));
  VoiceReader reader(&host);
  reader.Run(ReadOp::kPage, "", kPage, 1.0);
  host.FinishOne(reader);
  host.TakeSaid();
  // An answer interrupted the voice (a notice): the chunks are dropped.
  host.DropAll(reader);
  EXPECT_FALSE(reader.reading());
  EXPECT_EQ(reader.StateFor(kPage), Reading::kStopped);
  reader.Run(ReadOp::kResume, "", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid(), "From Wikipedia, the free encyclopedia.");
}

TEST(VoiceReaderTest, ASection) {
  FakeHost host(Fixture("wikipedia_article.html"));
  VoiceReader reader(&host);
  reader.Run(ReadOp::kSection, "history", kPage, 1.0);
  host.FinishAll(reader);
  EXPECT_EQ(host.TakeSaid(),
            "History.|The history section starts with this sentence. It has "
            "a second sentence too.|Early years.|The early years come "
            "first. Then the next step follows.|A first point of a list.|A "
            "second point of the list.|That's the end of the section.");
  reader.Run(ReadOp::kSection, "discography", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid().find("There's no section called discography. "
                                 "The sections are: Queen (band), History"),
            0u);
}

TEST(VoiceReaderTest, HeadingsAndParagraphs) {
  FakeHost host(Fixture("wikipedia_article.html"));
  VoiceReader reader(&host);
  // Not reading: the heading is said, and the place is after it.
  reader.Run(ReadOp::kNextHeading, "", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid(), "Heading level 1: Queen (band).");
  host.FinishAll(reader);
  reader.Run(ReadOp::kNextHeading, "", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid(), "Heading level 2: History.");
  host.FinishAll(reader);
  // The next paragraph is read, alone.
  reader.Run(ReadOp::kNextParagraph, "", kPage, 1.0);
  host.FinishAll(reader);
  EXPECT_EQ(host.TakeSaid(),
            "The history section starts with this sentence. It has a second "
            "sentence too.");
  reader.Run(ReadOp::kNextParagraph, "", kPage, 1.0);
  host.FinishAll(reader);
  EXPECT_EQ(host.TakeSaid(),
            "The early years come first. Then the next step follows.");
  reader.Run(ReadOp::kPreviousHeading, "", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid(), "Heading level 3: Early years.");
  // "Continue reading" goes on after it.
  reader.Interrupt();
  reader.Run(ReadOp::kResume, "", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid(),
            "The early years come first. Then the next step follows.");

  // While reading, a move goes on reading from there.
  reader.Interrupt();
  host.DropAll(reader);
  reader.Run(ReadOp::kNextHeading, "", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid(), "Heading level 2: Musical style.|The style "
                             "section has one paragraph. It has two "
                             "sentences.");
  EXPECT_TRUE(reader.reading());
  reader.Interrupt();
  host.DropAll(reader);
  reader.Run(ReadOp::kNextHeading, "", kPage, 1.0);
  host.TakeSaid();
  reader.Interrupt();
  host.DropAll(reader);
  reader.Run(ReadOp::kNextHeading, "", kPage, 1.0);
  host.TakeSaid();
  reader.Interrupt();
  host.DropAll(reader);
  reader.Interrupt();
  reader.Run(ReadOp::kNextHeading, "", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid(), "That's the last heading.");
}

TEST(VoiceReaderTest, FromHereFollowsTheFocus) {
  PageTree tree = Fixture("wikipedia_article.html");
  FakeHost host(tree);
  VoiceReader reader(&host);
  // Focus on the lead's link: reading starts there.
  for (const PageNode& node : host.tree().nodes) {
    if (node.role == PageRole::kLink && node.url == "#link") {
      host.tree().focus = node.handle;
    }
  }
  ASSERT_TRUE(host.tree().focus.valid());
  reader.Run(ReadOp::kFromHere, "", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid(), "a link inside.");
  host.FinishOne(reader);
  host.TakeSaid();
  reader.Interrupt();
  host.DropAll(reader);
  // The focus hasn't moved: from where it stopped.
  reader.Run(ReadOp::kFromHere, "", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid(),
            "The lead's second paragraph stands in for more text. It ends "
            "here.");
}

TEST(VoiceReaderTest, TheSelectionAndTheRate) {
  FakeHost host(Fixture("wikipedia_article.html"));
  VoiceReader reader(&host);
  reader.Run(ReadOp::kSelection, "", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid(),
            "Nothing is selected. Select some text, or say read the page.");
  // Select from the History heading's text to the middle of its paragraph.
  PageTree& tree = host.tree();
  PageNodeHandle heading_text;
  PageNodeHandle paragraph_text;
  for (size_t i = 0; i + 1 < tree.nodes.size(); ++i) {
    if (tree.nodes[i].role == PageRole::kStaticText &&
        tree.nodes[i].name == "History") {
      heading_text = tree.nodes[i].handle;
    }
    if (tree.nodes[i].role == PageRole::kStaticText &&
        tree.nodes[i].name.find("The history section") == 0) {
      paragraph_text = tree.nodes[i].handle;
    }
  }
  tree.selection.anchor = heading_text;
  tree.selection.anchor_offset = 0;
  tree.selection.focus = paragraph_text;
  tree.selection.focus_offset = 11;
  reader.Run(ReadOp::kSelection, "", kPage, 2.0);
  host.FinishAll(reader);
  // What is selected, as it reads (the edit link too).
  EXPECT_EQ(host.TakeSaid(), "History [ edit] The history");
  EXPECT_EQ(host.rate(), 2.0);
}

TEST(VoiceReaderTest, NoText) {
  FakeHost host{PageTree()};
  VoiceReader reader(&host);
  reader.Run(ReadOp::kPage, "", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid(), "There's no text to read on this page.");
  reader.Run(ReadOp::kNextHeading, "", kPage, 1.0);
  EXPECT_EQ(host.TakeSaid(), "There's no text to read on this page.");
  EXPECT_FALSE(reader.reading());
}

TEST(VoiceReaderTest, AnotherPageIsShownMidway) {
  FakeHost host(Fixture("wikipedia_article.html"));
  VoiceReader reader(&host);
  reader.Run(ReadOp::kPage, "", kPage, 1.0);
  host.TakeSaid();
  reader.PageChanged(kPage + 1);
  host.FinishAll(reader);
  // What was with the voice is said; nothing more is handed over.
  EXPECT_EQ(host.TakeSaid(), "");
  EXPECT_FALSE(reader.reading());
  EXPECT_EQ(reader.StateFor(kPage + 1), Reading::kNone);
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
