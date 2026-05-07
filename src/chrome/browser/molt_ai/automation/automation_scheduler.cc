// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_scheduler.h"

#include <algorithm>
#include <ctime>
#include <set>
#include <sstream>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"

namespace molt_ai {
namespace automation {

namespace {

// Parse a single cron field into a sorted set of allowed values in the
// inclusive range [min_val, max_val]. Returns false on malformed input.
bool ParseCronField(const std::string& field, int min_val, int max_val,
                    std::set<int>* out) {
  out->clear();
  if (field.empty()) return false;

  // Split by comma (lists).
  for (const auto& piece :
       base::SplitString(field, ",", base::TRIM_WHITESPACE,
                         base::SPLIT_WANT_NONEMPTY)) {
    int step = 1;
    std::string range = piece;
    auto slash = piece.find('/');
    if (slash != std::string::npos) {
      range = piece.substr(0, slash);
      if (!base::StringToInt(piece.substr(slash + 1), &step) || step <= 0)
        return false;
    }
    int lo = min_val, hi = max_val;
    if (range == "*") {
      // already lo..hi
    } else if (range.find('-') != std::string::npos) {
      auto dash = range.find('-');
      if (!base::StringToInt(range.substr(0, dash), &lo) ||
          !base::StringToInt(range.substr(dash + 1), &hi)) {
        return false;
      }
    } else {
      if (!base::StringToInt(range, &lo)) return false;
      hi = lo;
    }
    if (lo < min_val || hi > max_val || lo > hi) return false;
    for (int v = lo; v <= hi; v += step)
      out->insert(v);
  }
  return !out->empty();
}

// Increment |t| by one minute and re-explode.
struct ExplodedLocal {
  int year, month, mday, hour, minute, second, wday;
};

ExplodedLocal Explode(const std::tm& tm) {
  return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
          tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday};
}

}  // namespace

base::Time NextCronFire(const std::string& expression,
                         const std::string& /*timezone*/,
                         base::Time now) {
  // Parse 5 fields: minute hour mday month wday.
  auto fields = base::SplitString(expression, " ", base::TRIM_WHITESPACE,
                                   base::SPLIT_WANT_NONEMPTY);
  if (fields.size() != 5)
    return base::Time();

  std::set<int> minutes, hours, mdays, months, wdays;
  if (!ParseCronField(fields[0], 0, 59, &minutes)) return base::Time();
  if (!ParseCronField(fields[1], 0, 23, &hours))   return base::Time();
  if (!ParseCronField(fields[2], 1, 31, &mdays))   return base::Time();
  if (!ParseCronField(fields[3], 1, 12, &months))  return base::Time();
  if (!ParseCronField(fields[4], 0, 6,  &wdays))   return base::Time();

  // Walk forward minute-by-minute until all five fields match. Cap at
  // ~365 days so a malformed expression doesn't loop forever.
  std::time_t t = static_cast<std::time_t>(
      now.InSecondsFSinceUnixEpoch()) + 60;  // start from next minute
  // Round down to whole minute.
  t = (t / 60) * 60;
  const std::time_t kCap = t + 366 * 24 * 60 * 60;
  while (t < kCap) {
    std::tm local{};
    localtime_r(&t, &local);
    auto ex = Explode(local);
    if (minutes.count(ex.minute) && hours.count(ex.hour) &&
        mdays.count(ex.mday) && months.count(ex.month) &&
        wdays.count(ex.wday)) {
      return base::Time::FromTimeT(t);
    }
    t += 60;
  }
  return base::Time();
}

base::Time AutomationScheduler::NextFireTime(const Trigger& trigger,
                                              base::Time now) {
  if (trigger.until_unix > 0 &&
      now.InSecondsFSinceUnixEpoch() > trigger.until_unix) {
    return base::Time();
  }
  switch (trigger.type) {
    case TriggerType::MANUAL:
      return base::Time();  // never auto-fires

    case TriggerType::CRON:
      return NextCronFire(trigger.expression, trigger.timezone, now);

    case TriggerType::INTERVAL: {
      int seconds = 0;
      if (!base::StringToInt(trigger.expression, &seconds) || seconds <= 0)
        return base::Time();
      base::Time base_time = trigger.last_fired_unix > 0
          ? base::Time::FromTimeT(trigger.last_fired_unix)
          : now;
      return base_time + base::Seconds(seconds);
    }

    case TriggerType::AT: {
      // ISO-8601 — use a minimal parser (YYYY-MM-DDTHH:MM:SS optionally Z).
      base::Time t;
      if (!base::Time::FromString(trigger.expression.c_str(), &t))
        return base::Time();
      return t > now ? t : base::Time();
    }

    case TriggerType::ON_EVENT:
      return base::Time();  // event-driven, not time-driven
  }
  return base::Time();
}

AutomationScheduler::AutomationScheduler() = default;
AutomationScheduler::~AutomationScheduler() { Stop(); }

void AutomationScheduler::SetStorage(AutomationStorage* storage) {
  storage_ = storage;
}

void AutomationScheduler::SetFireCallback(ScheduledFireCallback cb) {
  fire_cb_ = std::move(cb);
}

void AutomationScheduler::Start() {
  if (started_)
    return;
  started_ = true;

  // Heartbeat every 60s catches sleep wake-ups + missed timers on
  // Windows/Linux where OneShotTimer alone can drift.
  heartbeat_.Start(FROM_HERE, base::Seconds(60),
                   base::BindRepeating(&AutomationScheduler::Tick,
                                        weak_factory_.GetWeakPtr()));
  Tick();
}

void AutomationScheduler::Stop() {
  next_timer_.Stop();
  heartbeat_.Stop();
  started_ = false;
}

void AutomationScheduler::Reschedule() {
  if (started_)
    Tick();
}

void AutomationScheduler::Tick() {
  ArmNext(base::Time::Now());
}

void AutomationScheduler::TickForTesting(base::Time now) {
  ArmNext(now);
}

void AutomationScheduler::ArmNext(base::Time now) {
  if (!storage_)
    return;

  std::vector<Script> scripts = storage_->ListAll();
  base::Time soonest;
  Script soonest_script;

  for (const auto& s : scripts) {
    if (!s.enabled) continue;

    // Catch-up: cron triggers that should have fired during shutdown.
    base::Time next = NextFireTime(s.trigger, now);

    // If trigger has never fired and start is in the past, fire it now.
    // (Only for CRON / INTERVAL — ON_EVENT and MANUAL never auto-fire.)
    bool catchup = false;
    if (s.trigger.type == TriggerType::CRON ||
        s.trigger.type == TriggerType::INTERVAL) {
      // If a cron expression matches a moment in the past since
      // last_fired_unix, run it now.
      if (s.trigger.last_fired_unix > 0) {
        base::Time last_fired =
            base::Time::FromTimeT(s.trigger.last_fired_unix);
        base::Time would_have_fired =
            NextFireTime(s.trigger, last_fired);
        if (would_have_fired.is_null() == false &&
            would_have_fired < now) {
          catchup = true;
        }
      }
    }

    if (catchup) {
      if (fire_cb_) fire_cb_.Run(s, now);
      // After catch-up, recompute next.
      next = NextFireTime(s.trigger, now);
    }

    if (next.is_null()) continue;

    if (soonest.is_null() || next < soonest) {
      soonest = next;
      soonest_script = s;
    }
  }

  if (soonest.is_null()) {
    next_timer_.Stop();
    return;
  }

  base::TimeDelta delay = soonest - now;
  if (delay.is_negative() || delay.is_zero())
    delay = base::Milliseconds(1);

  // Cap at 1 hour so we re-evaluate frequently for daylight saving / sleep.
  if (delay > base::Hours(1)) delay = base::Hours(1);

  next_timer_.Stop();
  next_timer_.Start(
      FROM_HERE, delay,
      base::BindOnce(
          [](base::WeakPtr<AutomationScheduler> self, Script script) {
            if (!self) return;
            if (self->fire_cb_) self->fire_cb_.Run(script, base::Time::Now());
            self->Tick();
          },
          weak_factory_.GetWeakPtr(), soonest_script));
}

}  // namespace automation
}  // namespace molt_ai
