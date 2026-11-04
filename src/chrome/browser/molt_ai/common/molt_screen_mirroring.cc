// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/molt_screen_mirroring.h"

#include <set>

#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/threading/thread_checker.h"

namespace molt_ai {

namespace {

struct Mirroring {
  // The UI thread (a thread, not a sequence: tests make new sequences on it).
  THREAD_CHECKER(thread_checker);
  std::set<const void*> sessions;
  base::ObserverList<ScreenMirroringObserver> observers;
};

Mirroring& Get() {
  static base::NoDestructor<Mirroring> mirroring;
  return *mirroring;
}

}  // namespace

void NoteScreenMirroring(const void* session, bool mirroring) {
  Mirroring& state = Get();
  DCHECK_CALLED_ON_VALID_THREAD(state.thread_checker);
  if (!session) {
    return;
  }
  const bool before = !state.sessions.empty();
  if (mirroring) {
    state.sessions.insert(session);
  } else {
    state.sessions.erase(session);
  }
  if (before != !state.sessions.empty()) {
    for (ScreenMirroringObserver& observer : state.observers) {
      observer.OnScreenMirroringChanged();
    }
  }
}

bool IsMirroringScreen() {
  Mirroring& state = Get();
  DCHECK_CALLED_ON_VALID_THREAD(state.thread_checker);
  return !state.sessions.empty();
}

void AddScreenMirroringObserver(ScreenMirroringObserver* observer) {
  Mirroring& state = Get();
  DCHECK_CALLED_ON_VALID_THREAD(state.thread_checker);
  state.observers.AddObserver(observer);
}

void RemoveScreenMirroringObserver(ScreenMirroringObserver* observer) {
  Mirroring& state = Get();
  DCHECK_CALLED_ON_VALID_THREAD(state.thread_checker);
  state.observers.RemoveObserver(observer);
}

}  // namespace molt_ai
