// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// uBO-style filter-list preprocessor (`!#if` / `!#else` / `!#endif`).
//
// adblock-rust treats `!#if` lines as comments, so without this step BOTH
// branches of every conditional block would be compiled — e.g. Safari-only
// or uBO-Lite-only workarounds would land in MoltBrowser. uBlock Origin's
// filters/privacy/badware lists use these directives heavily.
//
// `!#include` is dropped: the uAssets lists MoltShield ships are
// self-contained, and following includes would let a list pull extra
// content from arbitrary paths at update time.

use std::borrow::Cow;

/// Preprocessor environment. Each `!#if` token evaluates to true iff it is
/// listed in `true_tokens` (unknown tokens are false, as in uBO).
pub struct PreprocessEnv {
    true_tokens: Vec<&'static str>,
}

impl PreprocessEnv {
    /// The environment MoltBrowser presents: a Chromium browser running a
    /// uBO-syntax-compatible engine (adblock-rust) that supports user
    /// stylesheets but not HTML filtering, and is not uBO Lite (MV3).
    pub fn molt(is_mobile: bool) -> Self {
        let mut true_tokens = vec!["env_chromium", "cap_user_stylesheet", "ext_ublock"];
        if is_mobile {
            true_tokens.push("env_mobile");
        }
        PreprocessEnv { true_tokens }
    }

    fn eval_token(&self, token: &str) -> bool {
        token == "true" || self.true_tokens.iter().any(|t| *t == token)
    }
}

/// Evaluates a `!#if` expression. Grammar (as in uBO):
///   expr := or ; or := and ("||" and)* ; and := unary ("&&" unary)* ;
///   unary := "!" unary | "(" expr ")" | token
/// Malformed expressions evaluate to false so the guarded block is skipped.
fn eval_expression(expr: &str, env: &PreprocessEnv) -> bool {
    struct Parser<'a> {
        s: &'a [u8],
        pos: usize,
        env: &'a PreprocessEnv,
        ok: bool,
    }
    impl<'a> Parser<'a> {
        fn skip_ws(&mut self) {
            while self.pos < self.s.len() && self.s[self.pos].is_ascii_whitespace() {
                self.pos += 1;
            }
        }
        fn eat(&mut self, tok: &str) -> bool {
            self.skip_ws();
            if self.s[self.pos..].starts_with(tok.as_bytes()) {
                self.pos += tok.len();
                true
            } else {
                false
            }
        }
        fn or(&mut self, depth: u32) -> bool {
            let mut v = self.and(depth);
            while self.eat("||") {
                let rhs = self.and(depth);
                v = v || rhs;
            }
            v
        }
        fn and(&mut self, depth: u32) -> bool {
            let mut v = self.unary(depth);
            while self.eat("&&") {
                let rhs = self.unary(depth);
                v = v && rhs;
            }
            v
        }
        fn unary(&mut self, depth: u32) -> bool {
            if depth > 32 {
                self.ok = false;
                return false;
            }
            if self.eat("!") {
                return !self.unary(depth + 1);
            }
            if self.eat("(") {
                let v = self.or(depth + 1);
                if !self.eat(")") {
                    self.ok = false;
                }
                return v;
            }
            self.skip_ws();
            let start = self.pos;
            while self.pos < self.s.len()
                && (self.s[self.pos].is_ascii_alphanumeric() || self.s[self.pos] == b'_')
            {
                self.pos += 1;
            }
            if start == self.pos {
                self.ok = false;
                return false;
            }
            // Token bytes are ASCII alphanumerics, so this is valid UTF-8.
            let token = std::str::from_utf8(&self.s[start..self.pos]).unwrap_or("");
            self.env.eval_token(token)
        }
    }
    let mut p = Parser { s: expr.as_bytes(), pos: 0, env, ok: true };
    let v = p.or(0);
    p.skip_ws();
    p.ok && p.pos == p.s.len() && v
}

/// Applies `!#if` blocks and strips `!#include` lines. Returns the input
/// unchanged (borrowed) when it contains no directives.
pub fn preprocess<'a>(text: &'a str, env: &PreprocessEnv) -> Cow<'a, str> {
    if !text.contains("!#") {
        return Cow::Borrowed(text);
    }
    // Stack of (this block's condition, parent block active).
    let mut stack: Vec<(bool, bool)> = Vec::new();
    let mut active = true;
    let mut out = String::with_capacity(text.len());
    for line in text.lines() {
        let trimmed = line.trim();
        if let Some(rest) = trimmed.strip_prefix("!#if ") {
            let cond = eval_expression(rest, env);
            stack.push((cond, active));
            active = active && cond;
            continue;
        }
        if trimmed == "!#else" {
            if let Some(&(cond, parent)) = stack.last() {
                active = parent && !cond;
            }
            continue;
        }
        if trimmed == "!#endif" {
            if let Some((_, parent)) = stack.pop() {
                active = parent;
            }
            continue;
        }
        if trimmed.starts_with("!#include") {
            continue;
        }
        if active {
            out.push_str(line);
            out.push('\n');
        }
    }
    Cow::Owned(out)
}

/// Counts lines that are filters (not blank, not comments, not list headers).
/// Used for the "rules" figure in the MoltShield UI; adblock-rust doesn't
/// expose per-list counts without its `debug-info` feature.
pub fn count_filter_lines(text: &str) -> u32 {
    let mut n: u32 = 0;
    for line in text.lines() {
        let t = line.trim();
        if t.is_empty() || t.starts_with('!') || t.starts_with('[') {
            continue;
        }
        n = n.saturating_add(1);
    }
    n
}

#[cfg(test)]
mod tests {
    use super::*;

    fn env() -> PreprocessEnv {
        PreprocessEnv::molt(false)
    }

    #[test]
    fn expressions() {
        let e = env();
        assert!(eval_expression("env_chromium", &e));
        assert!(!eval_expression("env_safari", &e));
        assert!(eval_expression("!env_safari", &e));
        assert!(eval_expression("env_firefox || env_chromium", &e));
        assert!(!eval_expression("env_chromium && env_mobile", &e));
        assert!(eval_expression("!(env_mobile || env_safari)", &e));
        assert!(eval_expression("!ext_ubol", &e));
        assert!(!eval_expression("cap_html_filtering", &e));
        // Malformed -> false.
        assert!(!eval_expression("env_chromium &&", &e));
        assert!(!eval_expression("(env_chromium", &e));
        assert!(!eval_expression("", &e));
        assert!(eval_expression("env_mobile", &PreprocessEnv::molt(true)));
    }

    #[test]
    fn blocks_and_nesting() {
        let list = "a\n!#if env_safari\nsafari\n!#else\nnot-safari\n!#endif\n\
                    !#if env_chromium\nchromium\n!#if env_mobile\nmobile\n!#endif\n!#endif\n\
                    !#include other.txt\nb\n";
        let out = preprocess(list, &env());
        let lines: Vec<&str> = out.lines().collect();
        assert_eq!(lines, vec!["a", "not-safari", "chromium", "b"]);
    }

    #[test]
    fn unbalanced_directives_do_not_panic() {
        let out = preprocess("!#endif\n!#else\nx\n!#if env_safari\ny\n", &env());
        assert_eq!(out.lines().collect::<Vec<_>>(), vec!["x"]);
    }

    #[test]
    fn no_directives_borrows() {
        assert!(matches!(preprocess("||a.com^\n", &env()), Cow::Borrowed(_)));
    }

    #[test]
    fn counts_filters() {
        assert_eq!(count_filter_lines("[Adblock Plus]\n! c\n\n||a^\n##.b\n"), 2);
    }
}
