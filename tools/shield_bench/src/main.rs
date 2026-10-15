// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShield engine benchmark on real filter lists + C++ (cxx) smoke test.
//
//   ./fetch_lists.sh && cargo run --release [-- LISTS_DIR]
//
// Measures: engine build time + memory, serialized size, deserialize time,
// per-request matching latency over adblock-rust's real-world request corpus
// (data/requests.json, ~240k requests from top sites), cosmetic lookups, and
// the generic class/id query. Then drives the bridge's generated C++ API.

use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use molt_adblock::{DEFAULT_RESOURCES_JSON, EngineBuilder, ShieldEngine};

#[cxx::bridge(namespace = "shield_bench")]
mod bench_ffi {
    unsafe extern "C++" {
        include!("shield_bench/cxx/cxx_smoke.h");
        fn bench_domain_offset(host: &str) -> usize;
        fn run_cxx_smoke(list_text: &str, resources_json: &str) -> i32;
    }
}

/// The default MoltShield list set (see shield/filter_list_catalog.cc),
/// with the "trusted" flag used for uBO's own lists.
pub const LISTS: &[(&str, bool)] = &[
    ("easylist.txt", false),
    ("easyprivacy.txt", false),
    ("ubo-filters.txt", true),
    ("ubo-privacy.txt", true),
    ("ubo-badware.txt", true),
    ("ubo-quick-fixes.txt", true),
];

fn vm_kb(field: &str) -> u64 {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| {
            s.lines()
                .find(|l| l.starts_with(field))
                .and_then(|l| l.split_whitespace().nth(1)?.parse().ok())
        })
        .unwrap_or(0)
}

fn registrable_domain(host: &str) -> &str {
    let off = bench_ffi::bench_domain_offset(host);
    if off < host.len() { &host[off..] } else { host }
}

struct Req {
    url: String,
    host: String,
    source_host: String,
    ty: String,
    third_party: bool,
}

fn load_requests(path: &Path) -> Vec<Req> {
    let text = match std::fs::read_to_string(path) {
        Ok(t) => t,
        Err(_) => return Vec::new(),
    };
    let mut out = Vec::new();
    for line in text.lines() {
        let Ok(v) = serde_json::from_str::<serde_json::Value>(line) else { continue };
        let (Some(url), Some(frame), Some(cpt)) =
            (v["url"].as_str(), v["frameUrl"].as_str(), v["cpt"].as_str())
        else {
            continue;
        };
        // Top-level documents are never blocked by MoltShield.
        if cpt == "document" {
            continue;
        }
        let Some(parsed) = adblock::url_parser::parse_url(url) else { continue };
        let source_host = adblock::url_parser::parse_url(frame)
            .map(|p| p.hostname().to_string())
            .unwrap_or_default();
        let host = parsed.hostname().to_string();
        let third_party =
            source_host.is_empty() || registrable_domain(&host) != registrable_domain(&source_host);
        out.push(Req { url: parsed.url.clone(), host, source_host, ty: cpt.to_string(), third_party });
    }
    out
}

fn pct(sorted: &[Duration], p: f64) -> Duration {
    if sorted.is_empty() {
        return Duration::ZERO;
    }
    let idx = ((sorted.len() as f64 - 1.0) * p).round() as usize;
    sorted[idx]
}

fn build(lists: &[(String, bool)]) -> (ShieldEngine, u32) {
    let mut b = EngineBuilder::new(false);
    for (text, trusted) in lists {
        b.add_list(text, *trusted);
    }
    let lines = b.rule_lines();
    let mut e = b.build();
    e.use_resources_json(DEFAULT_RESOURCES_JSON);
    (e, lines)
}

fn main() {
    let dir: PathBuf = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("lists"));

    let mut lists = Vec::new();
    let mut total_bytes = 0usize;
    for (name, trusted) in LISTS {
        let p = dir.join(name);
        match std::fs::read_to_string(&p) {
            Ok(t) => {
                println!("list {:<22} {:>9} bytes trusted={}", name, t.len(), trusted);
                total_bytes += t.len();
                lists.push((t, *trusted));
            }
            Err(_) => {
                eprintln!("missing {} — run tools/shield_bench/fetch_lists.sh", p.display());
                std::process::exit(2);
            }
        }
    }

    // ---- Build ----
    let rss0 = vm_kb("VmRSS:");
    let mut build_times = Vec::new();
    let mut engine = None;
    let mut rule_lines = 0;
    for _ in 0..3 {
        let t = Instant::now();
        let (e, n) = build(&lists);
        build_times.push(t.elapsed());
        rule_lines = n;
        engine = Some(e);
    }
    build_times.sort();
    let engine = engine.unwrap();
    let rss_engine = vm_kb("VmRSS:").saturating_sub(rss0);
    println!(
        "\nbuild: {} lists, {:.1} MB text, {} filter lines -> median {:.0} ms (min {:.0}, max {:.0}); \
         peak RSS {} MB, RSS after build +{} MB",
        lists.len(),
        total_bytes as f64 / 1e6,
        rule_lines,
        build_times[1].as_secs_f64() * 1e3,
        build_times[0].as_secs_f64() * 1e3,
        build_times[2].as_secs_f64() * 1e3,
        vm_kb("VmHWM:") / 1024,
        rss_engine / 1024
    );

    // ---- Serialize / deserialize ----
    let t = Instant::now();
    let blob = engine.serialize();
    let ser = t.elapsed();
    let mut de_times = Vec::new();
    let mut restored = ShieldEngine::empty();
    for _ in 0..5 {
        let mut e = ShieldEngine::empty();
        let t = Instant::now();
        assert!(e.deserialize(&blob), "deserialize failed");
        de_times.push(t.elapsed());
        restored = e;
    }
    de_times.sort();
    restored.use_resources_json(DEFAULT_RESOURCES_JSON);
    println!(
        "serialize: {:.2} MB in {:.0} ms; deserialize median {:.1} ms",
        blob.len() as f64 / 1e6,
        ser.as_secs_f64() * 1e3,
        de_times[2].as_secs_f64() * 1e3
    );

    // ---- Network matching ----
    let reqs = load_requests(&dir.join("requests.json"));
    if reqs.is_empty() {
        println!("(no requests.json corpus — skipping matching benchmark)");
    } else {
        // Warm up regex caches like a running browser would be.
        for r in reqs.iter().take(2000) {
            restored.check(&r.url, &r.host, &r.source_host, &r.ty, r.third_party, "GET");
        }
        let mut times = Vec::with_capacity(reqs.len());
        let (mut blocked, mut excepted, mut redirected, mut rewritten) = (0usize, 0, 0, 0);
        let all = Instant::now();
        for r in &reqs {
            let t = Instant::now();
            let o = restored.check(&r.url, &r.host, &r.source_host, &r.ty, r.third_party, "GET");
            times.push(t.elapsed());
            blocked += o.should_block as usize;
            excepted += (o.matched && o.exception && !o.should_block) as usize;
            redirected += (o.should_block && o.redirect.is_some()) as usize;
            rewritten += (!o.should_block && o.rewritten_url.is_some()) as usize;
        }
        let total = all.elapsed();
        times.sort();
        println!(
            "match: {} requests in {:.0} ms -> mean {:.2} us, p50 {:.2} us, p99 {:.2} us, \
             p99.9 {:.1} us, max {:.1} us",
            reqs.len(),
            total.as_secs_f64() * 1e3,
            total.as_secs_f64() * 1e6 / reqs.len() as f64,
            pct(&times, 0.5).as_secs_f64() * 1e6,
            pct(&times, 0.99).as_secs_f64() * 1e6,
            pct(&times, 0.999).as_secs_f64() * 1e6,
            times.last().unwrap().as_secs_f64() * 1e6,
        );
        println!(
            "       blocked {} ({:.1}%), exception-saved {}, served redirect resource {}, \
             removeparam rewrites {}",
            blocked,
            100.0 * blocked as f64 / reqs.len() as f64,
            excepted,
            redirected,
            rewritten
        );
    }

    // ---- Cosmetic ----
    let sites = [
        "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
        "https://www.cnn.com/",
        "https://www.nytimes.com/",
        "https://www.reddit.com/",
        "https://www.amazon.com/",
        "https://www.bbc.co.uk/news",
        "https://www.theguardian.com/international",
        "https://stackoverflow.com/questions",
        "https://en.wikipedia.org/wiki/Main_Page",
        "https://www.forbes.com/",
        "https://www.dailymail.co.uk/home/index.html",
        "https://www.twitch.tv/",
    ];
    println!();
    for site in sites {
        let t = Instant::now();
        let c = restored.cosmetic(site);
        let dt = t.elapsed();
        println!(
            "cosmetic {:<46} {:>6.2} ms  css {:>7} B  scriptlets {:>6} B  generichide={} procedural={}",
            site,
            dt.as_secs_f64() * 1e3,
            c.hide_css.len(),
            c.injected_script.len(),
            c.generichide,
            c.procedural_count
        );
    }
    let mut classes: Vec<String> =
        (0..400).map(|i| format!("c{i}")).collect();
    classes.extend(
        ["ad", "ads", "adsbygoogle", "ad-banner", "sponsored", "advert", "header", "nav", "footer"]
            .iter()
            .map(|s| s.to_string()),
    );
    let ids: Vec<String> = ["main", "content", "ad_unit", "google_ads_iframe", "sidebar-ad"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    let t = Instant::now();
    let css = restored.hidden_class_id_css(
        "https://news.example/",
        classes.iter().map(String::as_str),
        ids.iter().map(String::as_str),
    );
    println!(
        "hidden_class_id_css: {} classes + {} ids -> {} rules in {:.2} ms",
        classes.len(),
        ids.len(),
        css.lines().count(),
        t.elapsed().as_secs_f64() * 1e3
    );

    // ---- C++ API smoke test through the generated cxx bindings ----
    println!("\nC++ (cxx) smoke test:");
    let failures = bench_ffi::run_cxx_smoke(&lists[0].0, DEFAULT_RESOURCES_JSON);
    if failures != 0 {
        eprintln!("C++ smoke test: {failures} failure(s)");
        std::process::exit(1);
    }
    println!("C++ smoke test: all checks passed");
}
