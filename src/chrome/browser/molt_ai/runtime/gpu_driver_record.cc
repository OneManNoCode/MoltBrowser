// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/gpu_driver_record.h"

#include <algorithm>
#include <utility>

namespace molt_ai {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr char kKeyPrefix[] = "vk:";
constexpr char kUnidentified[] = "unidentified";
// "vk:" + four 8-digit fields and the 32-digit UUID, ':'-separated.
constexpr size_t kKeyTextBytes = 3 + 4 * 9 + 32;

// VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS.
constexpr uint32_t kDriverIntelWindows = 5;
constexpr uint32_t kVendorNvidia = 0x10de;
constexpr uint32_t kVendorIntel = 0x8086;

void AppendHex32(uint32_t value, std::string* out) {
  for (int shift = 28; shift >= 0; shift -= 4) {
    out->push_back(kHexDigits[(value >> shift) & 0xf]);
  }
}

std::optional<uint32_t> HexDigit(char c) {
  if (c >= '0' && c <= '9') return static_cast<uint32_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint32_t>(c - 'a' + 10);
  return std::nullopt;
}

// Exactly 8 lowercase hex digits.
std::optional<uint32_t> ParseHex32(std::string_view text) {
  if (text.size() != 8) return std::nullopt;
  uint32_t value = 0;
  for (char c : text) {
    const std::optional<uint32_t> digit = HexDigit(c);
    if (!digit) return std::nullopt;
    value = value << 4 | *digit;
  }
  return value;
}

bool IsNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == ' ' || c == '(' || c == ')' ||
         c == '+' || c == '-' || c == '.' || c == '_' || c == '/' ||
         c == ',' || c == '[' || c == ']';
}

// Age of |then| at |now|; 0 if the clock went back.
int64_t Age(int64_t then, int64_t now) {
  return now > then ? now - then : 0;
}

}  // namespace

GpuDriverKey UnidentifiedGpuDriverKey() {
  return GpuDriverKey();
}

std::string GpuDriverKeyToString(const GpuDriverKey& key) {
  if (!key.identified) return kUnidentified;
  std::string out = kKeyPrefix;
  out.reserve(kKeyTextBytes);
  for (uint32_t field : {key.vendor_id, key.device_id, key.driver_id,
                         key.driver_version}) {
    AppendHex32(field, &out);
    out.push_back(':');
  }
  for (uint8_t byte : key.driver_uuid) {
    out.push_back(kHexDigits[byte >> 4]);
    out.push_back(kHexDigits[byte & 0xf]);
  }
  return out;
}

std::optional<GpuDriverKey> GpuDriverKeyFromString(std::string_view text) {
  if (text == kUnidentified) return UnidentifiedGpuDriverKey();
  if (text.size() != kKeyTextBytes || text.substr(0, 3) != kKeyPrefix) {
    return std::nullopt;
  }
  GpuDriverKey key;
  key.identified = true;
  uint32_t* const fields[] = {&key.vendor_id, &key.device_id, &key.driver_id,
                              &key.driver_version};
  size_t pos = 3;
  for (uint32_t* field : fields) {
    const std::optional<uint32_t> value = ParseHex32(text.substr(pos, 8));
    if (!value || text[pos + 8] != ':') return std::nullopt;
    *field = *value;
    pos += 9;
  }
  for (size_t i = 0; i < key.driver_uuid.size(); ++i) {
    const std::optional<uint32_t> high = HexDigit(text[pos + 2 * i]);
    const std::optional<uint32_t> low = HexDigit(text[pos + 2 * i + 1]);
    if (!high || !low) return std::nullopt;
    key.driver_uuid[i] = static_cast<uint8_t>(*high << 4 | *low);
  }
  return key;
}

std::string SanitizeGpuName(std::string_view raw) {
  std::string out;
  for (char c : raw) {
    if (out.size() >= kMaxGpuDisplayNameBytes) break;
    if (c == '\0') break;  // a C string's end
    if (!IsNameChar(c)) continue;
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out.push_back(c);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string SanitizeVersion(std::string_view version) {
  if (version.empty() || version.size() > kMaxVersionBytes) return "";
  for (char c : version) {
    if (!(c >= '0' && c <= '9') && c != '.') return "";
  }
  return std::string(version);
}

std::string FormatGpuDriverVersion(uint32_t vendor_id,
                                   uint32_t driver_id,
                                   uint32_t v) {
  auto n = [](uint32_t x) { return std::to_string(x); };
  if (vendor_id == kVendorNvidia) {
    return n(v >> 22) + "." + n((v >> 14) & 0xff) + "." + n((v >> 6) & 0xff) +
           "." + n(v & 0x3f);
  }
  if (vendor_id == kVendorIntel && driver_id == kDriverIntelWindows) {
    return n(v >> 14) + "." + n(v & 0x3fff);
  }
  return n(v >> 22) + "." + n((v >> 12) & 0x3ff) + "." + n(v & 0xfff);
}

GpuDriverEntry::GpuDriverEntry() = default;
GpuDriverEntry::~GpuDriverEntry() = default;
GpuDriverEntry::GpuDriverEntry(const GpuDriverEntry&) = default;
GpuDriverEntry& GpuDriverEntry::operator=(const GpuDriverEntry&) = default;
GpuDriverEntry::GpuDriverEntry(GpuDriverEntry&&) = default;
GpuDriverEntry& GpuDriverEntry::operator=(GpuDriverEntry&&) = default;

GpuDriverRecords::GpuDriverRecords() = default;
GpuDriverRecords::~GpuDriverRecords() = default;
GpuDriverRecords::GpuDriverRecords(const GpuDriverRecords&) = default;
GpuDriverRecords& GpuDriverRecords::operator=(const GpuDriverRecords&) =
    default;

GpuDriverEntry* GpuDriverRecords::Find(const GpuDriverKey& key) {
  for (GpuDriverEntry& entry : entries_) {
    if (entry.key == key) return &entry;
  }
  return nullptr;
}

// static
bool GpuDriverRecords::MaybeRetry(GpuDriverEntry* entry,
                                  int64_t now_ms,
                                  std::string_view browser_version) {
  if (!entry->turned_off) return false;
  const bool by_time = Age(entry->turned_off_ms, now_ms) >= kRetryAfterMs;
  const std::string version = SanitizeVersion(browser_version);
  const bool by_update = !version.empty() &&
                         !entry->turned_off_version.empty() &&
                         version != entry->turned_off_version;
  if (!by_time && !by_update) return false;
  entry->turned_off = false;
  entry->probation = true;
  entry->failure_ms.clear();
  entry->last_event_ms = now_ms;
  return true;
}

void GpuDriverRecords::Expire(int64_t now_ms) {
  std::erase_if(entries_, [&](const GpuDriverEntry& entry) {
    return !entry.turned_off &&
           Age(entry.last_event_ms, now_ms) >= kFailureMemoryMs;
  });
}

bool GpuDriverRecords::AllowGpu(const GpuDriverKey& key,
                                int64_t now_ms,
                                std::string_view browser_version,
                                bool* changed) {
  bool did_change = false;
  GpuDriverEntry* entry = Find(key);
  if (entry) did_change = MaybeRetry(entry, now_ms, browser_version);
  const bool allow = !entry || !entry->turned_off;
  if (changed) *changed = did_change;
  return allow;
}

bool GpuDriverRecords::RecordFailure(const GpuDriverKey& key,
                                     std::string_view name,
                                     int64_t now_ms,
                                     std::string_view browser_version) {
  Expire(now_ms);
  GpuDriverEntry* entry = Find(key);
  if (!entry) {
    if (entries_.size() >= kMaxEntries) {
      // Room for it: the oldest entry not turned off, else the one turned
      // off longest ago.
      auto oldest = entries_.end();
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->turned_off) continue;
        if (oldest == entries_.end() ||
            it->last_event_ms < oldest->last_event_ms) {
          oldest = it;
        }
      }
      if (oldest == entries_.end()) {
        oldest = std::min_element(
            entries_.begin(), entries_.end(),
            [](const GpuDriverEntry& a, const GpuDriverEntry& b) {
              return a.turned_off_ms < b.turned_off_ms;
            });
      }
      entries_.erase(oldest);
    }
    entries_.emplace_back();
    entry = &entries_.back();
    entry->key = key;
  }
  const std::string sanitized = SanitizeGpuName(name);
  if (!sanitized.empty()) entry->name = sanitized;
  entry->last_event_ms = now_ms;
  if (entry->turned_off) {
    return false;  // already off (a failure while it was being skipped)
  }
  // Only failures within kFailureMemoryMs count.
  std::erase_if(entry->failure_ms, [&](int64_t t) {
    return Age(t, now_ms) >= kFailureMemoryMs;
  });
  entry->failure_ms.push_back(now_ms);
  if (entry->failure_ms.size() > kFailuresToTurnOff) {
    entry->failure_ms.erase(entry->failure_ms.begin());
  }
  if (!entry->probation && entry->failure_ms.size() < kFailuresToTurnOff) {
    return false;
  }
  entry->turned_off = true;
  entry->probation = false;
  entry->turned_off_ms = now_ms;
  entry->turned_off_version = SanitizeVersion(browser_version);
  return true;
}

std::vector<GpuDriverEntry> GpuDriverRecords::TurnedOff(
    int64_t now_ms,
    std::string_view browser_version,
    bool* changed) {
  bool did_change = false;
  for (GpuDriverEntry& entry : entries_) {
    did_change = MaybeRetry(&entry, now_ms, browser_version) || did_change;
  }
  const size_t before = entries_.size();
  Expire(now_ms);
  did_change = did_change || entries_.size() != before;
  std::vector<GpuDriverEntry> off;
  for (const GpuDriverEntry& entry : entries_) {
    if (entry.turned_off) off.push_back(entry);
  }
  if (changed) *changed = did_change;
  return off;
}

bool GpuDriverRecords::RetryAll(int64_t now_ms) {
  bool any = false;
  for (GpuDriverEntry& entry : entries_) {
    if (!entry.turned_off) continue;
    entry.turned_off = false;
    entry.probation = true;
    entry.failure_ms.clear();
    entry.last_event_ms = now_ms;
    any = true;
  }
  return any;
}

void GpuDriverRecords::Restore(std::vector<GpuDriverEntry> entries,
                               int64_t now_ms) {
  entries_.clear();
  auto clamp_time = [&](int64_t t) {
    return t > now_ms ? now_ms : t;  // ahead of the clock: now
  };
  for (GpuDriverEntry& entry : entries) {
    if (entries_.size() >= kMaxEntries) break;
    // An identified key round-trips through its text form; an unidentified
    // one has nothing else set.
    if (!entry.key.identified && !(entry.key == UnidentifiedGpuDriverKey())) {
      continue;
    }
    if (Find(entry.key)) continue;  // a duplicate
    GpuDriverEntry clean;
    clean.key = entry.key;
    clean.name = SanitizeGpuName(entry.name);
    for (int64_t t : entry.failure_ms) {
      if (t > 0) clean.failure_ms.push_back(clamp_time(t));
    }
    std::sort(clean.failure_ms.begin(), clean.failure_ms.end());
    if (clean.failure_ms.size() > kFailuresToTurnOff) {
      clean.failure_ms.erase(
          clean.failure_ms.begin(),
          clean.failure_ms.end() - static_cast<ptrdiff_t>(kFailuresToTurnOff));
    }
    clean.last_event_ms =
        entry.last_event_ms > 0 ? clamp_time(entry.last_event_ms) : now_ms;
    clean.turned_off = entry.turned_off;
    if (clean.turned_off) {
      clean.turned_off_ms =
          entry.turned_off_ms > 0 ? clamp_time(entry.turned_off_ms) : now_ms;
      clean.turned_off_version = SanitizeVersion(entry.turned_off_version);
    }
    clean.probation = !clean.turned_off && entry.probation;
    entries_.push_back(std::move(clean));
  }
  Expire(now_ms);
}

void GpuDriverRecords::Clear() {
  entries_.clear();
}

}  // namespace molt_ai
