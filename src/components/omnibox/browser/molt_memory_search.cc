// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "components/omnibox/browser/molt_memory_search.h"

#include <map>

#include "base/check.h"
#include "base/no_destructor.h"

namespace {

// PrefService -> its profile's memory search. A function-local map (not a
// class field) of pointers that are only compared and handed back while
// registered.
std::map<const PrefService*, MoltMemorySearch*>& Registry() {
  static base::NoDestructor<std::map<const PrefService*, MoltMemorySearch*>>
      registry;
  return *registry;
}

}  // namespace

MoltMemorySearch::Results::Results() = default;
MoltMemorySearch::Results::~Results() = default;
MoltMemorySearch::Results::Results(Results&&) = default;
MoltMemorySearch::Results& MoltMemorySearch::Results::operator=(Results&&) =
    default;
MoltMemorySearch::Results::Results(const Results&) = default;
MoltMemorySearch::Results& MoltMemorySearch::Results::operator=(
    const Results&) = default;

// static
MoltMemorySearch* MoltMemorySearch::ForPrefs(const PrefService* prefs) {
  if (!prefs) {
    return nullptr;
  }
  auto it = Registry().find(prefs);
  return it == Registry().end() ? nullptr : it->second;
}

// static
void MoltMemorySearch::Register(const PrefService* prefs,
                                MoltMemorySearch* search) {
  CHECK(prefs);
  CHECK(search);
  Registry()[prefs] = search;
}

// static
void MoltMemorySearch::Unregister(const PrefService* prefs,
                                  MoltMemorySearch* search) {
  auto it = Registry().find(prefs);
  if (it != Registry().end() && it->second == search) {
    Registry().erase(it);
  }
}
