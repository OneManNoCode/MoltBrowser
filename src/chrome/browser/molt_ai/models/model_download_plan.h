// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A model download is one unit: the weights and, for a model that sees
// images, its vision projector (llama.cpp's "mmproj" GGUF, from the same
// repo at the same pinned revision; docs/design/SANDBOXED_INFERENCE.md
// section 10.4). The unit is downloaded file by file, each with its own
// ModelDownloadJob (pinned SHA-256 and exact size, streamed into
// "<file>.partial", moved into place only once verified), and counts as
// downloaded only when every file is complete.
//
// Pure (std-only) decisions, unit-tested in model_download_plan_unittest.cc:
//   - that a unit with a projector is fully pinned (fail closed; a
//     text-only model may still download unpinned, as "unverified"),
//   - which files a download still has to fetch, in order,
//   - the bytes it shows progress against and needs on disk,
//   - when the unit counts as downloaded.
// The caller does the IO: it stats the files (ModelFileOnDisk), runs the
// jobs and removes the unit's files on cancel and delete.

#ifndef CHROME_BROWSER_MOLT_AI_MODELS_MODEL_DOWNLOAD_PLAN_H_
#define CHROME_BROWSER_MOLT_AI_MODELS_MODEL_DOWNLOAD_PLAN_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace models {

enum class ModelFileRole {
  kWeights,
  kVisionProjector,
};

// One file of a model, as the registry describes it.
struct ModelFileSpec {
  ModelFileSpec();
  ~ModelFileSpec();
  ModelFileSpec(const ModelFileSpec&);
  ModelFileSpec& operator=(const ModelFileSpec&);
  ModelFileSpec(ModelFileSpec&&);
  ModelFileSpec& operator=(ModelFileSpec&&);

  ModelFileRole role = ModelFileRole::kWeights;
  // Hugging Face repo, pinned commit ("" = not pinned: "main") and the
  // file's name in the repo.
  std::string repo;
  std::string revision;
  std::string filename;
  // Where the verified file lives (UTF-8 path).
  std::string final_path;
  // Lowercase hex SHA-256; "" = not pinned.
  std::string sha256;
  // Exact size when pinned; otherwise the registry's estimate (0 unknown).
  int64_t size = 0;
};

// What is on disk for one file. The caller stats it.
struct ModelFileOnDisk {
  // A regular file at final_path, and its size.
  bool final_exists = false;
  int64_t final_size = 0;
  // Bytes in "<final_path>.partial" (0 when there is none).
  int64_t partial_size = 0;
};

// The URL the file is fetched from (ModelDownloadUrl()).
std::string ModelFileUrl(const ModelFileSpec& file);

// Pinned all the way: a 40-hex-digit revision, a SHA-256 and a size.
bool IsFullyPinned(const ModelFileSpec& file);

// The size ModelDownloadJob must find (Params::expected_size): the exact
// size of a pinned file, else 0 (unknown).
int64_t ExpectedSizeForJob(const ModelFileSpec& file);

// Whether the file is downloaded: present and, when pinned, of exactly its
// pinned size (IsCompleteModelFile()). A .partial never counts.
bool IsComplete(const ModelFileSpec& file, const ModelFileOnDisk& on_disk);

enum class PlanError {
  kNone,
  // Not a unit: no files, the weights not first, more than one projector,
  // or a stat list that doesn't match the files.
  kBadUnit,
  // A model with a projector whose files aren't all fully pinned. Such a
  // model is not even offered (IsPinnedForVision() in the runtime); this
  // refuses it again, before any byte is fetched.
  kNotPinned,
  // A pinned SHA-256 that isn't 64 lowercase hex digits, or a pinned file
  // without a size.
  kBadPin,
};

// User-facing text for an error (fixed, never a path or name).
std::string_view PlanErrorText(PlanError error);

// Whether |files| may be downloaded at all (the pins above).
PlanError CheckUnit(const std::vector<ModelFileSpec>& files);

struct DownloadPlan {
  DownloadPlan();
  ~DownloadPlan();
  DownloadPlan(const DownloadPlan&);
  DownloadPlan& operator=(const DownloadPlan&);
  DownloadPlan(DownloadPlan&&);
  DownloadPlan& operator=(DownloadPlan&&);

  PlanError error = PlanError::kNone;
  // Indices into the unit of the files still to download, in unit order
  // (the weights before the projector). Empty: the unit is downloaded.
  std::vector<size_t> to_fetch;
  // The whole unit's size, what progress counts up to ...
  int64_t total_bytes = 0;
  // ... and where it starts: the complete files' bytes.
  int64_t done_bytes = 0;
  // Disk space the download still needs: for each file to fetch, its size
  // less what its .partial already holds (all of it when the .partial is
  // as large as the file, which the job then starts over).
  int64_t bytes_to_write = 0;
};

// |on_disk| holds one entry per file of |files|, in the same order.
DownloadPlan PlanModelDownload(const std::vector<ModelFileSpec>& files,
                               const std::vector<ModelFileOnDisk>& on_disk);

// Whether the unit counts as downloaded: a valid unit (CheckUnit) whose
// every file is complete. Partial downloads (one file of two, or a
// .partial) never count.
bool IsUnitDownloaded(const std::vector<ModelFileSpec>& files,
                      const std::vector<ModelFileOnDisk>& on_disk);

}  // namespace models
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MODELS_MODEL_DOWNLOAD_PLAN_H_
