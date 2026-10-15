// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShieldEngine owns the live adblock-rust engine. It lives on
// MoltShield's dedicated engine sequence (base::SequenceBound in
// MoltShieldService) — never the UI or IO thread — and every network check
// and cosmetic lookup runs there.
//
// Rebuilds (after a list update) happen in BuildAndPersist() on a separate
// thread-pool task, so matching never waits on a multi-hundred-ms compile;
// the finished engine is moved to the engine sequence and swapped in with
// Swap(), a pointer move between two checks. Requests are therefore always
// checked against exactly one complete engine.
//
// Startup is the exception, by design: LoadAtStartup() runs ON the engine
// sequence, so the first requests queue behind it instead of slipping
// through unfiltered. From cache it is a ~30 ms deserialize (EasyList +
// EasyPrivacy + uBO, measured in tools/shield_bench).

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_ENGINE_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_ENGINE_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/sequence_checker.h"
#include "chrome/browser/molt_ai/shield/molt_shield_store.h"
#include "chrome/browser/molt_ai/shield/molt_shield_types.h"
#include "third_party/rust/cxx/v1/cxx.h"
#include "url/gurl.h"

namespace molt_ai::shield {

namespace ffi {
struct ShieldEngine;
}  // namespace ffi

class MoltShieldEngine {
 public:
  // A compiled engine plus what it was built from. Move-only; safe to move
  // across sequences (the Rust engine is Send + Sync).
  struct Compiled {
    explicit Compiled(rust::Box<ffi::ShieldEngine> engine);
    ~Compiled();
    Compiled(const Compiled&) = delete;
    Compiled& operator=(const Compiled&) = delete;

    rust::Box<ffi::ShieldEngine> engine;
    EngineManifest manifest;
    std::map<std::string, uint32_t> rule_lines;  // Per list id.
    bool used_builtin_list = false;
  };

  // What LoadAtStartup()/Swap() report back to the UI thread.
  struct Summary {
    Summary();
    Summary(const Summary&);
    Summary& operator=(const Summary&);
    ~Summary();

    bool from_cache = false;
    bool used_builtin_list = false;
    std::map<std::string, uint32_t> rule_lines;
  };

  struct StartupResult {
    StartupResult();
    StartupResult(StartupResult&&);
    StartupResult& operator=(StartupResult&&);
    ~StartupResult();

    StoreState state;
    Summary summary;
  };

  MoltShieldEngine();
  ~MoltShieldEngine();
  MoltShieldEngine(const MoltShieldEngine&) = delete;
  MoltShieldEngine& operator=(const MoltShieldEngine&) = delete;

  // Engine sequence; the first task posted there. Loads state.json, then
  // LoadAtStartup() with the enabled lists.
  StartupResult Startup(const base::FilePath& dir);

  // Engine sequence. Deserializes engine.dat when it was built from exactly
  // `lists` (same hashes, same adblock-rust format); otherwise compiles the
  // cached lists (or the built-in fallback list) and rewrites the cache.
  Summary LoadAtStartup(const base::FilePath& dir,
                        const std::vector<ListSpec>& lists,
                        bool is_mobile);

  // Engine sequence. Atomically replaces the live engine.
  Summary Swap(std::unique_ptr<Compiled> compiled);

  // Engine sequence. Before an engine is loaded, everything is allowed.
  CheckResult Check(const CheckInput& input) const;
  CosmeticData Cosmetic(const GURL& document_url) const;
  std::string HiddenClassIdCss(const GURL& document_url,
                               const std::vector<std::string>& classes,
                               const std::vector<std::string>& ids) const;

  // Any MayBlock thread-pool sequence. Reads the cached `lists`, compiles
  // them (falls back to the built-in list if none is usable), writes
  // engine.dat + engine.json, and returns the engine for Swap().
  static std::unique_ptr<Compiled> BuildAndPersist(
      const base::FilePath& dir,
      const std::vector<ListSpec>& lists,
      bool is_mobile);

 private:
  static Summary MakeSummary(const Compiled& compiled, bool from_cache);

  std::optional<rust::Box<ffi::ShieldEngine>> engine_;
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_ENGINE_H_
