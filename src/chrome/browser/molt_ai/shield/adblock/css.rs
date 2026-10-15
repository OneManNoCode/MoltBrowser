// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Turns cosmetic hide selectors into a stylesheet that is safe to insert
// into a page as a user stylesheet.
//
// Selectors come from filter lists, which are only semi-trusted (a list
// update could be compromised or simply buggy). A selector containing `{`
// or `}` could otherwise break out of its rule and inject arbitrary CSS —
// e.g. `body{background:url(https://tracker/…)}` to beacon every page load.
// So anything that could escape the rule or fetch a resource is dropped,
// and each selector gets its own rule so one invalid selector can't void
// the rest (a CSS parser drops a whole rule if any selector in its list is
// invalid).

/// Max selectors emitted per stylesheet, as a guard against a pathological
/// list making every page carry a multi-megabyte stylesheet.
pub const MAX_SELECTORS: usize = 30_000;

/// Max length of a single selector.
pub const MAX_SELECTOR_LEN: usize = 1024;

/// True if `selector` is safe to emit as `selector{display:none!important}`.
pub fn is_safe_selector(selector: &str) -> bool {
    if selector.is_empty() || selector.len() > MAX_SELECTOR_LEN {
        return false;
    }
    let trimmed = selector.trim_start();
    if trimmed.starts_with('@') {
        return false;
    }
    for b in selector.bytes() {
        match b {
            b'{' | b'}' | b';' | b'<' | b'\0' | b'\n' | b'\r' => return false,
            _ => {}
        }
    }
    // Comments could swallow the rule's declaration block; url()/image-set()
    // make no sense in a selector and are the obvious exfiltration vector.
    let lower = selector.to_ascii_lowercase();
    !(lower.contains("/*")
        || lower.contains("*/")
        || lower.contains("url(")
        || lower.contains("image-set(")
        || lower.contains("@import"))
}

/// Builds `sel{display:none!important}` rules for every safe selector.
pub fn hide_stylesheet<'a>(selectors: impl IntoIterator<Item = &'a str>) -> String {
    let mut css = String::new();
    let mut emitted = 0usize;
    for sel in selectors {
        if emitted >= MAX_SELECTORS {
            break;
        }
        if !is_safe_selector(sel) {
            continue;
        }
        css.push_str(sel);
        css.push_str("{display:none!important}\n");
        emitted += 1;
    }
    css
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_breakouts() {
        assert!(is_safe_selector(".ad-banner"));
        assert!(is_safe_selector("div[id^=\"div-gpt-ad\"]"));
        assert!(is_safe_selector(".foo\\:bar > a:not(.x)"));
        assert!(!is_safe_selector("a{}body{background:url(//t.example/x)}"));
        assert!(!is_safe_selector("a}"));
        assert!(!is_safe_selector("@import url(//evil)"));
        assert!(!is_safe_selector("a /* x"));
        assert!(!is_safe_selector("a[style*=\"url(\"]"));
        assert!(!is_safe_selector(""));
        assert!(!is_safe_selector(&"a".repeat(MAX_SELECTOR_LEN + 1)));
    }

    #[test]
    fn one_rule_per_selector() {
        let css = hide_stylesheet([".a", "bad{", "#b"]);
        assert_eq!(css, ".a{display:none!important}\n#b{display:none!important}\n");
    }
}
