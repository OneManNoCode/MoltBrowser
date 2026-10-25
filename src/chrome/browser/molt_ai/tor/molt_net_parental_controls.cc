// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_parental_controls.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/tor/molt_net_live_policy.h"

#if BUILDFLAG(IS_WIN)
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/win/parental_controls.h"
#endif

namespace molt_ai {
namespace tor {

void CheckMoltNetParentalControls(
    base::OnceCallback<void(bool allowed)> done) {
#if BUILDFLAG(IS_WIN)
  // As InitializeWinParentalControls() reads them: COM, may block.
  base::ThreadPool::CreateCOMSTATaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_BLOCKING})
      ->PostTaskAndReplyWithResult(
          FROM_HERE, base::BindOnce([] {
            return std::optional<bool>(
                MoltReadWinParentalControlsNow().logging_required);
          }),
          base::BindOnce(
              [](base::OnceCallback<void(bool)> then,
                 std::optional<bool> logging_required_now) {
                std::move(then).Run(
                    MoltNetParentalControlsAllowOpen(logging_required_now));
              },
              std::move(done)));
#else
  std::move(done).Run(MoltNetParentalControlsAllowOpen(std::nullopt));
#endif
}

}  // namespace tor
}  // namespace molt_ai
