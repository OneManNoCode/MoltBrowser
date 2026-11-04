// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Two sequences for one set of MoltBrowser's files (settings.json, a
// storage directory, a profile's learning file and routines), so that a
// file system that stops answering (a network home directory whose server
// went away: stat() itself waits) can't hold shutdown with a read:
//
//   - changes: every write and read-modify-write, in order, BLOCK_SHUTDOWN
//     (a change posted before shutdown is written). Only changes run here,
//     so a change never waits behind a read.
//   - reads: what only reads, CONTINUE_ON_SHUTDOWN: shutdown doesn't wait
//     for a read that hangs, and a read cut short by the browser ending
//     loses nothing. A read that writes on the side (a migration's scrub,
//     a claim cut short settled, a temporary file of a crash cleaned) does
//     so only with steps a crash at any point leaves safe, as everywhere in
//     these files (an atomic rename, a delete).
//
// PostReadAfterChanges() runs a read once every change posted before it
// has run (a page reloaded after a save lists what it saved), then on the
// read sequence, and replies on the calling sequence.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_MOLT_FILE_SEQUENCES_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_MOLT_FILE_SEQUENCES_H_

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"

namespace molt_ai {

// The traits of each.
inline base::TaskTraits MoltFileChangeTraits() {
  return {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
          base::TaskShutdownBehavior::BLOCK_SHUTDOWN};
}
inline base::TaskTraits MoltFileReadTraits() {
  return {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
          base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN};
}

namespace internal {

template <typename R, typename ReplyArg>
void PostMoltFileRead(scoped_refptr<base::SequencedTaskRunner> reads,
                      const base::Location& from,
                      base::OnceCallback<R()> read,
                      base::OnceCallback<void(ReplyArg)> reply) {
  reads->PostTaskAndReplyWithResult(from, std::move(read), std::move(reply));
}

}  // namespace internal

// From a sequence: |read| on |reads| once every task posted to |changes|
// before has run, then |reply| with its result on the calling sequence.
template <typename R, typename ReplyArg>
void PostReadAfterChanges(scoped_refptr<base::SequencedTaskRunner> changes,
                          scoped_refptr<base::SequencedTaskRunner> reads,
                          const base::Location& from,
                          base::OnceCallback<R()> read,
                          base::OnceCallback<void(ReplyArg)> reply) {
  changes->PostTaskAndReply(
      from, base::DoNothing(),
      base::BindOnce(&internal::PostMoltFileRead<R, ReplyArg>, std::move(reads),
                     from, std::move(read), std::move(reply)));
}

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_MOLT_FILE_SEQUENCES_H_
