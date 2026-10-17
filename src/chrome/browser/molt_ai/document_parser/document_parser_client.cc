// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/document_parser/document_parser_client.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "chrome/services/molt_ai/document_parser/public/mojom/document_parser.mojom.h"
#include "content/public/browser/service_process_host.h"
#include "content/public/common/result_codes.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace molt_ai {

namespace {

using Status = DocumentParseResult::Status;

// Shown in the OS task manager for the parser processes.
constexpr char16_t kDisplayName[] = u"MoltBrowser Document Parser";

mojo::PendingRemote<mojom::DocumentParser> LaunchInUtilityProcess(
    DocumentParserClient::ProcessCallback on_launched) {
  mojo::PendingRemote<mojom::DocumentParser> remote;
  content::ServiceProcessHost::Launch<mojom::DocumentParser>(
      remote.InitWithNewPipeAndPassReceiver(),
      content::ServiceProcessHost::Options()
          .WithDisplayName(std::u16string(kDisplayName))
          .WithProcessCallback(std::move(on_launched))
          .Pass());
  return remote;
}

DocumentParseResult Failed(std::string error) {
  return DocumentParseResult(Status::kFailed, std::string(), std::move(error));
}

// The utility process may be compromised: check everything it sends.
DocumentParseResult ValidateReply(mojom::ParseResultPtr reply,
                                  size_t max_chars) {
  if (!reply || !base::IsStringUTF8(reply->text) ||
      !base::IsStringUTF8(reply->error)) {
    return Failed("the document parser returned malformed output");
  }
  std::string error(
      base::TruncateUTF8ToByteSize(reply->error, mojom::kMaxErrorBytes));
  if (!reply->success) {
    return DocumentParseResult(Status::kNoText, std::string(),
                               std::move(error));
  }
  std::string text = std::move(reply->text);
  if (text.size() > max_chars) {
    text = std::string(base::TruncateUTF8ToByteSize(text, max_chars));
  }
  if (text.empty()) {
    return DocumentParseResult(Status::kNoText, std::string(),
                               std::move(error));
  }
  return DocumentParseResult(Status::kOk, std::move(text), std::string());
}

}  // namespace

DocumentParseResult::DocumentParseResult() = default;
DocumentParseResult::DocumentParseResult(Status status,
                                         std::string text,
                                         std::string error)
    : status(status), text(std::move(text)), error(std::move(error)) {}
DocumentParseResult::DocumentParseResult(const DocumentParseResult&) = default;
DocumentParseResult::DocumentParseResult(DocumentParseResult&&) = default;
DocumentParseResult& DocumentParseResult::operator=(
    const DocumentParseResult&) = default;
DocumentParseResult& DocumentParseResult::operator=(DocumentParseResult&&) =
    default;
DocumentParseResult::~DocumentParseResult() = default;

// The process of one call, for terminating it. Shared with the launch
// callback, which can run after its Call is gone (the client was destroyed,
// or the call timed out, before the launch finished): a kill requested before
// the launch happens as soon as the process exists.
class DocumentParserClient::ProcessKiller
    : public base::RefCounted<ProcessKiller> {
 public:
  ProcessKiller() = default;
  ProcessKiller(const ProcessKiller&) = delete;
  ProcessKiller& operator=(const ProcessKiller&) = delete;

  void OnLaunched(const base::Process& process) {
    if (!process.IsValid()) {
      return;
    }
    if (kill_exit_code_) {
      process.Terminate(*kill_exit_code_, /*wait=*/false);
      return;
    }
    process_ = process.Duplicate();
  }

  // Terminates the process (only ever once). Call it only while the call is
  // pending: the process can't have exited and had its id reused then,
  // since its exit would have disconnected the call first.
  void Kill(int exit_code) {
    if (kill_exit_code_) {
      return;
    }
    kill_exit_code_ = exit_code;
    if (process_.IsValid()) {
      process_.Terminate(exit_code, /*wait=*/false);
      process_.Close();
    }
  }

 private:
  friend class base::RefCounted<ProcessKiller>;
  ~ProcessKiller() = default;

  base::Process process_;
  std::optional<int> kill_exit_code_;
};

// One parse: its process, its pipe, its timeout and its caller's callback.
class DocumentParserClient::Call {
 public:
  Call(DocumentParserClient* owner, size_t max_chars, ResultCallback callback)
      : owner_(owner),
        max_chars_(std::min<size_t>(max_chars, mojom::kMaxOutputChars)),
        callback_(std::move(callback)),
        process_(base::MakeRefCounted<ProcessKiller>()) {}
  Call(const Call&) = delete;
  Call& operator=(const Call&) = delete;

  // A call destroyed before it was answered (its client is going away)
  // takes its process with it.
  ~Call() {
    if (callback_) {
      process_->Kill(content::RESULT_CODE_KILLED);
    }
  }

  // Launches the parser process and arms the timeout. False if the launcher
  // gave no pipe.
  bool Start(const LaunchCallback& launch, base::TimeDelta timeout) {
    remote_.Bind(
        launch.Run(base::BindOnce(&ProcessKiller::OnLaunched, process_)));
    if (!remote_.is_bound()) {
      return false;
    }
    // Unretained: |remote_| and |timer_| are owned by this call and drop
    // their callbacks when it is destroyed.
    remote_.set_disconnect_handler(
        base::BindOnce(&Call::OnDisconnected, base::Unretained(this)));
    timer_.Start(FROM_HERE, timeout,
                 base::BindOnce(&Call::OnTimeout, base::Unretained(this)));
    return true;
  }

  mojom::DocumentParser* parser() { return remote_.get(); }
  uint32_t max_chars() const { return static_cast<uint32_t>(max_chars_); }

  // The reply callback of every DocumentParser method.
  base::OnceCallback<void(mojom::ParseResultPtr)> ReplyCallback() {
    return base::BindOnce(&Call::OnReply, base::Unretained(this));
  }

  ResultCallback TakeCallback() { return std::move(callback_); }

 private:
  // Each of these ends the call: |this| is destroyed inside OnCallDone().
  void OnReply(mojom::ParseResultPtr reply) {
    owner_->OnCallDone(this, ValidateReply(std::move(reply), max_chars_));
  }

  void OnDisconnected() {
    owner_->OnCallDone(this,
                       Failed("the document parser stopped unexpectedly"));
  }

  void OnTimeout() {
    process_->Kill(content::RESULT_CODE_HUNG);
    owner_->OnCallDone(this, Failed("the document parser timed out"));
  }

  const raw_ptr<DocumentParserClient> owner_;
  const size_t max_chars_;
  ResultCallback callback_;
  const scoped_refptr<ProcessKiller> process_;
  mojo::Remote<mojom::DocumentParser> remote_;
  base::OneShotTimer timer_;
};

DocumentParserClient::DocumentParserClient()
    : DocumentParserClient(base::BindRepeating(&LaunchInUtilityProcess),
                           kTimeout) {}

DocumentParserClient::DocumentParserClient(LaunchCallback launch,
                                           base::TimeDelta timeout)
    : launch_(std::move(launch)), timeout_(timeout) {}

DocumentParserClient::~DocumentParserClient() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void DocumentParserClient::ExtractPdfText(base::span<const uint8_t> pdf,
                                          size_t max_chars,
                                          ResultCallback callback) {
  Call* call = StartCall(pdf.size(), max_chars, std::move(callback));
  if (call) {
    call->parser()->ExtractPdfText(mojo_base::BigBuffer(pdf), call->max_chars(),
                                   call->ReplyCallback());
  }
}

void DocumentParserClient::ConvertToMarkdown(base::span<const uint8_t> document,
                                             std::string_view extension,
                                             size_t max_chars,
                                             ResultCallback callback) {
  Call* call = StartCall(document.size(), max_chars, std::move(callback));
  if (call) {
    call->parser()->ConvertToMarkdown(mojo_base::BigBuffer(document),
                                      std::string(extension), call->max_chars(),
                                      call->ReplyCallback());
  }
}

void DocumentParserClient::ExtractDocxText(base::span<const uint8_t> docx,
                                           size_t max_chars,
                                           ResultCallback callback) {
  Call* call = StartCall(docx.size(), max_chars, std::move(callback));
  if (call) {
    call->parser()->ExtractDocxText(mojo_base::BigBuffer(docx),
                                    call->max_chars(), call->ReplyCallback());
  }
}

DocumentParserClient::Call* DocumentParserClient::StartCall(
    size_t document_size,
    size_t max_chars,
    ResultCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (document_size == 0) {
    Refuse(std::move(callback),
           DocumentParseResult(Status::kNoText, std::string(),
                               "the document is empty"));
    return nullptr;
  }
  if (document_size > mojom::kMaxDocumentBytes) {
    Refuse(std::move(callback),
           Failed("the document is too large to read (over 32 MiB)"));
    return nullptr;
  }
  if (calls_.size() >= kMaxConcurrentCalls) {
    Refuse(std::move(callback),
           Failed("too many documents are being read at once; try again"));
    return nullptr;
  }
  auto call = std::make_unique<Call>(this, max_chars, std::move(callback));
  if (!call->Start(launch_, timeout_)) {
    Refuse(call->TakeCallback(),
           Failed("the document parser could not be started"));
    return nullptr;
  }
  calls_.push_back(std::move(call));
  return calls_.back().get();
}

void DocumentParserClient::Refuse(ResultCallback callback,
                                  DocumentParseResult result) {
  // Never answer synchronously; and like every other answer, a refusal is
  // dropped if this client goes away first.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&DocumentParserClient::RunRefusal,
                                weak_ptr_factory_.GetWeakPtr(),
                                std::move(callback), std::move(result)));
}

void DocumentParserClient::RunRefusal(ResultCallback callback,
                                      DocumentParseResult result) {
  std::move(callback).Run(std::move(result));
}

void DocumentParserClient::OnCallDone(Call* call, DocumentParseResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = std::find_if(
      calls_.begin(), calls_.end(),
      [call](const std::unique_ptr<Call>& c) { return c.get() == call; });
  CHECK(it != calls_.end());
  std::unique_ptr<Call> done = std::move(*it);
  calls_.erase(it);
  ResultCallback callback = done->TakeCallback();
  // Closes the pipe: a parser that answered exits on its own (a hung one was
  // terminated by OnTimeout()).
  done.reset();
  // Last: the callback may start another call, or destroy this client.
  std::move(callback).Run(std::move(result));
}

}  // namespace molt_ai
