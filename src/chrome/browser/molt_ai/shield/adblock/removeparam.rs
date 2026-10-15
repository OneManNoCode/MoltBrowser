// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// `$removeparam` support around adblock-rust (tracking-parameter stripping).
//
// adblock-rust 0.13.3 implements `$removeparam=name` itself (the rewritten
// URL comes back in `BlockerResult::rewritten_url`), with two gaps this module
// fills:
//
//  * Exceptions. `@@...$removeparam` filters fail to parse in adblock-rust
//    ("removeparam with exception") and are dropped, so uBlock Origin's
//    exceptions for web archives and URL-defense wrappers (whose paths embed
//    whole URLs that must not be edited) would be ignored.
//    `scan_list()` collects them, rewritten as ordinary blocking filters for
//    a separate small engine: when one of those matches a request, its
//    rewrite is discarded. Exceptions are applied per request, not per
//    parameter (`@@...$removeparam=foo` keeps every parameter) — it can only
//    strip less than uBO, never more.
//
//  * `to=`. A generic `$removeparam=name,to=a.com|b.com` (uBO uses them for
//    Twitter/X and Amazon) never matches in adblock-rust 0.13.3. Each one is
//    added again as `||a.com^$removeparam=name`, one per plain domain
//    (entity `amazon.*` and negated `~a.com` entries are skipped: those rules
//    then strip less, never more). The originals stay inert.
//
//  * A built-in list. uBO's privacy list strips ~135 click identifiers
//    (fbclid, gclid, msclkid, mc_eid, ...) but deliberately keeps utm_*, and
//    a user may turn that list off. `BUILTIN_TRACKING_PARAMS` is compiled in
//    as `$removeparam=name,document,subdocument` — navigations only, so
//    first-party APIs that take campaign parameters in XHR keep working —
//    for every name the enabled lists don't already strip generically.

use std::collections::BTreeSet;

/// Tracking parameters stripped from navigations by default. Names must
/// match adblock-rust's `$removeparam` syntax (`[A-Za-z0-9_-]+`); matching
/// is exact and case-sensitive. Parameters some sites need to function
/// (e.g. Marketo's `mkt_tok`, used by unsubscribe links) are left out.
pub const BUILTIN_TRACKING_PARAMS: &[&str] = &[
    // Google Analytics / Urchin campaign parameters.
    "utm_source",
    "utm_medium",
    "utm_campaign",
    "utm_term",
    "utm_content",
    "utm_id",
    "utm_name",
    "utm_cid",
    "utm_reader",
    "utm_referrer",
    "utm_social",
    "utm_social-type",
    "utm_brand",
    "utm_place",
    "utm_pubreferrer",
    "utm_swu",
    "utm_viz_id",
    "utm_source_platform",
    "utm_creative_format",
    "utm_marketing_tactic",
    "_gl",
    // Ad click identifiers.
    "gclid",
    "gclsrc",
    "dclid",
    "gbraid",
    "wbraid",
    "gad_source",
    "srsltid",
    "fbclid",
    "msclkid",
    "twclid",
    "ttclid",
    "li_fat_id",
    "epik",
    "yclid",
    "ymclid",
    "ysclid",
    "rb_clickid",
    "irclickid",
    "wickedid",
    // Social share and email-marketing identifiers.
    "igshid",
    "igsh",
    "mc_eid",
    "_hsenc",
    "__hssc",
    "__hstc",
    "__hsfp",
    "hsCtaTracking",
    "oly_anon_id",
    "oly_enc_id",
    "vero_id",
    "vero_conv",
    "_openstat",
    "s_cid",
];

/// Removeparam-related content of one filter list.
#[derive(Debug, Default, PartialEq)]
pub struct ListScan {
    /// Parameters the list strips everywhere (`$removeparam=name` with no
    /// pattern, domain or type restriction).
    pub generic_params: BTreeSet<String>,
    /// `@@...$removeparam[=...]` filters, rewritten as blocking filters for
    /// the exception engine.
    pub exception_filters: Vec<String>,
    /// Hostname-anchored equivalents of generic `to=` removeparam filters,
    /// for the main engine.
    pub to_filters: Vec<String>,
}

/// adblock-rust's accepted `$removeparam` value syntax.
pub fn is_valid_param_name(name: &str) -> bool {
    !name.is_empty()
        && name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

fn is_removeparam_option(opt: &str) -> bool {
    opt == "removeparam" || opt.starts_with("removeparam=")
}

/// Network filter options that restrict content types (as in adblock-rust's
/// parser, `~` negations included).
fn is_type_option(opt: &str) -> bool {
    matches!(
        opt.trim_start_matches('~'),
        "document"
            | "doc"
            | "subdocument"
            | "frame"
            | "xmlhttprequest"
            | "xhr"
            | "script"
            | "image"
            | "stylesheet"
            | "css"
            | "object"
            | "object-subrequest"
            | "ping"
            | "beacon"
            | "websocket"
            | "font"
            | "media"
            | "other"
            | "all"
    )
}

/// Splits a network filter into pattern and options at its options `$`.
/// Returns None for lines that aren't network filters with options.
fn split_options(line: &str) -> Option<(&str, &str)> {
    // A regex pattern (`/.../$opts`) may itself contain `$`; the options
    // separator is the last `$`, as in adblock-rust.
    let dollar = line.rfind('$')?;
    Some((&line[..dollar], &line[dollar + 1..]))
}

/// Scans one (preprocessed) list for generic removeparam filters and
/// removeparam exceptions. Cheap: only lines mentioning `removeparam` are
/// looked at.
pub fn scan_list(text: &str) -> ListScan {
    let mut scan = ListScan::default();
    for raw in text.lines() {
        if !raw.contains("removeparam") {
            continue;
        }
        let line = raw.trim();
        if line.starts_with('!') || line.contains("##") || line.contains("#@#") {
            continue;
        }
        let Some((pattern, options)) = split_options(line) else { continue };
        let opts: Vec<&str> = options.split(',').map(str::trim).collect();
        if !opts.iter().any(|o| is_removeparam_option(o)) {
            continue;
        }

        if let Some(exception_pattern) = pattern.strip_prefix("@@") {
            let mut rest: Vec<&str> =
                opts.iter().copied().filter(|o| !is_removeparam_option(o)).collect();
            if !rest.iter().any(|o| is_type_option(o)) {
                // adblock-rust's default types for $removeparam.
                rest.extend(["document", "subdocument", "xmlhttprequest"]);
            }
            scan.exception_filters.push(format!("{exception_pattern}${}", rest.join(",")));
            continue;
        }

        if !(pattern.is_empty() || pattern == "*") {
            continue;
        }
        if opts.len() == 1 {
            if let Some(name) = opts[0].strip_prefix("removeparam=") {
                if is_valid_param_name(name) {
                    scan.generic_params.insert(name.to_string());
                }
            }
            continue;
        }
        if let Some(to) = opts.iter().find_map(|o| o.strip_prefix("to=")) {
            let rest: Vec<&str> =
                opts.iter().copied().filter(|o| !o.starts_with("to=")).collect();
            for domain in to.split('|') {
                if is_plain_domain(domain) {
                    scan.to_filters.push(format!("||{domain}^${}", rest.join(",")));
                }
            }
        }
    }
    scan
}

/// A hostname usable in a `||host^` pattern (no entity `.*`, negation, or
/// wildcard).
fn is_plain_domain(domain: &str) -> bool {
    !domain.is_empty()
        && !domain.starts_with('.')
        && !domain.ends_with('.')
        && domain.contains('.')
        && domain.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-')
}

/// The built-in filter list for every `BUILTIN_TRACKING_PARAMS` entry not in
/// `already_generic`, and how many filters it holds.
pub fn builtin_list(already_generic: &BTreeSet<String>) -> (String, u32) {
    let mut list = String::new();
    let mut n = 0u32;
    for name in BUILTIN_TRACKING_PARAMS {
        if already_generic.contains(*name) {
            continue;
        }
        list.push_str("$removeparam=");
        list.push_str(name);
        list.push_str(",document,subdocument\n");
        n += 1;
    }
    (list, n)
}

/// True if `rewritten` only drops query parameters from `original`: same
/// text up to the `?`, and a shorter URL. adblock-rust builds rewrites that
/// way; this guards the invariant callers rely on (same origin and path).
pub fn is_param_only_rewrite(original: &str, rewritten: &str) -> bool {
    let Some(q) = original.find('?') else { return false };
    rewritten.len() < original.len()
        && rewritten.as_bytes().get(..q) == original.as_bytes().get(..q)
        && matches!(rewritten.as_bytes().get(q), None | Some(b'?') | Some(b'#'))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn builtin_names_are_valid_and_unique() {
        let mut seen = BTreeSet::new();
        for name in BUILTIN_TRACKING_PARAMS {
            assert!(is_valid_param_name(name), "{name}");
            assert!(seen.insert(*name), "duplicate {name}");
        }
    }

    #[test]
    fn scans_generic_rules_and_exceptions() {
        let scan = scan_list(
            "! comment with removeparam\n\
             $removeparam=fbclid\n\
             *$removeparam=gclid\n\
             $removeparam=s,to=x.com|twitter.com\n\
             ||reddit.com^$doc,removeparam=ref\n\
             $doc,removeparam=s_cid\n\
             $removeparam=/^utm_/\n\
             @@||web.archive.org/*/http$removeparam\n\
             @@||archive.*/http$removeparam,doc,to=archive.is|archive.ph\n\
             @@||example.com^$removeparam=foo,domain=a.test\n\
             $removeparam=asc_source,to=aboutamazon.com|amazon.*|~a.com|amzn.to\n\
             example.com##.removeparam\n",
        );
        assert_eq!(
            scan.generic_params.iter().map(String::as_str).collect::<Vec<_>>(),
            ["fbclid", "gclid"]
        );
        assert_eq!(
            scan.exception_filters,
            [
                "||web.archive.org/*/http$document,subdocument,xmlhttprequest",
                "||archive.*/http$doc,to=archive.is|archive.ph",
                "||example.com^$domain=a.test,document,subdocument,xmlhttprequest",
            ]
        );
        assert_eq!(
            scan.to_filters,
            [
                "||x.com^$removeparam=s",
                "||twitter.com^$removeparam=s",
                "||aboutamazon.com^$removeparam=asc_source",
                "||amzn.to^$removeparam=asc_source",
            ]
        );
    }

    #[test]
    fn builtin_list_skips_params_lists_provide() {
        let mut have = BTreeSet::new();
        have.insert("fbclid".to_string());
        let (list, n) = builtin_list(&have);
        assert_eq!(n as usize, BUILTIN_TRACKING_PARAMS.len() - 1);
        assert!(!list.contains("=fbclid,"));
        assert!(list.contains("$removeparam=utm_source,document,subdocument\n"));
    }

    #[test]
    fn param_only_rewrites() {
        assert!(is_param_only_rewrite("https://a.test/p?x=1&fbclid=2", "https://a.test/p?x=1"));
        assert!(is_param_only_rewrite("https://a.test/p?fbclid=2", "https://a.test/p"));
        assert!(is_param_only_rewrite("https://a.test/p?fbclid=2#top", "https://a.test/p#top"));
        assert!(!is_param_only_rewrite("https://a.test/p", "https://a.test/"));
        assert!(!is_param_only_rewrite("https://a.test/p?x=1", "https://b.test/p?x"));
        assert!(!is_param_only_rewrite("https://a.test/p?x=1", "https://a.test/p?x=1"));
        assert!(!is_param_only_rewrite("https://a.test/p?x=1", "https://a.test/pz"));
    }
}
