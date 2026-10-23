// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/gpu_inference_prefs.h"

#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include "components/prefs/pref_registry_simple.h"

namespace molt_ai {

namespace {

constexpr char kKey[] = "key";
constexpr char kName[] = "name";
constexpr char kFailures[] = "failures";
constexpr char kLastEvent[] = "last_event";
constexpr char kOff[] = "off";
constexpr char kOffSince[] = "off_since";
constexpr char kOffVersion[] = "off_version";
constexpr char kProbation[] = "probation";

// Milliseconds as a double (exact up to 2^53, far beyond any date).
base::Value TimeValue(int64_t ms) {
  return base::Value(static_cast<double>(ms));
}

// A stored time: a finite, non-negative double below 2^53; nullopt
// otherwise.
std::optional<int64_t> TimeFrom(const base::Value* value) {
  // GetIfDouble() also takes an int.
  const std::optional<double> d =
      value ? value->GetIfDouble() : std::nullopt;
  if (!d || !std::isfinite(*d) || *d < 0 || *d > 9007199254740992.0) {
    return std::nullopt;
  }
  return static_cast<int64_t>(*d);
}

}  // namespace

void RegisterGpuInferenceLocalStatePrefs(PrefRegistrySimple* registry) {
  registry->RegisterIntegerPref(kGpuInferenceModePref,
                                static_cast<int>(GpuInferenceMode::kAutomatic));
  registry->RegisterListPref(kGpuDriverRecordsPref);
}

GpuInferenceMode GpuInferenceModeFromPref(int value) {
  return value == static_cast<int>(GpuInferenceMode::kOff)
             ? GpuInferenceMode::kOff
             : GpuInferenceMode::kAutomatic;
}

base::ListValue GpuDriverEntriesToPref(
    const std::vector<GpuDriverEntry>& entries) {
  base::ListValue list;
  for (const GpuDriverEntry& entry : entries) {
    base::DictValue dict;
    dict.Set(kKey, GpuDriverKeyToString(entry.key));
    dict.Set(kName, entry.name);
    base::ListValue failures;
    for (int64_t t : entry.failure_ms) failures.Append(TimeValue(t));
    dict.Set(kFailures, std::move(failures));
    dict.Set(kLastEvent, TimeValue(entry.last_event_ms));
    dict.Set(kOff, entry.turned_off);
    dict.Set(kOffSince, TimeValue(entry.turned_off_ms));
    dict.Set(kOffVersion, entry.turned_off_version);
    dict.Set(kProbation, entry.probation);
    list.Append(std::move(dict));
  }
  return list;
}

std::vector<GpuDriverEntry> GpuDriverEntriesFromPref(
    const base::ListValue& list) {
  std::vector<GpuDriverEntry> entries;
  for (const base::Value& item : list) {
    if (entries.size() >= GpuDriverRecords::kMaxEntries) break;
    const base::DictValue* dict = item.GetIfDict();
    if (!dict) continue;
    const std::string* key_text = dict->FindString(kKey);
    std::optional<GpuDriverKey> key =
        key_text ? GpuDriverKeyFromString(*key_text) : std::nullopt;
    if (!key) continue;
    GpuDriverEntry entry;
    entry.key = *key;
    if (const std::string* name = dict->FindString(kName)) {
      entry.name = SanitizeGpuName(*name);
    }
    if (const base::ListValue* failures = dict->FindList(kFailures)) {
      for (const base::Value& t : *failures) {
        if (std::optional<int64_t> ms = TimeFrom(&t)) {
          entry.failure_ms.push_back(*ms);
        }
        if (entry.failure_ms.size() > GpuDriverRecords::kFailuresToTurnOff) {
          entry.failure_ms.erase(entry.failure_ms.begin());
        }
      }
    }
    entry.last_event_ms = TimeFrom(dict->Find(kLastEvent)).value_or(0);
    entry.turned_off = dict->FindBool(kOff).value_or(false);
    entry.turned_off_ms = TimeFrom(dict->Find(kOffSince)).value_or(0);
    if (const std::string* version = dict->FindString(kOffVersion)) {
      entry.turned_off_version = SanitizeVersion(*version);
    }
    entry.probation = dict->FindBool(kProbation).value_or(false);
    entries.push_back(std::move(entry));
  }
  return entries;
}

}  // namespace molt_ai
