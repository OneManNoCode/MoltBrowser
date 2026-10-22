// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/models/model_download_plan.h"

#include <cstdint>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/models/download_policy.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace models {
namespace {

const char kRevision[] = "0123456789abcdef0123456789abcdef01234567";
const char kWeightsHash[] =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const char kProjectorHash[] =
    "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
constexpr int64_t kWeightsSize = 1'107'409'472;
constexpr int64_t kProjectorSize = 445'000'000;

ModelFileSpec Weights() {
  ModelFileSpec f;
  f.role = ModelFileRole::kWeights;
  f.repo = "Qwen/Qwen3-VL-2B-Instruct-GGUF";
  f.revision = kRevision;
  f.filename = "Qwen3VL-2B-Instruct-Q4_K_M.gguf";
  f.final_path = "/m/Qwen3VL-2B-Instruct-Q4_K_M.gguf";
  f.sha256 = kWeightsHash;
  f.size = kWeightsSize;
  return f;
}

ModelFileSpec Projector() {
  ModelFileSpec f = Weights();
  f.role = ModelFileRole::kVisionProjector;
  f.filename = "mmproj-Qwen3VL-2B-Instruct-Q8_0.gguf";
  f.final_path = "/m/mmproj-Qwen3VL-2B-Instruct-Q8_0.gguf";
  f.sha256 = kProjectorHash;
  f.size = kProjectorSize;
  return f;
}

// A text-only model not pinned yet: downloaded from "main", unverified.
ModelFileSpec UnpinnedWeights() {
  ModelFileSpec f;
  f.repo = "a/b";
  f.filename = "m.gguf";
  f.final_path = "/m/m.gguf";
  f.size = 1000;  // the registry's estimate
  return f;
}

std::vector<ModelFileSpec> Pair() {
  return {Weights(), Projector()};
}

ModelFileOnDisk Absent() {
  return ModelFileOnDisk();
}

ModelFileOnDisk Present(int64_t size) {
  ModelFileOnDisk d;
  d.final_exists = true;
  d.final_size = size;
  return d;
}

ModelFileOnDisk Partial(int64_t bytes) {
  ModelFileOnDisk d;
  d.partial_size = bytes;
  return d;
}

TEST(ModelDownloadPlanTest, BothFilesComeFromThePinnedRevision) {
  EXPECT_EQ(ModelFileUrl(Weights()),
            "https://huggingface.co/Qwen/Qwen3-VL-2B-Instruct-GGUF/resolve/" +
                std::string(kRevision) + "/Qwen3VL-2B-Instruct-Q4_K_M.gguf");
  EXPECT_EQ(ModelFileUrl(Projector()),
            "https://huggingface.co/Qwen/Qwen3-VL-2B-Instruct-GGUF/resolve/" +
                std::string(kRevision) +
                "/mmproj-Qwen3VL-2B-Instruct-Q8_0.gguf");
}

TEST(ModelDownloadPlanTest, EachFileIsVerifiedAgainstItsOwnPin) {
  EXPECT_EQ(ExpectedSizeForJob(Weights()), kWeightsSize);
  EXPECT_EQ(ExpectedSizeForJob(Projector()), kProjectorSize);
  EXPECT_EQ(ExpectedSizeForJob(UnpinnedWeights()), 0);
  // What the job checks a finished file against (VerifyDownload): a
  // projector with the weights' hash, or the right hash at the wrong size,
  // is discarded.
  EXPECT_EQ(VerifyDownload(kProjectorHash, kProjectorSize, Projector().sha256,
                           ExpectedSizeForJob(Projector())),
            VerifyResult::kVerified);
  EXPECT_EQ(VerifyDownload(kWeightsHash, kProjectorSize, Projector().sha256,
                           ExpectedSizeForJob(Projector())),
            VerifyResult::kHashMismatch);
  EXPECT_EQ(VerifyDownload(kProjectorHash, kProjectorSize - 1,
                           Projector().sha256, ExpectedSizeForJob(Projector())),
            VerifyResult::kSizeMismatch);
  EXPECT_EQ(VerifyDownload(kProjectorHash, kWeightsSize, Weights().sha256,
                           ExpectedSizeForJob(Weights())),
            VerifyResult::kHashMismatch);
}

TEST(ModelDownloadPlanTest, PinsOfAVisionModel) {
  EXPECT_TRUE(IsFullyPinned(Weights()));
  EXPECT_TRUE(IsFullyPinned(Projector()));
  EXPECT_EQ(CheckUnit(Pair()), PlanError::kNone);

  // Fail closed: every field of both files, or no download at all.
  auto with = [](auto change) {
    std::vector<ModelFileSpec> unit = Pair();
    change(unit);
    return CheckUnit(unit);
  };
  EXPECT_EQ(with([](auto& u) { u[1].sha256.clear(); }), PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[1].size = 0; }), PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[1].size = -5; }), PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[1].revision = ""; }), PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[1].revision = "main"; }),
            PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[0].sha256.clear(); }), PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[0].size = 0; }), PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[0].revision = ""; }), PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[1].sha256[0] = 'E'; }),
            PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[1].sha256.pop_back(); }),
            PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[0].revision.pop_back(); }),
            PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[0].revision[3] = 'G'; }),
            PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[0].revision[3] = 'A'; }),
            PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[1].revision += "0"; }),
            PlanError::kNotPinned);
  EXPECT_EQ(with([](auto& u) { u[0].sha256 += "0"; }), PlanError::kNotPinned);

  // A refused unit is never planned, whatever is on disk.
  std::vector<ModelFileSpec> unpinned = Pair();
  unpinned[1].sha256.clear();
  const DownloadPlan plan =
      PlanModelDownload(unpinned, {Present(kWeightsSize), Absent()});
  EXPECT_EQ(plan.error, PlanError::kNotPinned);
  EXPECT_TRUE(plan.to_fetch.empty());
  EXPECT_FALSE(IsUnitDownloaded(unpinned, {Present(kWeightsSize),
                                           Present(kProjectorSize)}));
  EXPECT_FALSE(PlanErrorText(PlanError::kNotPinned).empty());
}

TEST(ModelDownloadPlanTest, TextOnlyModelsMayStillBeUnpinned) {
  EXPECT_EQ(CheckUnit({UnpinnedWeights()}), PlanError::kNone);
  EXPECT_EQ(CheckUnit({Weights()}), PlanError::kNone);
  ModelFileSpec bad = Weights();
  bad.sha256 = "not-a-hash";
  EXPECT_EQ(CheckUnit({bad}), PlanError::kBadPin);
  bad = Weights();
  bad.size = 0;
  EXPECT_EQ(CheckUnit({bad}), PlanError::kBadPin);
  EXPECT_FALSE(PlanErrorText(PlanError::kBadPin).empty());
}

TEST(ModelDownloadPlanTest, WhatIsAUnit) {
  EXPECT_EQ(CheckUnit({}), PlanError::kBadUnit);
  EXPECT_EQ(CheckUnit({Projector()}), PlanError::kBadUnit);
  EXPECT_EQ(CheckUnit({Projector(), Weights()}), PlanError::kBadUnit);
  EXPECT_EQ(CheckUnit({Weights(), Weights()}), PlanError::kBadUnit);
  EXPECT_EQ(CheckUnit({Weights(), Projector(), Projector()}),
            PlanError::kBadUnit);
  // The stat list must match.
  EXPECT_EQ(PlanModelDownload(Pair(), {Absent()}).error, PlanError::kBadUnit);
  EXPECT_FALSE(IsUnitDownloaded(Pair(), {Present(kWeightsSize)}));
  EXPECT_FALSE(IsUnitDownloaded({}, {}));
}

TEST(ModelDownloadPlanTest, FreshDownloadFetchesWeightsThenProjector) {
  const DownloadPlan plan = PlanModelDownload(Pair(), {Absent(), Absent()});
  EXPECT_EQ(plan.error, PlanError::kNone);
  EXPECT_EQ(plan.to_fetch, (std::vector<size_t>{0, 1}));
  EXPECT_EQ(plan.total_bytes, kWeightsSize + kProjectorSize);
  EXPECT_EQ(plan.done_bytes, 0);
  EXPECT_EQ(plan.bytes_to_write, kWeightsSize + kProjectorSize);
  EXPECT_FALSE(IsUnitDownloaded(Pair(), {Absent(), Absent()}));
}

TEST(ModelDownloadPlanTest, WeightsWithoutProjectorAreAPartialDownload) {
  // The weights finished, the projector didn't start (or failed, or its
  // hash didn't match and it was discarded): not downloaded, and only
  // the projector is fetched next time.
  const std::vector<ModelFileOnDisk> disk = {Present(kWeightsSize), Absent()};
  EXPECT_FALSE(IsUnitDownloaded(Pair(), disk));
  const DownloadPlan plan = PlanModelDownload(Pair(), disk);
  EXPECT_EQ(plan.to_fetch, (std::vector<size_t>{1}));
  EXPECT_EQ(plan.total_bytes, kWeightsSize + kProjectorSize);
  EXPECT_EQ(plan.done_bytes, kWeightsSize);
  EXPECT_EQ(plan.bytes_to_write, kProjectorSize);
}

TEST(ModelDownloadPlanTest, ProjectorWithoutWeightsIsAPartialDownload) {
  // The weights' hash didn't match (discarded) after a projector from an
  // earlier attempt: the weights are fetched again, the projector kept.
  const std::vector<ModelFileOnDisk> disk = {Absent(), Present(kProjectorSize)};
  EXPECT_FALSE(IsUnitDownloaded(Pair(), disk));
  const DownloadPlan plan = PlanModelDownload(Pair(), disk);
  EXPECT_EQ(plan.to_fetch, (std::vector<size_t>{0}));
  EXPECT_EQ(plan.done_bytes, kProjectorSize);
  EXPECT_EQ(plan.bytes_to_write, kWeightsSize);
}

TEST(ModelDownloadPlanTest, PartialFilesNeverCount) {
  // .partial files of both, even complete-sized ones never renamed (the
  // job only moves a verified file into place).
  for (const auto& disk : std::vector<std::vector<ModelFileOnDisk>>{
           {Partial(10), Partial(10)},
           {Partial(kWeightsSize), Partial(kProjectorSize)},
           {Present(kWeightsSize), Partial(kProjectorSize)},
           {Partial(kWeightsSize), Present(kProjectorSize)},
       }) {
    EXPECT_FALSE(IsUnitDownloaded(Pair(), disk));
  }
  // A resumed projector: only what is missing is needed on disk ...
  DownloadPlan plan =
      PlanModelDownload(Pair(), {Present(kWeightsSize), Partial(1000)});
  EXPECT_EQ(plan.to_fetch, (std::vector<size_t>{1}));
  EXPECT_EQ(plan.bytes_to_write, kProjectorSize - 1000);
  // ... unless its .partial is a complete-sized leftover, which the job
  // writes again from byte 0.
  plan = PlanModelDownload(Pair(), {Present(kWeightsSize),
                                    Partial(kProjectorSize + 7)});
  EXPECT_EQ(plan.bytes_to_write, kProjectorSize);
  plan = PlanModelDownload(Pair(),
                           {Present(kWeightsSize), Partial(kProjectorSize)});
  EXPECT_EQ(plan.bytes_to_write, kProjectorSize);
  plan = PlanModelDownload(Pair(), {Present(kWeightsSize),
                                    Partial(kProjectorSize - 1)});
  EXPECT_EQ(plan.bytes_to_write, 1);
}

TEST(ModelDownloadPlanTest, AFileOfTheWrongSizeIsNotComplete) {
  // Truncated or replaced after it was verified.
  for (int64_t size : {int64_t{0}, int64_t{1}, kWeightsSize - 1,
                       kWeightsSize + 1}) {
    EXPECT_FALSE(IsUnitDownloaded(Pair(),
                                  {Present(size), Present(kProjectorSize)}))
        << size;
    const DownloadPlan plan =
        PlanModelDownload(Pair(), {Present(size), Present(kProjectorSize)});
    EXPECT_EQ(plan.to_fetch, (std::vector<size_t>{0})) << size;
  }
  EXPECT_FALSE(IsUnitDownloaded(
      Pair(), {Present(kWeightsSize), Present(kProjectorSize - 1)}));
}

TEST(ModelDownloadPlanTest, BothFilesCompleteIsDownloaded) {
  const std::vector<ModelFileOnDisk> disk = {Present(kWeightsSize),
                                             Present(kProjectorSize)};
  EXPECT_TRUE(IsUnitDownloaded(Pair(), disk));
  const DownloadPlan plan = PlanModelDownload(Pair(), disk);
  EXPECT_TRUE(plan.to_fetch.empty());
  EXPECT_EQ(plan.done_bytes, plan.total_bytes);
  EXPECT_EQ(plan.bytes_to_write, 0);
}

TEST(ModelDownloadPlanTest, DeletionLeavesNothingThatCounts) {
  // DeleteModel removes both files (and the handler their .partial files).
  EXPECT_FALSE(IsUnitDownloaded(Pair(), {Absent(), Absent()}));
  // A delete that left one file behind (it couldn't be removed) is not a
  // downloaded model either, and the next download fetches the other.
  EXPECT_FALSE(IsUnitDownloaded(Pair(), {Absent(), Present(kProjectorSize)}));
  EXPECT_FALSE(IsUnitDownloaded(Pair(), {Present(kWeightsSize), Absent()}));
}

TEST(ModelDownloadPlanTest, UnpinnedTextModelUsesTheEstimate) {
  const std::vector<ModelFileSpec> unit = {UnpinnedWeights()};
  // Any non-empty file counts once it is in place (it was moved there
  // only after a complete download).
  EXPECT_TRUE(IsUnitDownloaded(unit, {Present(1234)}));
  EXPECT_FALSE(IsUnitDownloaded(unit, {Present(0)}));
  EXPECT_FALSE(IsUnitDownloaded(unit, {Partial(1000)}));
  DownloadPlan plan = PlanModelDownload(unit, {Partial(400)});
  EXPECT_EQ(plan.to_fetch, (std::vector<size_t>{0}));
  EXPECT_EQ(plan.total_bytes, 1000);
  EXPECT_EQ(plan.bytes_to_write, 600);
  // A .partial beyond the estimate: resumed, nothing more is promised.
  plan = PlanModelDownload(unit, {Partial(5000)});
  EXPECT_EQ(plan.bytes_to_write, 0);
}

}  // namespace
}  // namespace models
}  // namespace molt_ai
