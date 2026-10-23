// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The memory search snippet (memory/memory_snippet.h) on page text a site
// put in the user's history and any query.
//
// Checks: no memory or UB error or timeout; the snippet is at most
// |max_bytes| of the chunk plus an ellipsis at each cut end, is a piece of
// the chunk, the chunk itself when it fits, and never splits a UTF-8
// character of a valid chunk.

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/browser/molt_ai/memory/memory_snippet.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider p(data, size);
  const size_t max_bytes = p.ConsumeBool()
                               ? 240  // memory_backend.cc's kSnippetBytes
                               : p.ConsumeIntegralInRange<size_t>(0, 600);
  const std::string query = p.ConsumeRandomLengthString(512);
  const std::string chunk = p.ConsumeRemainingBytesAsString();

  const std::string out = molt_ai::memory::BestSnippet(chunk, query, max_bytes);
  if (max_bytes == 0) {
    CHECK(out.empty());
    return 0;
  }
  if (chunk.size() <= max_bytes) {
    CHECK(out == chunk);
    return 0;
  }
  // Without an ellipsis at either end (one the chunk itself had too: the
  // rest is still a piece of the chunk).
  static constexpr char kEllipsis[] = "\xE2\x80\xA6";
  CHECK(out.size() <= max_bytes + 6);
  std::string body = out;
  if (body.rfind(kEllipsis, 0) == 0) body.erase(0, 3);
  if (body.size() >= 3 && body.compare(body.size() - 3, 3, kEllipsis) == 0) {
    body.erase(body.size() - 3);
  }
  CHECK(body.size() <= max_bytes) << "snippet over max_bytes";
  CHECK(chunk.find(body) != std::string::npos) << "not a piece of the chunk";
  if (molt_ai::IsValidUtf8(chunk)) {
    CHECK(molt_ai::IsValidUtf8(out)) << "a character was cut";
  }
  return 0;
}
