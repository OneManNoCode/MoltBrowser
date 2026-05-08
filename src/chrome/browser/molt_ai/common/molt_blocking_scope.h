// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ScopedAllowBlockingForMolt — RAII helper that lets MoltBrowser code
// perform synchronous file I/O on the UI thread. Required because
// content::BrowserMainLoop calls base::DisallowUnresponsiveTasks() at
// the end of PreMainMessageLoopRun, after which any blocking call
// from the UI thread FATAL-DCHECKs.
//
// We use this for the small per-user JSON files that back
// settings.json, automations/*.molt, persona files, and memory dumps.
// Each is at most a few KB and reads in microseconds, so the standard
// "post to thread pool" pattern would be over-engineered for a
// hobbyist-scale fork. The friend declaration that lets this class
// reach base::ScopedAllowBlocking lives in
// base/threading/thread_restrictions.h.
//
// Use it as a stack guard at the top of any function that does
// synchronous file I/O:
//
//   void MyHandler::HandleGet(const base::ListValue&) {
//     ScopedAllowBlockingForMolt allow_blocking;
//     base::ReadFileToString(path, &contents);
//     ...
//   }

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_MOLT_BLOCKING_SCOPE_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_MOLT_BLOCKING_SCOPE_H_

#include "base/threading/thread_restrictions.h"

class ScopedAllowBlockingForMolt : public base::ScopedAllowBlocking {};

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_MOLT_BLOCKING_SCOPE_H_
