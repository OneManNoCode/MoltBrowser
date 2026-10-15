// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/models/download_policy.h"

namespace molt_ai {
namespace models {

std::string ModelDownloadUrl(const std::string& repo,
                             const std::string& revision,
                             const std::string& filename) {
  return "https://huggingface.co/" + repo + "/resolve/" +
         (revision.empty() ? std::string("main") : revision) + "/" + filename;
}

bool IsValidSha256Hex(const std::string& hex) {
  if (hex.size() != 64) return false;
  for (char c : hex) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

ResumeAction DecideResumeAction(int64_t requested_offset,
                                int http_status,
                                int64_t range_first) {
  if (http_status == 200) {
    // A fresh request, or a server/CDN that ignored the Range header and
    // is sending the whole file again: start over (never append a full
    // body after existing bytes).
    return requested_offset == 0 ? ResumeAction::kAppend
                                 : ResumeAction::kRestart;
  }
  if (http_status == 206) {
    if (range_first == requested_offset) return ResumeAction::kAppend;
    // A partial body that doesn't start where our file ends can't be
    // spliced in; a range starting at 0 can still be written from scratch.
    return range_first == 0 ? ResumeAction::kRestart : ResumeAction::kFail;
  }
  return ResumeAction::kFail;
}

VerifyResult VerifyDownload(const std::string& actual_sha256,
                            int64_t actual_size,
                            const std::string& expected_sha256,
                            int64_t expected_size) {
  if (expected_size > 0 && actual_size != expected_size) {
    return VerifyResult::kSizeMismatch;
  }
  if (expected_sha256.empty()) return VerifyResult::kUnverified;
  return actual_sha256 == expected_sha256 ? VerifyResult::kVerified
                                          : VerifyResult::kHashMismatch;
}

}  // namespace models
}  // namespace molt_ai
