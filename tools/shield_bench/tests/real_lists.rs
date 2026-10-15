// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Behavioral tests of the molt_adblock bridge against the REAL default list
// set (run ./fetch_lists.sh first; tests are skipped with a notice if the
// lists are missing so `cargo test` still works offline).

use std::path::Path;
use std::sync::OnceLock;

use molt_adblock::{DEFAULT_RESOURCES_JSON, EngineBuilder, ShieldEngine};

const LISTS: &[(&str, bool)] = &[
    ("easylist.txt", false),
    ("easyprivacy.txt", false),
    ("ubo-filters.txt", true),
    ("ubo-privacy.txt", true),
    ("ubo-badware.txt", true),
    ("ubo-quick-fixes.txt", true),
];

fn engine() -> Option<&'static ShieldEngine> {
    static ENGINE: OnceLock<Option<ShieldEngine>> = OnceLock::new();
    ENGINE
        .get_or_init(|| {
            let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("lists");
            let mut b = EngineBuilder::new(false);
            for (name, trusted) in LISTS {
                let text = std::fs::read_to_string(dir.join(name)).ok()?;
                b.add_list(&text, *trusted);
            }
            let mut e = b.build();
            assert!(e.use_resources_json(DEFAULT_RESOURCES_JSON) > 100);
            Some(e)
        })
        .as_ref()
}

macro_rules! engine_or_skip {
    () => {
        match engine() {
            Some(e) => e,
            None => {
                eprintln!("skipping: run tools/shield_bench/fetch_lists.sh first");
                return;
            }
        }
    };
}

fn host_of(url: &str) -> &str {
    url.split('/').nth(2).unwrap_or("")
}

fn check(e: &ShieldEngine, url: &str, page_host: &str, ty: &str) -> molt_adblock::CheckOutcome {
    let host = host_of(url);
    let tp = !(host == page_host || host.ends_with(&format!(".{page_host}"))
        || page_host.ends_with(&format!(".{host}")));
    e.check(url, host, page_host, ty, tp, "GET")
}

#[test]
fn blocks_well_known_ads_and_trackers() {
    let e = engine_or_skip!();
    for (url, ty) in [
        ("https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js", "script"),
        ("https://securepubads.g.doubleclick.net/tag/js/gpt.js", "script"),
        ("https://connect.facebook.net/en_US/fbevents.js", "script"),
        ("https://static.hotjar.com/c/hotjar-123.js?sv=6", "script"),
        ("https://sb.scorecardresearch.com/beacon.js", "script"),
        ("https://bat.bing.com/bat.js", "script"),
        ("https://ib.adnxs.com/ut/v3/prebid", "xmlhttprequest"),
        ("https://www.facebook.com/tr/?id=1&ev=PageView", "image"),
    ] {
        let r = check(e, url, "www.cnn.com", ty);
        assert!(r.should_block, "expected block: {url} -> {r:?}");
    }
}

#[test]
fn serves_redirect_resources() {
    let e = engine_or_skip!();
    // uBO neuters Google Analytics with a stub instead of breaking pages that
    // call ga().
    let r = check(e, "https://www.google-analytics.com/analytics.js", "www.example.org", "script");
    assert!(r.should_block, "{r:?}");
    let d = r.redirect.expect("GA should be served a surrogate");
    assert!(d.starts_with("data:application/javascript;base64,"), "{d}");
}

#[test]
fn leaves_ordinary_resources_alone() {
    let e = engine_or_skip!();
    for (url, page, ty) in [
        ("https://code.jquery.com/jquery-3.7.1.min.js", "www.example.org", "script"),
        ("https://fonts.googleapis.com/css2?family=Roboto", "www.example.org", "stylesheet"),
        ("https://ajax.googleapis.com/ajax/libs/jquery/3.7.1/jquery.min.js", "www.example.org", "script"),
        ("https://upload.wikimedia.org/wikipedia/commons/a/a9/Example.jpg", "en.wikipedia.org", "image"),
        ("https://www.cnn.com/media/sites/cnn/assets/app.js", "www.cnn.com", "script"),
        ("https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg", "www.youtube.com", "image"),
        ("https://api.github.com/repos/brave/adblock-rust", "github.com", "xmlhttprequest"),
    ] {
        let r = check(e, url, page, ty);
        assert!(!r.should_block, "unexpected block: {url} on {page} -> {r:?}");
    }
}

#[test]
fn youtube_gets_scriptlets_and_hiding() {
    let e = engine_or_skip!();
    let c = e.cosmetic("https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    assert!(!c.injected_script.is_empty(), "uBO ships YouTube ad scriptlets");
    assert!(c.injected_script.starts_with("(function() {"));
    assert!(c.injected_script.contains("const scriptletGlobals = {};"));
    assert!(!c.hide_css.is_empty());
    for line in c.hide_css.lines() {
        assert!(line.ends_with("{display:none!important}"), "{line}");
        assert_eq!(line.matches('{').count(), 1, "{line}");
    }
}

#[test]
fn generic_cosmetics_on_news_site() {
    let e = engine_or_skip!();
    let c = e.cosmetic("https://www.bbc.co.uk/news");
    assert!(!c.generichide);
    let css = e.hidden_class_id_css(
        "https://www.bbc.co.uk/news",
        ["adsbygoogle-box", "sponsored-post", "article-body"],
        ["ad_unit", "main"],
    );
    // EasyList: ##.adsbygoogle-box, ##.sponsored-post, ###ad_unit
    assert!(css.contains(".adsbygoogle-box{display:none!important}"), "{css}");
    assert!(css.contains(".sponsored-post{display:none!important}"), "{css}");
    assert!(css.contains("#ad_unit{display:none!important}"), "{css}");
    assert!(!css.contains("article-body") && !css.contains("#main{"));
}

#[test]
fn serialized_engine_matches_original() {
    let e = engine_or_skip!();
    let mut restored = ShieldEngine::empty();
    assert!(restored.deserialize(&e.serialize()));
    restored.use_resources_json(DEFAULT_RESOURCES_JSON);
    let corpus = Path::new(env!("CARGO_MANIFEST_DIR")).join("lists/requests.json");
    let Ok(text) = std::fs::read_to_string(corpus) else { return };
    let mut compared = 0;
    for line in text.lines().take(20_000) {
        let v: serde_json::Value = match serde_json::from_str(line) {
            Ok(v) => v,
            Err(_) => continue,
        };
        let (Some(url), Some(frame), Some(cpt)) =
            (v["url"].as_str(), v["frameUrl"].as_str(), v["cpt"].as_str())
        else {
            continue;
        };
        let page = host_of(frame).to_string();
        let a = check(e, url, &page, cpt);
        let b = check(&restored, url, &page, cpt);
        assert_eq!(a, b, "{url}");
        compared += 1;
    }
    assert!(compared > 10_000);
}

/// A navigation, checked in its own context the way MoltShield's navigation
/// interceptor does (source = target host, first-party).
fn navigate(e: &ShieldEngine, url: &str) -> Option<String> {
    let host = host_of(url);
    e.check(url, host, host, "document", false, "GET").rewritten_url
}

#[test]
fn removeparam_strips_click_ids_and_campaign_params_from_navigations() {
    let e = engine_or_skip!();
    for (url, want) in [
        // uBO privacy (generic) + MoltShield built-in utm_* together.
        (
            "https://www.example.com/article?id=5&utm_source=news&utm_medium=email&fbclid=IwAR0",
            "https://www.example.com/article?id=5",
        ),
        ("https://shop.example/p?gclid=Cj0KCQ&color=red", "https://shop.example/p?color=red"),
        ("https://news.example/?mc_eid=12ab34&msclkid=99", "https://news.example/"),
        // `$removeparam=s,to=twitter.com|x.com` etc.: inert in adblock-rust,
        // applied through MoltShield's `||x.com^` conversion.
        ("https://x.com/someone/status/1?s=20&ref_src=twsrc", "https://x.com/someone/status/1"),
        // `...,to=aboutamazon.com|amazon.*|amzn.to`: plain domains only (the
        // `amazon.*` entity isn't converted, so amazon.de keeps it).
        ("https://amzn.to/3xYz?asc_source=a&tag=b", "https://amzn.to/3xYz?tag=b"),
        // `||youtu.be^$removeparam=si`
        ("https://youtu.be/dQw4w9WgXcQ?si=Ab12Cd34", "https://youtu.be/dQw4w9WgXcQ"),
    ] {
        assert_eq!(navigate(e, url).as_deref(), Some(want), "{url}");
    }
}

#[test]
fn removeparam_leaves_functional_parameters_alone() {
    let e = engine_or_skip!();
    for url in [
        // `s` is only stripped on twitter.com / x.com (`to=`), not from
        // every WordPress search.
        "https://blog.example.org/?s=privacy+browser",
        "https://www.google.com/search?q=adblock&hl=en",
        "https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=42",
        "https://en.wikipedia.org/w/index.php?title=Special:Search&search=tracking",
        // Empty values are kept, names are case-sensitive.
        "https://a.example/?fbclid=&UTM_SOURCE=x",
        "https://www.amazon.de/dp/B0?asc_source=a",
    ] {
        assert_eq!(navigate(e, url), None, "{url}");
    }
}

#[test]
fn removeparam_honors_ubo_exceptions() {
    let e = engine_or_skip!();
    // `@@||web.archive.org/*/http$removeparam`: the archived URL is part of
    // the path; editing it would break the archive lookup.
    assert_eq!(
        navigate(e, "https://web.archive.org/web/2024/https://news.example/?fbclid=1&utm_source=x"),
        None
    );
    // `@@||urldefense.com^$removeparam`
    assert_eq!(navigate(e, "https://urldefense.com/v3/__https://a.example/?utm_source=x__;"), None);
}

#[test]
fn removeparam_on_subresources_uses_list_rules_only() {
    let e = engine_or_skip!();
    // uBO privacy's generic `$removeparam=fbclid` covers fetch/XHR too.
    let url = "https://www.example.com/api/item?fbclid=IwAR0&id=3";
    let r = check(e, url, "www.example.com", "xmlhttprequest");
    assert!(!r.should_block, "{r:?}");
    assert_eq!(r.rewritten_url.as_deref(), Some("https://www.example.com/api/item?id=3"), "{r:?}");
    // EasyPrivacy blocks cxense outright (uBO also strips its widget
    // parameters; the proxy ignores rewrites of blocked requests).
    let r = check(
        e,
        "https://api.cxense.com/public/widget/data?widgetId=w1&sid=s2&json=1",
        "www.example.com",
        "xmlhttprequest",
    );
    assert!(r.should_block, "{r:?}");
    // The built-in utm_* list is navigation-only.
    let r = check(
        e,
        "https://www.example.com/api/signup?utm_source=ad",
        "www.example.com",
        "xmlhttprequest",
    );
    assert_eq!(r.rewritten_url, None);
    // Images aren't a removeparam type by default.
    let r = check(e, "https://cdn.example.net/x.png?fbclid=1", "www.example.com", "image");
    assert_eq!(r.rewritten_url, None);
}

#[test]
fn builtin_params_do_not_duplicate_list_rules() {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("lists");
    let Ok(privacy) = std::fs::read_to_string(dir.join("ubo-privacy.txt")) else {
        eprintln!("skipping: run tools/shield_bench/fetch_lists.sh first");
        return;
    };
    let mut b = EngineBuilder::new(false);
    let all = b.builtin_params();
    b.add_list(&privacy, true);
    let remaining = b.builtin_params();
    // fbclid, gclid, msclkid, mc_eid, ... come from the list; utm_* don't.
    assert!(remaining < all, "{remaining} vs {all}");
    assert!(remaining >= 20, "utm_* should remain built in: {remaining}");
}

/// Hosts named by procedural/action cosmetic rules in the default lists
/// (first plain hostname of each rule).
fn procedural_hosts() -> Vec<String> {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("lists");
    let mut hosts = std::collections::BTreeSet::new();
    for (name, _) in LISTS {
        let Ok(text) = std::fs::read_to_string(dir.join(name)) else { continue };
        for line in text.lines() {
            let Some(sep) = line.find("##").or_else(|| line.find("#?#")) else { continue };
            let body = &line[sep..];
            let procedural = [
                ":has-text(", ":upward(", ":matches-css", ":xpath(", ":min-text-length(",
                ":matches-attr(", ":matches-path(", ":remove()", ":style(", ":remove-attr(",
                ":remove-class(", ":-abp-contains(",
            ]
            .iter()
            .any(|op| body.contains(op));
            if !procedural || sep == 0 || line.starts_with('!') || body.starts_with("##+js") {
                continue;
            }
            if let Some(host) = line[..sep]
                .split(',')
                .find(|h| !h.starts_with('~') && !h.ends_with(".*") && h.contains('.'))
            {
                hosts.insert(host.to_string());
            }
        }
    }
    hosts.into_iter().collect()
}

#[test]
fn procedural_filters_from_real_lists() {
    let e = engine_or_skip!();
    let hosts = procedural_hosts();
    assert!(hosts.len() > 100, "{}", hosts.len());
    let (mut count, mut dropped, mut pages_with_js, mut style_rules) = (0u64, 0u64, 0u64, 0u64);
    // Every page's procedural input, for the JS engine's corpus test
    // (tools/shield_bench/procedural_filters_test.js).
    let mut corpus = Vec::new();
    for host in &hosts {
        let c = e.cosmetic(&format!("https://{host}/"));
        count += u64::from(c.procedural_count);
        dropped += u64::from(c.procedural_dropped);
        style_rules += c.hide_css.lines().filter(|l| !l.ends_with("{display:none!important}")).count()
            as u64;
        if c.procedural_count == 0 {
            assert!(c.procedural_json.is_empty());
            continue;
        }
        pages_with_js += 1;
        let v: serde_json::Value = serde_json::from_str(&c.procedural_json).expect("valid JSON");
        corpus.push(serde_json::json!({
            "host": host,
            "filters": v.clone(),
            "styles": c.procedural_styles.clone(),
        }));
        let filters = v.as_array().expect("array");
        assert_eq!(filters.len(), c.procedural_count as usize);
        for f in filters {
            assert!(!f["selector"].as_array().expect("ops").is_empty(), "{f}");
            if let Some(i) = f["action"]["arg"].as_u64().filter(|_| f["action"]["type"] == "style") {
                assert!((i as usize) < c.procedural_styles.len(), "{f}");
            }
        }
        // The hide stylesheet no longer carries procedural selectors.
        for line in c.hide_css.lines() {
            assert!(!line.contains(":has-text(") && !line.contains(":upward("), "{line}");
            assert_eq!(line.matches('{').count(), 1, "{line}");
        }
        for s in &c.procedural_styles {
            assert!(s.ends_with("!important") && !s.contains(['{', '}', '\\']), "{s}");
        }
    }
    let out = Path::new(env!("CARGO_MANIFEST_DIR")).join("target/procedural_corpus.json");
    std::fs::write(&out, serde_json::Value::Array(corpus).to_string()).expect("write corpus");
    eprintln!(
        "procedural: {} hosts, {pages_with_js} with procedural filters, {count} filters applied, \
         {dropped} dropped ({:.1}%), {style_rules} :style() rules as CSS",
        hosts.len(),
        100.0 * dropped as f64 / (count + dropped).max(1) as f64
    );
    assert!(count > 300, "{count}");
    // Most real-world procedural filters are supported and pass vetting.
    assert!(dropped * 10 < count, "dropped {dropped} of {}", count + dropped);
}

#[test]
fn procedural_real_rule_shapes() {
    let e = engine_or_skip!();
    // uBO filters: `bestbuy.com#?#div[data-testid="Row_testID"]:has(span:has-text(Sponsored))`
    let c = e.cosmetic("https://www.bestbuy.com/");
    assert!(
        c.procedural_json.contains(
            r#"{"selector":[{"arg":"div[data-testid=\"Row_testID\"]","type":"css-selector"},{"arg":[{"arg":" span","type":"css-selector"},{"arg":{"text":"Sponsored"},"type":"has-text"}],"type":"has"}]}"#
        ),
        "{}",
        c.procedural_json
    );
    assert!(!c.hide_css.contains(":has-text("));
}
