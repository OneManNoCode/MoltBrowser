// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShield's on-disk state, browser-wide, under
// <user-data-dir>/MoltShield/:
//   state.json      settings + per-list download state (ETag, sha256, ...)
//   lists/<id>.txt  last good copy of each filter list
//   engine.dat      serialized adblock-rust engine (fast startup)
//   engine.json     what engine.dat was built from (format key + list hashes)
//
// Parsing/serialization is pure and unit-tested; Load*/Save*/Read* do
// blocking I/O and must run on a MayBlock sequence.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_STORE_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_STORE_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "base/values.h"

namespace molt_ai::shield {

// Download/cache state of one list.
struct ListState {
  ListState();
  ListState(const ListState&);
  ListState& operator=(const ListState&);
  ~ListState();

  std::string etag;
  std::string last_modified;  // HTTP Last-Modified, for If-Modified-Since.
  std::string sha256_hex;     // Of lists/<id>.txt.
  int64_t size = 0;
  std::string title;
  std::string version;
  uint32_t rule_lines = 0;
  base::Time fetched_at;  // Content last changed.
  base::Time checked_at;  // Last 200/304.
  std::string last_error;
};

// User-facing switches.
struct ShieldSettings {
  ShieldSettings();
  ShieldSettings(const ShieldSettings&);
  ShieldSettings& operator=(const ShieldSettings&);
  ~ShieldSettings();

  bool enabled = true;          // Network blocking + cosmetics, all sites.
  bool cosmetic = true;         // Hide CSS + scriptlets.
  bool consent_killer = true;   // Auto-reject cookie banners.
  // Overrides of FilterListEntry::enabled_by_default, by list id.
  std::map<std::string, bool> list_enabled;
  base::Time last_update_check;
};

struct StoreState {
  StoreState();
  StoreState(const StoreState&);
  StoreState& operator=(const StoreState&);
  ~StoreState();

  ShieldSettings settings;
  std::map<std::string, ListState> lists;
};

// What engine.dat was built from.
struct EngineManifest {
  EngineManifest();
  EngineManifest(const EngineManifest&);
  EngineManifest& operator=(const EngineManifest&);
  ~EngineManifest();

  std::string format_key;  // molt_adblock engine_format_key().
  // list id -> sha256 of the list text compiled in. Empty map = built-in
  // fallback list only.
  std::map<std::string, std::string> list_hashes;
  std::string engine_sha256;  // Of engine.dat, to catch corruption.
};

// A cached list to compile into the engine.
struct ListSpec {
  ListSpec();
  ListSpec(const ListSpec&);
  ListSpec& operator=(const ListSpec&);
  ~ListSpec();

  std::string id;
  bool trusted = false;
  // sha256 recorded when the list was downloaded; the cached file must still
  // match (catches disk corruption and tampering with the cache). Empty if
  // the list was never downloaded.
  std::string expected_sha256;
};

base::DictValue SerializeState(const StoreState& state);
StoreState ParseState(const base::DictValue& dict);

base::DictValue SerializeManifest(const EngineManifest& manifest);
std::optional<EngineManifest> ParseManifest(const base::DictValue& dict);

// Paths.
base::FilePath ListPath(const base::FilePath& dir, std::string_view id);
base::FilePath StatePath(const base::FilePath& dir);
base::FilePath EnginePath(const base::FilePath& dir);
base::FilePath ManifestPath(const base::FilePath& dir);

// Blocking I/O. Writes are atomic (temp file + rename).
StoreState LoadState(const base::FilePath& dir);
bool SaveState(const base::FilePath& dir, const StoreState& state);
std::optional<EngineManifest> LoadManifest(const base::FilePath& dir);
bool SaveManifest(const base::FilePath& dir, const EngineManifest& manifest);
std::optional<std::string> ReadFileCapped(const base::FilePath& path,
                                          int64_t max_bytes);
bool WriteFileAtomic(const base::FilePath& path, std::string_view data);

std::string Sha256Hex(std::string_view data);

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_STORE_H_
