// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_engine.h"

#include <utility>

#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/threading/scoped_blocking_call.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/shield/adblock/molt_adblock.rs.h"
#include "chrome/browser/molt_ai/shield/filter_list_catalog.h"
#include "chrome/browser/molt_ai/shield/molt_shield_request_utils.h"

namespace molt_ai::shield {

namespace {

// Serialized EasyList-scale engines are ~7 MB; leave generous headroom.
constexpr int64_t kMaxEngineBytes = 96 * 1024 * 1024;

#if BUILDFLAG(IS_ANDROID)
constexpr bool kIsMobile = true;
#else
constexpr bool kIsMobile = false;
#endif

rust::Str ToStr(const std::string& s) {
  // Callers only pass ASCII (GURL specs, canonical hosts, type strings), so
  // rust::Str's UTF-8 validation can't fail.
  return rust::Str(s.data(), s.size());
}

}  // namespace

MoltShieldEngine::Compiled::Compiled(rust::Box<ffi::ShieldEngine> e)
    : engine(std::move(e)) {}
MoltShieldEngine::Compiled::~Compiled() = default;

MoltShieldEngine::Summary::Summary() = default;
MoltShieldEngine::Summary::Summary(const Summary&) = default;
MoltShieldEngine::Summary& MoltShieldEngine::Summary::operator=(
    const Summary&) = default;
MoltShieldEngine::Summary::~Summary() = default;

MoltShieldEngine::StartupResult::StartupResult() = default;
MoltShieldEngine::StartupResult::StartupResult(StartupResult&&) = default;
MoltShieldEngine::StartupResult& MoltShieldEngine::StartupResult::operator=(
    StartupResult&&) = default;
MoltShieldEngine::StartupResult::~StartupResult() = default;

MoltShieldEngine::MoltShieldEngine() {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

MoltShieldEngine::~MoltShieldEngine() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

// static
MoltShieldEngine::Summary MoltShieldEngine::MakeSummary(
    const Compiled& compiled,
    bool from_cache) {
  Summary summary;
  summary.from_cache = from_cache;
  summary.used_builtin_list = compiled.used_builtin_list;
  summary.rule_lines = compiled.rule_lines;
  return summary;
}

MoltShieldEngine::StartupResult MoltShieldEngine::Startup(
    const base::FilePath& dir) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  StartupResult result;
  result.state = LoadState(dir);
  result.summary =
      LoadAtStartup(dir, EnabledListSpecs(result.state), kIsMobile);
  return result;
}

MoltShieldEngine::Summary MoltShieldEngine::LoadAtStartup(
    const base::FilePath& dir,
    const std::vector<ListSpec>& lists,
    bool is_mobile) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);

  std::map<std::string, std::string> expected;
  for (const ListSpec& list : lists) {
    if (!list.expected_sha256.empty()) {
      expected[list.id] = list.expected_sha256;
    }
  }

  std::optional<EngineManifest> manifest = LoadManifest(dir);
  if (manifest &&
      manifest->format_key == std::string(ffi::engine_format_key()) &&
      manifest->list_hashes == expected) {
    std::optional<std::string> blob =
        ReadFileCapped(EnginePath(dir), kMaxEngineBytes);
    if (blob && Sha256Hex(*blob) == manifest->engine_sha256) {
      rust::Box<ffi::ShieldEngine> engine = ffi::new_empty_engine();
      if (engine->deserialize(rust::Slice<const uint8_t>(
              reinterpret_cast<const uint8_t*>(blob->data()), blob->size()))) {
        engine->use_resources(ffi::default_resources_json());
        engine_.emplace(std::move(engine));
        Summary summary;
        summary.from_cache = true;
        summary.used_builtin_list = expected.empty();
        return summary;
      }
    }
    LOG(WARNING) << "[MoltShield] engine cache unusable; recompiling lists";
  }

  std::unique_ptr<Compiled> compiled = BuildAndPersist(dir, lists, is_mobile);
  Summary summary = MakeSummary(*compiled, /*from_cache=*/false);
  engine_.emplace(std::move(compiled->engine));
  return summary;
}

MoltShieldEngine::Summary MoltShieldEngine::Swap(
    std::unique_ptr<Compiled> compiled) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Summary summary = MakeSummary(*compiled, /*from_cache=*/false);
  // The old engine is dropped here, after the new one is in place; nothing
  // else references it (all checks run on this sequence).
  engine_.emplace(std::move(compiled->engine));
  return summary;
}

CheckResult MoltShieldEngine::Check(const CheckInput& input) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CheckResult result;
  if (!engine_) {
    return result;
  }
  ffi::CheckResult r = (*engine_)->check_request(
      ToStr(input.url), ToStr(input.host), ToStr(input.source_host),
      ToStr(input.type), input.third_party, ToStr(input.method));
  result.should_block = r.should_block;
  result.important = r.important;
  result.has_exception = r.has_exception;
  result.redirect_data_url = std::string(r.redirect);
  result.rewritten_url = std::string(r.rewritten_url);
  return result;
}

CosmeticData MoltShieldEngine::Cosmetic(const GURL& document_url) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CosmeticData data;
  if (!engine_ || !document_url.SchemeIsHTTPOrHTTPS()) {
    return data;
  }
  ffi::CosmeticResources r =
      (*engine_)->url_cosmetic_resources(ToStr(document_url.spec()));
  data.hide_css = std::string(r.hide_css);
  data.scriptlet_js = std::string(r.injected_script);
  data.generichide = r.generichide;
  return data;
}

std::string MoltShieldEngine::HiddenClassIdCss(
    const GURL& document_url,
    const std::vector<std::string>& classes,
    const std::vector<std::string>& ids) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!engine_ || !document_url.SchemeIsHTTPOrHTTPS()) {
    return std::string();
  }
  // Tokens come from the renderer (untrusted); the Rust side bounds count
  // and length and skips non-UTF-8 entries.
  return std::string((*engine_)->hidden_class_id_css(
      ToStr(document_url.spec()), classes, ids));
}

// static
std::unique_ptr<MoltShieldEngine::Compiled> MoltShieldEngine::BuildAndPersist(
    const base::FilePath& dir,
    const std::vector<ListSpec>& lists,
    bool is_mobile) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  rust::Box<ffi::EngineBuilder> builder =
      ffi::new_engine_builder(is_mobile || kIsMobile);
  EngineManifest manifest;
  manifest.format_key = std::string(ffi::engine_format_key());
  std::map<std::string, uint32_t> rule_lines;

  for (const ListSpec& list : lists) {
    if (list.expected_sha256.empty()) {
      continue;  // Never downloaded.
    }
    std::optional<std::string> text =
        ReadFileCapped(ListPath(dir, list.id), kMaxListBytes);
    if (!text) {
      LOG(WARNING) << "[MoltShield] list missing from cache: " << list.id;
      continue;
    }
    if (Sha256Hex(*text) != list.expected_sha256 ||
        !base::IsStringUTF8(*text)) {
      LOG(WARNING) << "[MoltShield] cached list fails integrity check: "
                   << list.id;
      continue;
    }
    rule_lines[list.id] =
        builder->add_list(rust::Str(text->data(), text->size()), list.trusted);
    manifest.list_hashes[list.id] = list.expected_sha256;
  }

  bool used_builtin = false;
  if (manifest.list_hashes.empty()) {
    builder->add_list(ffi::builtin_filter_list(), /*trusted=*/false);
    used_builtin = true;
  }

  auto compiled =
      std::make_unique<Compiled>(ffi::build_engine(std::move(builder)));
  compiled->engine->use_resources(ffi::default_resources_json());

  rust::Vec<uint8_t> blob = compiled->engine->serialize();
  std::string_view blob_view(reinterpret_cast<const char*>(blob.data()),
                             blob.size());
  manifest.engine_sha256 = Sha256Hex(blob_view);
  // Engine first, manifest last: a crash in between leaves a manifest that
  // doesn't match engine.dat, which LoadAtStartup() treats as a cache miss.
  if (!WriteFileAtomic(EnginePath(dir), blob_view) ||
      !SaveManifest(dir, manifest)) {
    LOG(WARNING) << "[MoltShield] could not persist engine cache";
  }

  compiled->manifest = std::move(manifest);
  compiled->rule_lines = std::move(rule_lines);
  compiled->used_builtin_list = used_builtin;
  return compiled;
}

}  // namespace molt_ai::shield
