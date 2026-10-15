// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// cxx bridge exposing MoltShield's adblock engine (Brave's adblock-rust,
// MPL-2.0) to C++. The engine logic lives in engine.rs; this file only
// adapts types.
//
// Threading: with adblock-rust's `single-thread` feature disabled (see
// //third_party/rust/chromium_crates_io/Cargo.toml), `ShieldEngine` is
// Send + Sync. C++ owns each engine on MoltShield's dedicated sequence
// (MoltShieldEngine); a rebuilt engine is moved there and swapped in.
//
// Panics: Chromium builds Rust with panic=abort, so nothing here may panic
// on untrusted input (filter lists, page-supplied class names, URLs). All
// parsing goes through fallible APIs.
//
// Domain resolution: the crate's embedded public-suffix list is disabled;
// adblock-rust calls back into Chromium's net::registry_controlled_domains
// through `registrable_domain_offset` (domain_resolver.cc), so MoltShield and
// the rest of the browser always agree on what a "site" is.

mod css;
mod engine;
mod preprocess;

use std::sync::Once;

pub use engine::{
    BUILTIN_FILTER_LIST, CheckOutcome, CosmeticOutcome, DEFAULT_RESOURCES_JSON, EngineBuilder,
    ShieldEngine,
};

#[cxx::bridge(namespace = "molt_ai::shield::ffi")]
mod ffi {
    /// Verdict for one network request.
    struct CheckResult {
        /// A blocking filter matched (may still be overridden by an
        /// exception unless `important`).
        matched: bool,
        important: bool,
        has_exception: bool,
        /// Final verdict: cancel the request (or serve `redirect`).
        should_block: bool,
        /// `data:` URL to serve instead (`$redirect=`), or empty.
        redirect: String,
        /// URL without `$removeparam` parameters, or empty if unchanged.
        rewritten_url: String,
    }

    /// Cosmetic resources for one document URL.
    struct CosmeticResources {
        /// User stylesheet: one `sel{display:none!important}` rule per
        /// vetted selector.
        hide_css: String,
        /// Scriptlets to run in the main world at document start (an IIFE);
        /// arguments are JSON-escaped by adblock-rust. Empty if none.
        injected_script: String,
        /// `$generichide`: skip the generic class/id pass for this page.
        generichide: bool,
        procedural_count: u32,
    }

    unsafe extern "C++" {
        include!("chrome/browser/molt_ai/shield/adblock/domain_resolver.h");

        /// Byte offset within `host` at which its registrable domain
        /// (eTLD+1) starts, per Chromium's registry_controlled_domains
        /// (private registries included). Returns 0 when `host` has no
        /// registrable domain (IP literal, bare suffix, single label).
        fn registrable_domain_offset(host: &str) -> usize;
    }

    extern "Rust" {
        type EngineBuilder;
        type ShieldEngine;

        /// Cache key for serialized engines ("adblock-rust-X/molt-N").
        fn engine_format_key() -> String;
        /// Bundled uBlock Origin scriptlet/redirect resources (JSON).
        fn default_resources_json() -> &'static str;
        /// Small fallback list used until real lists are downloaded.
        fn builtin_filter_list() -> &'static str;

        fn new_engine_builder(is_mobile: bool) -> Box<EngineBuilder>;
        /// Adds a list (after `!#if` preprocessing). Returns its filter-line
        /// count. `trusted` lists may use `trusted-*` scriptlets.
        fn add_list(self: &mut EngineBuilder, text: &str, trusted: bool) -> u32;
        fn rule_lines(self: &EngineBuilder) -> u32;
        /// Compiles the lists. Slow (hundreds of ms for EasyList-scale
        /// sets): call off the UI thread.
        fn build_engine(builder: Box<EngineBuilder>) -> Box<ShieldEngine>;

        fn new_empty_engine() -> Box<ShieldEngine>;
        /// Returns false (engine unchanged) on corrupt/mismatched data.
        fn deserialize(self: &mut ShieldEngine, data: &[u8]) -> bool;
        fn serialize(self: &ShieldEngine) -> Vec<u8>;
        /// Returns the number of resources accepted (0 = rejected).
        fn use_resources(self: &mut ShieldEngine, resources_json: &str) -> u32;

        /// `request_type` is an adblock-rust type string ("script", "image",
        /// "stylesheet", "xmlhttprequest", "sub_frame", "websocket", …).
        fn check_request(
            self: &ShieldEngine,
            url: &str,
            hostname: &str,
            source_hostname: &str,
            request_type: &str,
            third_party: bool,
            method: &str,
        ) -> CheckResult;

        fn url_cosmetic_resources(self: &ShieldEngine, url: &str) -> CosmeticResources;

        /// CSS for generic `##.class` / `###id` rules matching the given
        /// page tokens (untrusted; bounded internally). Empty under
        /// `$generichide`.
        #[cxx_name = "hidden_class_id_css"]
        fn ffi_hidden_class_id_css(
            self: &ShieldEngine,
            url: &str,
            classes: &CxxVector<CxxString>,
            ids: &CxxVector<CxxString>,
        ) -> String;
    }
}

/// adblock-rust domain resolver backed by Chromium's PSL.
struct ChromiumDomainResolver;

impl adblock::url_parser::ResolvesDomain for ChromiumDomainResolver {
    fn get_host_domain(&self, host: &str) -> (usize, usize) {
        let start = ffi::registrable_domain_offset(host);
        if start >= host.len() || !host.is_char_boundary(start) {
            (0, host.len())
        } else {
            (start, host.len())
        }
    }
}

static DOMAIN_RESOLVER_ONCE: Once = Once::new();

/// Installs the Chromium-backed domain resolver. Must run before any engine
/// parses a URL; every entry point that creates an engine calls it.
pub(crate) fn ensure_domain_resolver() {
    DOMAIN_RESOLVER_ONCE.call_once(|| {
        // Err only if a resolver was already set, which is fine.
        let _ = adblock::url_parser::set_domain_resolver(Box::new(ChromiumDomainResolver));
    });
}

fn engine_format_key() -> String {
    engine::engine_format_key()
}

fn default_resources_json() -> &'static str {
    engine::DEFAULT_RESOURCES_JSON
}

fn builtin_filter_list() -> &'static str {
    engine::BUILTIN_FILTER_LIST
}

fn new_engine_builder(is_mobile: bool) -> Box<EngineBuilder> {
    Box::new(EngineBuilder::new(is_mobile))
}

fn build_engine(builder: Box<EngineBuilder>) -> Box<ShieldEngine> {
    Box::new((*builder).build())
}

fn new_empty_engine() -> Box<ShieldEngine> {
    Box::new(ShieldEngine::empty())
}

impl ShieldEngine {
    fn use_resources(&mut self, resources_json: &str) -> u32 {
        u32::try_from(self.use_resources_json(resources_json)).unwrap_or(u32::MAX)
    }

    fn check_request(
        &self,
        url: &str,
        hostname: &str,
        source_hostname: &str,
        request_type: &str,
        third_party: bool,
        method: &str,
    ) -> ffi::CheckResult {
        let r = self.check(url, hostname, source_hostname, request_type, third_party, method);
        ffi::CheckResult {
            matched: r.matched,
            important: r.important,
            has_exception: r.exception,
            should_block: r.should_block,
            redirect: r.redirect.unwrap_or_default(),
            rewritten_url: r.rewritten_url.unwrap_or_default(),
        }
    }

    fn url_cosmetic_resources(&self, url: &str) -> ffi::CosmeticResources {
        let c = self.cosmetic(url);
        ffi::CosmeticResources {
            hide_css: c.hide_css,
            injected_script: c.injected_script,
            generichide: c.generichide,
            procedural_count: c.procedural_count,
        }
    }

    fn ffi_hidden_class_id_css(
        &self,
        url: &str,
        classes: &cxx::CxxVector<cxx::CxxString>,
        ids: &cxx::CxxVector<cxx::CxxString>,
    ) -> String {
        // Non-UTF-8 tokens can't match any rule; skip them.
        let classes = classes.iter().filter_map(|s| s.to_str().ok());
        let ids = ids.iter().filter_map(|s| s.to_str().ok());
        self.hidden_class_id_css(url, classes, ids)
    }
}
