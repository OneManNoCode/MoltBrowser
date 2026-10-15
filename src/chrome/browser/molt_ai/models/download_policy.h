// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Pure (std-only) decisions for on-device model downloads (S13), unit-tested
// in download_policy_unittest.cc:
//   - which URL to fetch (pinned revision instead of a moving "main"),
//   - whether a server response to a resumed (Range) request can be
//     appended to the .partial file or the download must restart,
//   - whether a finished file may replace the model (size + SHA-256).

#ifndef CHROME_BROWSER_MOLT_AI_MODELS_DOWNLOAD_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_MODELS_DOWNLOAD_POLICY_H_

#include <cstdint>
#include <string>

namespace molt_ai {
namespace models {

// https://huggingface.co/<repo>/resolve/<revision>/<filename>. An empty
// |revision| means the entry isn't pinned yet and falls back to "main".
std::string ModelDownloadUrl(const std::string& repo,
                             const std::string& revision,
                             const std::string& filename);

// True for a 64-character lowercase hex SHA-256 digest.
bool IsValidSha256Hex(const std::string& hex);

enum class ResumeAction {
  kAppend,    // body continues exactly where the .partial file ends
  kRestart,   // server sent the whole file (or another range): truncate
              // the .partial file and write from byte 0
  kFail,      // not a usable response
};

// |requested_offset|: bytes already in the .partial file (0 = fresh).
// |http_status|: response code. |range_first|: first byte position from a
// 206 Content-Range header, or -1 if absent/unparseable.
ResumeAction DecideResumeAction(int64_t requested_offset,
                                int http_status,
                                int64_t range_first);

enum class VerifyResult {
  kVerified,      // matches the pinned SHA-256 (and size, if known)
  kUnverified,    // no pinned hash: allowed, but flagged to the user
  kSizeMismatch,  // truncated / oversized file
  kHashMismatch,  // content differs from the pinned file
};

// |expected_sha256| empty = entry not pinned. |expected_size| <= 0 =
// unknown size.
VerifyResult VerifyDownload(const std::string& actual_sha256,
                            int64_t actual_size,
                            const std::string& expected_sha256,
                            int64_t expected_size);

}  // namespace models
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MODELS_DOWNLOAD_POLICY_H_
