// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Exercises the molt_adblock bridge through its generated C++ API exactly
// the way the browser glue (shield/molt_shield_engine.cc) calls it.

#include "shield_bench/cxx/cxx_smoke.h"

#include <cstdio>
#include <string>
#include <vector>

// cxx-build's location for the generated header of the real bridge; in
// Chromium it is "chrome/browser/molt_ai/shield/adblock/molt_adblock.rs.h".
#include "shield_bench/src/chrome/browser/molt_ai/shield/adblock/molt_adblock.rs.h"

namespace shield_bench {

namespace ffi = molt_ai::shield::ffi;

namespace {

int g_failures = 0;

void Expect(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) {
    ++g_failures;
  }
}

bool Contains(const rust::String& haystack, const char* needle) {
  return std::string(haystack).find(needle) != std::string::npos;
}

}  // namespace

int32_t run_cxx_smoke(rust::Str list_text, rust::Str resources_json) {
  g_failures = 0;

  Expect(!std::string(ffi::engine_format_key()).empty(), "engine_format_key");
  Expect(ffi::builtin_filter_list().size() > 100, "builtin_filter_list");

  // Build: builder -> engine, as MoltShieldEngine::BuildFromLists does.
  rust::Box<ffi::EngineBuilder> builder = ffi::new_engine_builder(false);
  uint32_t lines = builder->add_list(list_text, /*trusted=*/false);
  lines += builder->add_list(
      "example.com##+js(set-constant, adsEnabled, false)\n"
      "example.com##.promo\n##.generic-ad\n"
      "example.com##.card:has-text(Sponsored)\n"
      "example.com##.tile:has-text(Ad):style(opacity: 0)\n",
      /*trusted=*/true);
  Expect(lines > 1000 && builder->rule_lines() == lines, "add_list counts");
  rust::Box<ffi::ShieldEngine> engine = ffi::build_engine(std::move(builder));
  Expect(engine->use_resources(resources_json) > 100, "use_resources");

  ffi::CheckResult r = engine->check_request(
      "https://securepubads.g.doubleclick.net/tag/js/gpt.js",
      "securepubads.g.doubleclick.net", "www.cnn.com", "script",
      /*third_party=*/true, "GET");
  Expect(r.matched && r.should_block, "doubleclick gpt.js blocked");

  r = engine->check_request("https://www.cnn.com/app.js", "www.cnn.com",
                            "www.cnn.com", "script", false, "GET");
  Expect(!r.should_block, "first-party script allowed");

  // Serialize -> fresh engine -> deserialize (startup fast path).
  rust::Vec<uint8_t> blob = engine->serialize();
  rust::Box<ffi::ShieldEngine> restored = ffi::new_empty_engine();
  Expect(restored->deserialize(
             rust::Slice<const uint8_t>(blob.data(), blob.size())),
         "deserialize");
  restored->use_resources(resources_json);
  const uint8_t junk[] = {1, 2, 3};
  Expect(!restored->deserialize(rust::Slice<const uint8_t>(junk, 3)),
         "deserialize rejects junk");
  r = restored->check_request(
      "https://securepubads.g.doubleclick.net/tag/js/gpt.js",
      "securepubads.g.doubleclick.net", "www.cnn.com", "script", true, "GET");
  Expect(r.should_block, "restored engine still blocks");

  ffi::CosmeticResources c =
      restored->url_cosmetic_resources("https://www.example.com/");
  Expect(Contains(c.hide_css, ".promo{display:none!important}"),
         "hostname hide css");
  Expect(Contains(c.injected_script, "setConstant("), "scriptlet injected");
  Expect(Contains(c.procedural_json, "\"has-text\"") &&
             !Contains(c.hide_css, ":has-text(") && c.procedural_count == 2,
         "procedural filters split from the stylesheet");
  Expect(c.procedural_styles.size() == 1 &&
             std::string(c.procedural_styles[0]) == "opacity:0!important",
         "procedural styles via rust::Vec<rust::String>");
  // As MoltShieldEngine::Cosmetic copies them out.
  std::vector<std::string> styles;
  styles.reserve(c.procedural_styles.size());
  for (const rust::String& style : c.procedural_styles) {
    styles.emplace_back(style);
  }
  Expect(styles == std::vector<std::string>{"opacity:0!important"},
         "procedural styles copied to std::vector<std::string>");

  std::vector<std::string> classes = {"generic-ad", "header", "x"};
  std::vector<std::string> ids = {"main"};
  rust::String css =
      restored->hidden_class_id_css("https://other.test/", classes, ids);
  Expect(Contains(css, ".generic-ad{display:none!important}"),
         "hidden_class_id_css via CxxVector<CxxString>");

  // Navigation removeparam (built-in utm_* list), as
  // MoltShieldNavigationInterceptor asks for it: own host as source,
  // first-party, type "document".
  r = restored->check_request(
      "https://www.example.com/a?utm_source=x&id=1", "www.example.com",
      "www.example.com", "document", /*third_party=*/false, "GET");
  Expect(!r.should_block &&
             std::string(r.rewritten_url) == "https://www.example.com/a?id=1",
         "navigation removeparam rewrite");
  r = restored->check_request("https://www.example.com/a?id=1",
                              "www.example.com", "www.example.com",
                              "document", false, "GET");
  Expect(r.rewritten_url.empty(), "no rewrite without tracking params");

  return g_failures;
}

}  // namespace shield_bench
