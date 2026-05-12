// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/agent_inbox_registry.h"

#include <ctime>
#include <utility>

#include "base/no_destructor.h"

namespace molt_ai {
namespace automation {

AgentInboxEntry::AgentInboxEntry() = default;
AgentInboxEntry::AgentInboxEntry(const AgentInboxEntry&) = default;
AgentInboxEntry::AgentInboxEntry(AgentInboxEntry&&) = default;
AgentInboxEntry& AgentInboxEntry::operator=(const AgentInboxEntry&) = default;
AgentInboxEntry& AgentInboxEntry::operator=(AgentInboxEntry&&) = default;
AgentInboxEntry::~AgentInboxEntry() = default;

// static
AgentInboxRegistry* AgentInboxRegistry::Get() {
  static base::NoDestructor<AgentInboxRegistry> instance;
  return instance.get();
}

AgentInboxRegistry::AgentInboxRegistry() = default;
AgentInboxRegistry::~AgentInboxRegistry() = default;

int64_t AgentInboxRegistry::NewId() {
  return next_id_++;
}

void AgentInboxRegistry::Add(AgentInboxEntry entry) {
  const int64_t id = entry.id;
  if (id <= 0) {
    return;
  }
  entries_[id] = std::move(entry);
}

void AgentInboxRegistry::UpdateStep(int64_t id,
                                    int current_step,
                                    std::string note) {
  auto it = entries_.find(id);
  if (it == entries_.end()) {
    return;
  }
  it->second.current_step = current_step;
  it->second.status_note = std::move(note);
}

void AgentInboxRegistry::Finish(int64_t id, bool succeeded) {
  auto it = entries_.find(id);
  if (it == entries_.end()) {
    return;
  }
  it->second.succeeded = succeeded;
  // Set finished_at to now (seconds since epoch).
  it->second.finished_at_unix =
      static_cast<int64_t>(time(nullptr));
}

void AgentInboxRegistry::Remove(int64_t id) {
  entries_.erase(id);
}

std::vector<AgentInboxEntry> AgentInboxRegistry::List() const {
  std::vector<AgentInboxEntry> out;
  out.reserve(entries_.size());
  for (const auto& kv : entries_) {
    out.push_back(kv.second);
  }
  return out;
}

}  // namespace automation
}  // namespace molt_ai
