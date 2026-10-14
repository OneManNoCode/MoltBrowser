// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Generates and compiles the C++ side of the molt_adblock cxx bridge exactly
// as Chromium's rust_static_library(cxx_bindings=...) would, plus the bench's
// own small bridge (src/main.rs) used to drive the C++ API.

fn main() {
    const BRIDGE: &str = "../../src/chrome/browser/molt_ai/shield/adblock/molt_adblock.rs";
    cxx_build::bridges([BRIDGE, "src/main.rs"])
        .file("cxx/test_domain_resolver.cc")
        .file("cxx/cxx_smoke.cc")
        // Provides "third_party/rust/cxx/v1/cxx.h" like Chromium does.
        .include("cxx/include")
        // So the bridge's include!("chrome/browser/.../domain_resolver.h")
        // resolves to the real header.
        .include("../../src")
        .std("c++20")
        .flag_if_supported("-Wall")
        .flag_if_supported("-Werror")
        .compile("shield_bench_cxx");

    for f in [
        BRIDGE,
        "../../src/chrome/browser/molt_ai/shield/adblock/domain_resolver.h",
        "src/main.rs",
        "cxx/test_domain_resolver.cc",
        "cxx/cxx_smoke.cc",
        "cxx/cxx_smoke.h",
    ] {
        println!("cargo:rerun-if-changed={f}");
    }
}
