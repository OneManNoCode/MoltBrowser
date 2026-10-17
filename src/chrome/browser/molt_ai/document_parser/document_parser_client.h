// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// DocumentParserClient: how the browser parses untrusted documents (audit S5,
// the "Rule of 2"): PDFs downloaded for /pdf, and the PDF / Office / e-book /
// CSV files attached in the AI chat. Every parse runs in a utility process
// under the most restrictive sandbox (molt_ai.mojom.DocumentParser,
// sandbox.mojom.Sandbox.kService; //chrome/services/molt_ai/document_parser),
// so a parser bug or a Rust panic costs that process, never the browser.
//
//   - One process per call. Launching costs some latency per document, which
//     these user-initiated actions can afford, and it isolates documents
//     from each other: a document that compromises its parser never sees, or
//     changes the text of, a later one, and a crash or hang affects only its
//     own call. Reusing a process would save the launch but share all three.
//   - A timeout per call (kTimeout): a hostile document can make a parser
//     loop forever. On timeout the call fails, the pipe is closed and the
//     process is terminated. Closing the pipe alone would not do: a process
//     stuck inside a parse never gets to notice it.
//   - A crashed or killed process fails its call (disconnect handler), so
//     every call is answered exactly once.
//   - Replies are untrusted: text or error strings that are not valid UTF-8
//     (base::IsStringUTF8) fail the call, and the text is cut to the call's
//     max_chars.
//
// Still outside a sandbox (tracked separately): OCR of scans and images runs
// the bundled tesseract as an unsandboxed helper process (molt_ai/ocr), and
// llama.cpp inference runs in the browser process (molt_ai/runtime).
//
// The service keeps no profile state, so incognito and MoltNet profiles need
// nothing special. Use on the UI thread.

#ifndef CHROME_BROWSER_MOLT_AI_DOCUMENT_PARSER_DOCUMENT_PARSER_CLIENT_H_
#define CHROME_BROWSER_MOLT_AI_DOCUMENT_PARSER_DOCUMENT_PARSER_CLIENT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "chrome/services/molt_ai/document_parser/public/mojom/document_parser.mojom-forward.h"
#include "mojo/public/cpp/bindings/pending_remote.h"

namespace molt_ai {

struct DocumentParseResult {
  enum class Status {
    // `text` holds the document's text: valid UTF-8, not empty, at most the
    // call's max_chars bytes.
    kOk,
    // The parser ran and found nothing to extract, or refused the document
    // as malformed. `error` may say why; it can be empty.
    kNoText,
    // No usable answer: the parser process crashed, was killed or timed out,
    // its reply was malformed, or the document could not be sent. `error`
    // says which.
    kFailed,
  };

  DocumentParseResult();
  DocumentParseResult(Status status, std::string text, std::string error);
  DocumentParseResult(const DocumentParseResult&);
  DocumentParseResult(DocumentParseResult&&);
  DocumentParseResult& operator=(const DocumentParseResult&);
  DocumentParseResult& operator=(DocumentParseResult&&);
  ~DocumentParseResult();

  Status status = Status::kFailed;
  std::string text;
  std::string error;
};

class DocumentParserClient {
 public:
  using ResultCallback = base::OnceCallback<void(DocumentParseResult)>;
  // Receives the parser's process once it runs, so that a hung parse can be
  // terminated.
  using ProcessCallback = base::OnceCallback<void(const base::Process&)>;
  // Starts one parser process and returns its remote. The default launches a
  // sandboxed utility process; tests pass a fake.
  using LaunchCallback =
      base::RepeatingCallback<mojo::PendingRemote<mojom::DocumentParser>(
          ProcessCallback)>;

  static constexpr base::TimeDelta kTimeout = base::Seconds(30);
  // Each call is a process; calls beyond this many at once fail right away.
  static constexpr size_t kMaxConcurrentCalls = 4;

  // Parses in sandboxed utility processes.
  DocumentParserClient();
  // For tests: |launch| instead of utility processes, and a custom timeout.
  DocumentParserClient(LaunchCallback launch, base::TimeDelta timeout);
  DocumentParserClient(const DocumentParserClient&) = delete;
  DocumentParserClient& operator=(const DocumentParserClient&) = delete;
  // Pending calls end without running their callbacks, and their processes
  // are terminated.
  ~DocumentParserClient();

  // Each call copies the document before returning and later runs
  // |callback| exactly once (never synchronously), unless this client is
  // destroyed first. At most |max_chars| bytes of text come back (and never
  // more than mojom::kMaxOutputChars).

  // The text layer of a PDF. kNoText: no readable text layer (e.g. a scan).
  void ExtractPdfText(base::span<const uint8_t> pdf,
                      size_t max_chars,
                      ResultCallback callback);

  // Office / e-book / spreadsheet / CSV to Markdown (the anydoc crate).
  // |extension| is the file name's, lowercase and without the dot ("docx");
  // the format is sniffed from the bytes first. kNoText carries anydoc's
  // diagnostic, or an empty error when the document converted to nothing.
  void ConvertToMarkdown(base::span<const uint8_t> document,
                         std::string_view extension,
                         size_t max_chars,
                         ResultCallback callback);

  // The flat text of a .docx (word/document.xml).
  void ExtractDocxText(base::span<const uint8_t> docx,
                       size_t max_chars,
                       ResultCallback callback);

  size_t pending_calls_for_testing() const { return calls_.size(); }

 private:
  class Call;
  class ProcessKiller;

  // A started call (its process launching), or null after answering
  // |callback| with a refusal.
  Call* StartCall(size_t document_size,
                  size_t max_chars,
                  ResultCallback callback);
  void Refuse(ResultCallback callback, DocumentParseResult result);
  void RunRefusal(ResultCallback callback, DocumentParseResult result);
  // Ends |call| (closing its pipe) and then runs its callback with |result|.
  void OnCallDone(Call* call, DocumentParseResult result);

  const LaunchCallback launch_;
  const base::TimeDelta timeout_;
  std::vector<std::unique_ptr<Call>> calls_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<DocumentParserClient> weak_ptr_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_DOCUMENT_PARSER_DOCUMENT_PARSER_CLIENT_H_
