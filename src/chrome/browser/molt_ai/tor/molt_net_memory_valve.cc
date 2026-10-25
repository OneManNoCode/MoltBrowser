// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_memory_valve.h"

#include <memory>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/supports_user_data.h"
#include "base/system/sys_info.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/molt_ai/tor/molt_net_memory_valve_policy.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"

namespace molt_ai::tor {

// Defined in molt_net_window.cc, with the rest of how an identity ends.
void EndMoltNetIdentityForMemory(const base::FilePath& path,
                                 const Profile::OTRProfileID& id);

namespace {

uint64_t ReadAvailablePhysicalMemory() {
  return base::SysInfo::AmountOfAvailablePhysicalMemory().InBytes();
}

const char kMoltNetMemoryValveKey[] = "MoltNetMemoryValve";

struct ValveHolder : public base::SupportsUserData::Data {
  explicit ValveHolder(scoped_refptr<MoltNetMemoryValve> valve)
      : valve(std::move(valve)) {}
  scoped_refptr<MoltNetMemoryValve> valve;
};

int64_t NowUs() {
  return (base::TimeTicks::Now() - base::TimeTicks()).InMicroseconds();
}

}  // namespace

MoltNetMemoryValve::MoltNetMemoryValve(Profile* moltnet_profile,
                                       uint64_t reserve,
                                       ReadAvailableMemory read,
                                       EndIdentity end)
    : path_(moltnet_profile->GetOriginalProfile()->GetPath()),
      id_(moltnet_profile->GetOTRProfileID()),
      reserve_(reserve),
      read_(read ? read : &ReadAvailablePhysicalMemory),
      end_(end ? end : &EndMoltNetIdentityForMemory),
      ui_task_runner_(base::SequencedTaskRunner::GetCurrentDefault()) {}

MoltNetMemoryValve::~MoltNetMemoryValve() = default;

void MoltNetMemoryValve::OnGrowthBeyondFloor() {
  const int64_t now = NowUs();
  if (!MoltNetMemoryValveReads(ended_.load(), reading_.load(),
                               (now - last_reading_us_.load()) / 1000)) {
    return;
  }
  bool expected = false;
  if (!reading_.compare_exchange_strong(expected, true)) {
    return;
  }
  last_reading_us_.store(now);
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&MoltNetMemoryValve::Read, base::WrapRefCounted(this)));
}

void MoltNetMemoryValve::Read() {
  if (MoltNetMemoryValveEnds(read_(), reserve_) && !ended_.exchange(true)) {
    ui_task_runner_->PostTask(FROM_HERE, base::BindOnce(end_, path_, id_));
  }
  reading_.store(false);
}

base::RepeatingClosure MoltNetMemoryValveCallback(Profile* profile,
                                                  uint64_t reserve) {
  if (!profile || !IsMoltNetProfile(profile)) {
    return base::RepeatingClosure();
  }
  auto* holder =
      static_cast<ValveHolder*>(profile->GetUserData(kMoltNetMemoryValveKey));
  if (!holder) {
    auto created = std::make_unique<ValveHolder>(
        base::MakeRefCounted<MoltNetMemoryValve>(profile, reserve));
    holder = created.get();
    profile->SetUserData(kMoltNetMemoryValveKey, std::move(created));
  }
  return base::BindRepeating(&MoltNetMemoryValve::OnGrowthBeyondFloor,
                             holder->valve);
}

}  // namespace molt_ai::tor
