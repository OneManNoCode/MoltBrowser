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
