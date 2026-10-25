// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// One click allows one thing.
//
// A message from one of MoltBrowser's own pages that allows something (an
// agent's "Allow", running a proposal the user confirmed, changing a
// workflow's trust or confirming its sites, saving a run, changing a
// security setting, opening a MoltNet window from the onion page) counts
// only with the click itself: the page frame's transient user activation,
// which its script can't make up (the click's activation reaches the
// browser before the message: the page's message pipe is frame-associated).
// The activation is consumed, so a script that sends several such messages
// after one click (on the button, or anywhere else on the page) gets one of
// them through at most.
//
// Header-only and std-only (a template over the frame type), so
// molt_ai_unittests tests it with a fake frame.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_MOLT_USER_CLICK_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_MOLT_USER_CLICK_H_

namespace molt_ai {

// Whether an action |allow|ed by the page goes ahead: only with the frame's
// transient user activation, which this consumes. |frame| is the page's
// content::RenderFrameHost (or null), anything with
// ConsumeTransientUserActivation(). Nothing is consumed unless |allow|.
template <typename Frame>
[[nodiscard]] bool ConsumeUserClick(bool allow, Frame* frame) {
  return allow && frame && frame->ConsumeTransientUserActivation();
}

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_MOLT_USER_CLICK_H_
