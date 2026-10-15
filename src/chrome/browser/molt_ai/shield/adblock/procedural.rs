// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Procedural cosmetic filters (uBO's `:has-text()`, `:upward()`,
// `:matches-css()`, `:xpath()`, ...) and action filters (`:remove()`,
// `:style()`, `:remove-attr()`, `:remove-class()`).
//
// MoltShield builds adblock-rust without its `css-validation` feature (it
// would need the Servo `cssparser`/`selectors` crates vendored), and without
// it adblock-rust doesn't parse procedural operators at all: a rule such as
// `example.com##.ad:has-text(Sponsored)` comes back as a plain "hide
// selector" (invalid CSS, so the stylesheet silently ignored it), and only
// action filters come back in `procedural_actions`, with their selector as
// one unparsed `css-selector` and `:style()` arguments unvalidated. So this
// module does that parsing itself, for the filters that apply to one page:
//
//  * plain selectors stay in the hide stylesheet;
//  * `:style()` on a plain selector becomes a user-stylesheet rule;
//  * everything else becomes a filter for the renderer's procedural engine
//    (src/chrome/renderer/molt_ai/shield/procedural_filters_js.h), as JSON.
//
// Everything here comes from filter lists, which are untrusted input (a
// compromised or buggy list update). The renderer never evaluates any of it
// as script: it is data for a fixed engine. On top of that this module
// vets it: regular expressions must have a shape that can't backtrack
// catastrophically, styles can't fetch anything or escape their rule,
// attribute/class names are plain tokens, and every size is capped. A
// filter that fails any check is dropped (and counted).
//
// Output JSON (one object per filter):
//   {"selector": [op, ...],           // applied left to right
//    "action": {"type": ...},         // absent: hide
//    "path": matcher,                 // optional :matches-path() precondition
//    "watch": ["attr", ...]}          // optional :watch-attr() names
// ops: {"type":"css-selector","arg":"sel"}   (see the engine for how a
//        leading combinator is resolved), "has-text"/"min-text-length"/
//      "matches-attr"/"matches-css[-before|-after]"/"upward"/"xpath", and
//      "has"/"not" whose arg is a nested op list.
// matcher: {"text":"..."} (substring), {"exact":"..."} or
//          {"re":"source","flags":"i"}.

use serde_json::{Value, json};

use crate::css;

/// Per-document caps.
pub const MAX_FILTERS: usize = 500;
pub const MAX_JSON_BYTES: usize = 256 * 1024;
pub const MAX_STYLES: usize = 64;
/// Per-filter caps.
const MAX_SELECTOR_LEN: usize = 2048;
const MAX_ARG_LEN: usize = 1024;
const MAX_DEPTH: u32 = 3;
const MAX_OPS: usize = 24;
pub const MAX_REGEX_LEN: usize = 512;
/// Unbounded quantifiers (`*`, `+`, `{n,}`) allowed in one regex.
const MAX_UNBOUNDED_QUANTIFIERS: usize = 2;
const MAX_STYLE_LEN: usize = 1024;
const MAX_DECLARATIONS: usize = 32;
const MAX_UPWARD: u64 = 255;
const MAX_TEXT_LENGTH: u64 = 100_000;

/// What `process` makes of one page's cosmetic filters.
#[derive(Debug, Default)]
pub struct Output {
    /// Plain CSS hide selectors (for `css::hide_stylesheet`).
    pub plain_hide_selectors: Vec<String>,
    /// `selector{prop:value!important;...}` rules from `:style()` filters
    /// with a plain selector.
    pub style_css: String,
    /// JSON array of filters for the procedural engine; empty if none.
    pub filters_json: String,
    /// Declaration blocks for `:style()` on procedural selectors; filters
    /// reference them by index (`{"type":"style","arg":N}`).
    pub styles: Vec<String>,
    /// Filters in `filters_json`.
    pub count: u32,
    /// Filters dropped as unsupported or unsafe (or over a cap).
    pub dropped: u32,
}

/// An action from adblock-rust's `ProceduralOrActionFilter` JSON.
struct RawAction {
    kind: String,
    arg: Option<String>,
}

/// Reads adblock-rust's `ProceduralOrActionFilter` JSON. Without
/// css-validation its selector is always one `css-selector`; anything else
/// isn't something this module knows how to vet.
fn parse_raw_filter(raw: &str) -> Option<(String, Option<RawAction>)> {
    let v: Value = serde_json::from_str(raw).ok()?;
    let [op] = v.get("selector")?.as_array()?.as_slice() else { return None };
    if op.get("type")?.as_str()? != "css-selector" {
        return None;
    }
    let selector = op.get("arg")?.as_str()?.to_string();
    let action = match v.get("action") {
        None | Some(Value::Null) => None,
        Some(a) => Some(RawAction {
            kind: a.get("type")?.as_str()?.to_string(),
            arg: a.get("arg").and_then(Value::as_str).map(str::to_string),
        }),
    };
    Some((selector, action))
}

/// One parsed filter before serialization.
struct Filter {
    ops: Vec<Value>,
    path: Option<Value>,
    watch: Vec<String>,
}

/// Splits `hide_selectors` (adblock-rust's, procedural ones included) and
/// `actions` (adblock-rust's `procedural_actions` JSON strings) into a
/// stylesheet part and the procedural engine's input.
pub fn process<'a>(
    hide_selectors: impl IntoIterator<Item = &'a str>,
    actions: impl IntoIterator<Item = &'a str>,
) -> Output {
    let mut out = Output::default();
    let mut filters: Vec<Value> = Vec::new();
    let mut style_rules: Vec<String> = Vec::new();

    let mut push = |out: &mut Output, filter: Filter, action: Option<Value>| {
        if filters.len() >= MAX_FILTERS {
            out.dropped += 1;
            return;
        }
        let mut obj = serde_json::Map::new();
        obj.insert("selector".into(), Value::Array(filter.ops));
        if let Some(action) = action {
            obj.insert("action".into(), action);
        }
        if let Some(path) = filter.path {
            obj.insert("path".into(), path);
        }
        if !filter.watch.is_empty() {
            obj.insert("watch".into(), json!(filter.watch));
        }
        filters.push(Value::Object(obj));
    };

    for selector in hide_selectors {
        if !has_procedural_syntax(selector) {
            out.plain_hide_selectors.push(selector.to_string());
            continue;
        }
        match parse_filter(selector) {
            Some(filter) => push(&mut out, filter, None),
            None => out.dropped += 1,
        }
    }

    for raw in actions {
        let Some((selector, raw_action)) = parse_raw_filter(raw) else {
            out.dropped += 1;
            continue;
        };
        let selector = selector.as_str();
        let plain = !has_procedural_syntax(selector);
        let action = match raw_action {
            None => None,
            Some(a) => match sanitize_action(&a) {
                Some(a) => Some(a),
                None => {
                    out.dropped += 1;
                    continue;
                }
            },
        };
        match action {
            // `:style()` on a plain selector: a user-stylesheet rule.
            Some(ActionKind::Style(decls)) if plain => {
                if css::is_safe_selector(selector) {
                    style_rules.push(format!("{selector}{{{decls}}}\n"));
                } else {
                    out.dropped += 1;
                }
            }
            action => {
                let Some(filter) = parse_filter(selector) else {
                    out.dropped += 1;
                    continue;
                };
                let action = match action {
                    None => None,
                    Some(ActionKind::Style(decls)) => {
                        let index = match out.styles.iter().position(|s| *s == decls) {
                            Some(i) => i,
                            None if out.styles.len() < MAX_STYLES => {
                                out.styles.push(decls);
                                out.styles.len() - 1
                            }
                            None => {
                                out.dropped += 1;
                                continue;
                            }
                        };
                        Some(json!({"type": "style", "arg": index}))
                    }
                    Some(ActionKind::Other(v)) => Some(v),
                };
                push(&mut out, filter, action);
            }
        }
    }

    style_rules.sort_unstable();
    out.style_css = style_rules.concat();

    // Deterministic order (the inputs come from hash sets), then the byte
    // cap: filters past it are dropped.
    let mut encoded: Vec<String> = filters.iter().map(Value::to_string).collect();
    encoded.sort_unstable();
    let mut json = String::from("[");
    for filter in encoded {
        if json.len() + filter.len() + 2 > MAX_JSON_BYTES {
            out.dropped += 1;
            continue;
        }
        if out.count > 0 {
            json.push(',');
        }
        json.push_str(&filter);
        out.count += 1;
    }
    if out.count > 0 {
        json.push(']');
        out.filters_json = json;
    }
    out
}

enum ActionKind {
    /// Sanitized declaration block.
    Style(String),
    Other(Value),
}

fn sanitize_action(a: &RawAction) -> Option<ActionKind> {
    let arg = a.arg.as_deref().unwrap_or("");
    match a.kind.as_str() {
        "remove" => Some(ActionKind::Other(json!({"type": "remove"}))),
        "style" => sanitize_style(arg).map(ActionKind::Style),
        "remove-attr" if is_attr_name(arg) => {
            Some(ActionKind::Other(json!({"type": "remove-attr", "arg": arg})))
        }
        "remove-class" if is_class_name(arg) => {
            Some(ActionKind::Other(json!({"type": "remove-class", "arg": arg})))
        }
        _ => None,
    }
}

fn is_attr_name(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 128
        && s.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b':' | b'.'))
}

fn is_class_name(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 128
        && s.chars().all(|c| c.is_alphanumeric() || c == '-' || c == '_')
}

/// Turns a `:style()` argument into `prop:value!important;...`, or None if
/// it could do more than style the matched elements.
pub fn sanitize_style(style: &str) -> Option<String> {
    if style.is_empty() || style.len() > MAX_STYLE_LEN {
        return None;
    }
    let lower = style.to_ascii_lowercase();
    // No escaping the declaration block, no comments or escapes that could
    // hide either, nothing that loads a resource or (in old engines) runs
    // code.
    const FORBIDDEN: &[&str] = &[
        "/*", "*/", "url(", "image-set(", "image(", "cross-fade(", "element(", "expression(",
        "-moz-binding", "behavior", "javascript:", "attr(",
    ];
    if style.bytes().any(|b| matches!(b, b'{' | b'}' | b'<' | b'>' | b'\\' | b'@' | 0..=0x1f | 0x7f))
        || FORBIDDEN.iter().any(|bad| lower.contains(bad))
    {
        return None;
    }
    let mut out = Vec::new();
    for decl in split_top_level(style, b';') {
        let decl = decl.trim();
        if decl.is_empty() {
            continue;
        }
        let (prop, value) = decl.split_once(':')?;
        let prop = prop.trim().to_ascii_lowercase();
        let mut value = value.trim();
        if let Some(v) = value.strip_suffix("!important") {
            value = v.trim_end();
        }
        let prop_ok = !prop.is_empty()
            && prop.len() <= 64
            && prop.trim_start_matches('-').starts_with(|c: char| c.is_ascii_alphabetic())
            && prop.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-');
        if !prop_ok || value.is_empty() || value.contains('!') || value.contains(':') {
            return None;
        }
        out.push(format!("{prop}:{value}!important"));
    }
    if out.is_empty() || out.len() > MAX_DECLARATIONS {
        return None;
    }
    Some(out.join(";"))
}

/// Splits at `sep` outside quotes and parentheses.
fn split_top_level(s: &str, sep: u8) -> Vec<&str> {
    let b = s.as_bytes();
    let (mut depth, mut quote, mut start) = (0i32, 0u8, 0usize);
    let mut parts = Vec::new();
    let mut i = 0;
    while i < b.len() {
        let c = b[i];
        if quote != 0 {
            if c == b'\\' {
                i += 1;
            } else if c == quote {
                quote = 0;
            }
        } else if c == b'"' || c == b'\'' {
            quote = c;
        } else if c == b'(' {
            depth += 1;
        } else if c == b')' {
            depth -= 1;
        } else if c == sep && depth == 0 {
            parts.push(&s[start..i]);
            start = i + 1;
        }
        i += 1;
    }
    parts.push(&s[start..]);
    parts
}

// ---------------------------------------------------------------------------
// Selector parsing

/// Operators whose argument is a string (not a selector).
const STRING_OPS: &[&str] = &[
    "has-text",
    "-abp-contains",
    "contains",
    "matches-attr",
    "matches-css",
    "matches-css-before",
    "matches-css-after",
    "matches-path",
    "min-text-length",
    "upward",
    "nth-ancestor",
    "xpath",
    "watch-attr",
];
/// Operators whose argument is a selector: native CSS unless the argument
/// is itself procedural.
const SELECTOR_OPS: &[&str] = &["has", "-abp-has", "if", "not", "if-not"];
/// uBO operators MoltShield doesn't implement; a filter using one is
/// dropped rather than applied partially.
const UNSUPPORTED_OPS: &[&str] = &[
    "others",
    "shadow",
    "matches-media",
    "matches-prop",
    "matches-property",
    "spath",
    "remove",
    "style",
    "remove-attr",
    "remove-class",
    "-abp-properties",
    "properties",
    "subject",
];

/// True if `selector` uses any procedural/action syntax (cheap pre-check;
/// `parse_filter` decides precisely).
pub fn has_procedural_syntax(selector: &str) -> bool {
    if !selector.contains(":") {
        return false;
    }
    let mut found = false;
    scan_pseudos(selector, &mut |name, _arg| {
        if STRING_OPS.contains(&name)
            || UNSUPPORTED_OPS.contains(&name)
            || matches!(name, "-abp-has" | "if" | "if-not")
        {
            found = true;
        }
    });
    found
}

/// Calls `f(name, arg)` for every functional pseudo-class `:name(arg)` at
/// any nesting depth. Best-effort; malformed input just stops the scan.
fn scan_pseudos(s: &str, f: &mut dyn FnMut(&str, &str)) {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'\\' => i += 2,
            b'[' => match skip_brackets(b, i) {
                Some(j) => i = j,
                None => return,
            },
            b':' => {
                let start = i + 1;
                let mut j = start;
                while j < b.len() && is_ident_byte(b[j]) {
                    j += 1;
                }
                if j < b.len() && b[j] == b'(' {
                    let Some(close) = find_close_paren(b, j) else { return };
                    let arg = &s[j + 1..close];
                    f(&s[start..j], arg);
                    scan_pseudos(arg, f);
                    i = close + 1;
                } else {
                    i = j.max(i + 1);
                }
            }
            _ => i += 1,
        }
    }
}

fn is_ident_byte(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'-' || c == b'_'
}

/// Index just past the `]` closing the `[` at `open` (quotes respected).
fn skip_brackets(b: &[u8], open: usize) -> Option<usize> {
    let mut i = open + 1;
    let mut quote = 0u8;
    while i < b.len() {
        let c = b[i];
        if c == b'\\' {
            i += 2;
            continue;
        }
        if quote != 0 {
            if c == quote {
                quote = 0;
            }
        } else if c == b'"' || c == b'\'' {
            quote = c;
        } else if c == b']' {
            return Some(i + 1);
        }
        i += 1;
    }
    None
}

/// Index of the `)` matching the `(` at `open`: escapes skip a byte, parens
/// nest. Quotes are not special (uBO's has-text arguments are free text,
/// e.g. `:has-text(Don't miss)`).
fn find_close_paren(b: &[u8], open: usize) -> Option<usize> {
    let mut depth = 0i32;
    let mut i = open;
    while i < b.len() {
        match b[i] {
            b'\\' => i += 1,
            b'(' => depth += 1,
            b')' => {
                depth -= 1;
                if depth == 0 {
                    return Some(i);
                }
            }
            _ => {}
        }
        i += 1;
    }
    None
}

/// Where a selector's first css part is resolved.
#[derive(Clone, Copy, PartialEq)]
enum Context {
    /// From the document (a filter's start).
    Document,
    /// Relative to the element (`:has()`): a bare compound means
    /// "descendant".
    Relative,
    /// The element itself (`:not()`).
    SelfElement,
}

fn parse_filter(selector: &str) -> Option<Filter> {
    if selector.len() > MAX_SELECTOR_LEN {
        return None;
    }
    let mut filter = Filter { ops: Vec::new(), path: None, watch: Vec::new() };
    let ops = parse_selector(selector, 0, Context::Document, &mut filter)?;
    if ops.is_empty() {
        return None;
    }
    filter.ops = ops;
    Some(filter)
}

fn parse_selector(
    sel: &str,
    depth: u32,
    ctx: Context,
    filter: &mut Filter,
) -> Option<Vec<Value>> {
    if depth > MAX_DEPTH {
        return None;
    }
    let b = sel.as_bytes();
    let mut ops: Vec<Value> = Vec::new();
    let mut css = String::new();
    let mut seg_start = 0;
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'\\' => i += 2,
            b'[' => i = skip_brackets(b, i)?,
            b':' if b.get(i + 1) == Some(&b':') => i += 2,
            b':' => {
                let start = i + 1;
                let mut j = start;
                while j < b.len() && is_ident_byte(b[j]) {
                    j += 1;
                }
                if j >= b.len() || b[j] != b'(' {
                    i = j.max(i + 1);
                    continue;
                }
                let close = find_close_paren(b, j)?;
                let name = &sel[start..j];
                let arg = &sel[j + 1..close];
                if arg.len() > MAX_ARG_LEN {
                    return None;
                }
                if UNSUPPORTED_OPS.contains(&name) {
                    return None;
                }
                if SELECTOR_OPS.contains(&name) {
                    css.push_str(&sel[seg_start..i]);
                    if !has_procedural_syntax(arg) {
                        // Native CSS (canonical names for uBO's aliases).
                        match name {
                            "has" | "not" => css.push_str(&sel[i..=close]),
                            "-abp-has" | "if" => {
                                css.push_str(":has(");
                                css.push_str(arg);
                                css.push(')');
                            }
                            _ => {
                                css.push_str(":not(:has(");
                                css.push_str(arg);
                                css.push_str("))");
                            }
                        }
                    } else {
                        flush_css(&mut ops, &mut css, ctx)?;
                        // `:not(x)` tests the element itself; `:has(x)` (and
                        // `:if-not(x)`, i.e. `:not(:has(x))`) its relatives.
                        let inner_ctx =
                            if name == "not" { Context::SelfElement } else { Context::Relative };
                        let inner = parse_selector(arg, depth + 1, inner_ctx, filter)?;
                        let op = match name {
                            "not" => json!({"type": "not", "arg": inner}),
                            "if-not" => {
                                json!({"type": "not", "arg": [{"type": "has", "arg": inner}]})
                            }
                            _ => json!({"type": "has", "arg": inner}),
                        };
                        ops.push(op);
                    }
                    seg_start = close + 1;
                    i = close + 1;
                    continue;
                }
                if STRING_OPS.contains(&name) {
                    css.push_str(&sel[seg_start..i]);
                    flush_css(&mut ops, &mut css, ctx)?;
                    match string_op(name, arg)? {
                        StringOp::Op(op) => ops.push(op),
                        StringOp::Path(m) => {
                            // A page-level precondition: only meaningful once,
                            // at the top level.
                            if depth != 0 || filter.path.is_some() {
                                return None;
                            }
                            filter.path = Some(m);
                        }
                        StringOp::Watch(attrs) => filter.watch.extend(attrs),
                    }
                    seg_start = close + 1;
                    i = close + 1;
                    continue;
                }
                // Any other functional pseudo-class (:nth-child, :is, ...)
                // is native CSS, as long as nothing procedural hides inside.
                if has_procedural_syntax(arg) {
                    return None;
                }
                i = close + 1;
            }
            _ => i += 1,
        }
        if ops.len() > MAX_OPS {
            return None;
        }
    }
    css.push_str(&sel[seg_start.min(sel.len())..]);
    flush_css(&mut ops, &mut css, ctx)?;
    if ops.len() > MAX_OPS {
        return None;
    }
    Some(ops)
}

/// Turns pending CSS text into css-selector op(s). The first part of a
/// selector is resolved per `ctx`; later parts are relative to the previous
/// op's elements: a leading combinator (` `, `>`, `+`, `~`) selects from
/// them, no combinator filters them (`:has-text(x).foo`).
fn flush_css(ops: &mut Vec<Value>, css: &mut String, ctx: Context) -> Option<()> {
    let text = std::mem::take(css);
    if text.trim().is_empty() {
        return Some(());
    }
    let first = ops.is_empty();
    let starts_ws = text.starts_with(|c: char| c.is_ascii_whitespace());
    let trimmed = text.trim();
    let part = if first {
        match ctx {
            Context::Document => {
                if trimmed.starts_with(['>', '+', '~']) {
                    return None;
                }
                trimmed.to_string()
            }
            Context::Relative => {
                if trimmed.starts_with(['>', '+', '~']) {
                    trimmed.to_string()
                } else {
                    format!(" {trimmed}")
                }
            }
            Context::SelfElement => trimmed.to_string(),
        }
    } else if trimmed.starts_with(['>', '+', '~']) {
        trimmed.to_string()
    } else if starts_ws {
        format!(" {trimmed}")
    } else {
        trimmed.to_string()
    };
    if part.len() > MAX_SELECTOR_LEN {
        return None;
    }
    // `+ a > b` / `~ a b`: the sibling step and the rest are separate ops
    // (the engine resolves a sibling combinator one compound at a time).
    if part.starts_with(['+', '~']) {
        let (step, rest) = split_first_compound(&part);
        ops.push(json!({"type": "css-selector", "arg": step}));
        if !rest.trim().is_empty() {
            let mut rest = rest.to_string();
            return flush_css(ops, &mut rest, ctx);
        }
        return Some(());
    }
    ops.push(json!({"type": "css-selector", "arg": part}));
    Some(())
}

/// Splits `+ a.b > c` into (`+ a.b`, ` > c`): the combinator and its first
/// compound selector, and the rest (starting with whitespace or a
/// combinator).
fn split_first_compound(part: &str) -> (&str, &str) {
    let b = part.as_bytes();
    let mut i = 1;
    while i < b.len() && b[i].is_ascii_whitespace() {
        i += 1;
    }
    while i < b.len() {
        match b[i] {
            b'\\' => i += 2,
            b'[' => match skip_brackets(b, i) {
                Some(j) => i = j,
                None => return (part, ""),
            },
            b'(' => match find_close_paren(b, i) {
                Some(j) => i = j + 1,
                None => return (part, ""),
            },
            c if c.is_ascii_whitespace() || matches!(c, b'>' | b'+' | b'~') => {
                return (&part[..i], &part[i..]);
            }
            _ => i += 1,
        }
    }
    (part, "")
}

enum StringOp {
    Op(Value),
    Path(Value),
    Watch(Vec<String>),
}

fn string_op(name: &str, arg: &str) -> Option<StringOp> {
    let arg = arg.trim();
    Some(StringOp::Op(match name {
        "has-text" | "-abp-contains" | "contains" => {
            json!({"type": "has-text", "arg": text_matcher(arg, false)?})
        }
        "min-text-length" => {
            let n: u64 = arg.parse().ok().filter(|n| *n <= MAX_TEXT_LENGTH)?;
            json!({"type": "min-text-length", "arg": n})
        }
        "upward" | "nth-ancestor" => {
            if let Ok(n) = arg.parse::<u64>() {
                if n == 0 || n > MAX_UPWARD {
                    return None;
                }
                json!({"type": "upward", "arg": n})
            } else if name == "upward" && !arg.is_empty() && !has_procedural_syntax(arg) {
                json!({"type": "upward", "arg": {"css": arg}})
            } else {
                return None;
            }
        }
        "xpath" => {
            let expr = unquote(arg);
            if expr.is_empty() {
                return None;
            }
            json!({"type": "xpath", "arg": expr})
        }
        "matches-attr" => {
            let (name, value) = split_key_value(arg)?;
            let name = text_matcher(name, true)?;
            let value = match value {
                Some(v) => text_matcher(v, true)?,
                None => Value::Null,
            };
            json!({"type": "matches-attr", "arg": {"name": name, "value": value}})
        }
        "matches-css" | "matches-css-before" | "matches-css-after" => {
            let (prop, value) = arg.split_once(':')?;
            let prop = prop.trim().to_ascii_lowercase();
            if prop.is_empty() || !prop.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-') {
                return None;
            }
            json!({"type": name, "arg": {"prop": prop, "value": text_matcher(value.trim(), true)?}})
        }
        "matches-path" => return Some(StringOp::Path(text_matcher(arg, false)?)),
        "watch-attr" => {
            let attrs: Vec<String> =
                arg.split(',').map(|a| a.trim().to_string()).filter(|a| !a.is_empty()).collect();
            if attrs.is_empty() || attrs.len() > 16 || !attrs.iter().all(|a| is_attr_name(a)) {
                return None;
            }
            return Some(StringOp::Watch(attrs));
        }
        _ => return None,
    }))
}

/// `name` or `name=value` / `"name"="value"` / `/re/=/re/` (uBO's
/// `:matches-attr()` syntax).
fn split_key_value(arg: &str) -> Option<(&str, Option<&str>)> {
    let b = arg.as_bytes();
    // The key ends at the first `=` outside quotes and regex literal.
    let mut i = 0;
    let mut quote = 0u8;
    let in_regex = b.first() == Some(&b'/');
    let mut regex_open = in_regex;
    if in_regex {
        i = 1;
    }
    while i < b.len() {
        let c = b[i];
        if c == b'\\' {
            i += 2;
            continue;
        }
        if regex_open {
            if c == b'/' {
                regex_open = false;
            }
        } else if quote != 0 {
            if c == quote {
                quote = 0;
            }
        } else if c == b'"' || c == b'\'' {
            quote = c;
        } else if c == b'=' {
            return Some((&arg[..i], Some(&arg[i + 1..])));
        }
        i += 1;
    }
    Some((arg, None))
}

/// `arg` without one pair of surrounding quotes.
fn unquote(arg: &str) -> &str {
    if arg.len() >= 2
        && ((arg.starts_with('"') && arg.ends_with('"'))
            || (arg.starts_with('\'') && arg.ends_with('\'')))
    {
        &arg[1..arg.len() - 1]
    } else {
        arg
    }
}

/// A string or `/regex/flags` argument. Plain text is a substring match
/// (`exact` makes it a whole-value match, as for attribute names and CSS
/// values, which may also be quoted -- `"name"="/re/"`, as in uBO).
fn text_matcher(arg: &str, exact: bool) -> Option<Value> {
    if arg.len() > MAX_ARG_LEN {
        return None;
    }
    let arg = if exact { unquote(arg) } else { arg };
    if arg.len() >= 2 && arg.starts_with('/') {
        if let Some(end) = arg.rfind('/') {
            if end > 0 {
                let source = &arg[1..end];
                let flags = &arg[end + 1..];
                if flags.bytes().all(|f| matches!(f, b'i' | b'm' | b's' | b'u' | b'g' | b'y')) {
                    // `g`/`y` make RegExp#test stateful: drop them.
                    let flags: String =
                        flags.chars().filter(|f| matches!(f, 'i' | 'm' | 's' | 'u')).collect();
                    if !is_safe_regex(source) {
                        return None;
                    }
                    return Some(json!({"re": source, "flags": flags}));
                }
            }
        }
    }
    Some(if exact { json!({"exact": arg}) } else { json!({"text": arg}) })
}

/// Conservative check that a JS regular expression can't backtrack
/// catastrophically: bounded length, no backreferences, no quantified group
/// that itself contains a quantifier or an alternation (`(a+)+`, `(a|ab)*`),
/// and at most MAX_UNBOUNDED_QUANTIFIERS unbounded quantifiers. Rejects some
/// harmless patterns; filters using them are dropped.
pub fn is_safe_regex(src: &str) -> bool {
    if src.is_empty() || src.len() > MAX_REGEX_LEN {
        return false;
    }
    let b = src.as_bytes();
    // Per open group: (contains a repetition, contains an alternation).
    let mut stack: Vec<(bool, bool)> = vec![(false, false)];
    // Inner state of the group that was just closed, if the last atom was a
    // group; whether there is an atom a quantifier could apply to.
    let mut last_group: Option<(bool, bool)> = None;
    let mut have_atom = false;
    let mut unbounded = 0usize;
    let mut i = 0;
    while i < b.len() {
        let c = b[i];
        match c {
            b'\\' => {
                let Some(&n) = b.get(i + 1) else { return false };
                if (n.is_ascii_digit() && n != b'0') || n == b'k' {
                    return false; // backreference
                }
                i += 2;
                have_atom = true;
                last_group = None;
            }
            b'[' => {
                // Character class: skip to the unescaped `]` (a `]` right
                // after `[` or `[^` is literal).
                let mut j = i + 1;
                if b.get(j) == Some(&b'^') {
                    j += 1;
                }
                if b.get(j) == Some(&b']') {
                    j += 1;
                }
                while j < b.len() && b[j] != b']' {
                    if b[j] == b'\\' {
                        j += 1;
                    }
                    j += 1;
                }
                if j >= b.len() {
                    return false;
                }
                i = j + 1;
                have_atom = true;
                last_group = None;
            }
            b'(' => {
                stack.push((false, false));
                i += 1;
                if b.get(i) == Some(&b'?') {
                    // (?: (?= (?! (?<= (?<! (?<name>
                    i += 1;
                    match b.get(i) {
                        Some(b':' | b'=' | b'!') => i += 1,
                        Some(b'<') => {
                            i += 1;
                            if matches!(b.get(i), Some(b'=' | b'!')) {
                                i += 1;
                            } else {
                                while i < b.len() && b[i] != b'>' {
                                    i += 1;
                                }
                                i += 1;
                            }
                        }
                        _ => return false,
                    }
                }
                have_atom = false;
                last_group = None;
            }
            b')' => {
                let inner = stack.pop().unwrap_or((false, false));
                let Some(parent) = stack.last_mut() else { return false };
                parent.0 |= inner.0;
                parent.1 |= inner.1;
                last_group = Some(inner);
                have_atom = true;
                i += 1;
            }
            b'|' => {
                if let Some(top) = stack.last_mut() {
                    top.1 = true;
                }
                have_atom = false;
                last_group = None;
                i += 1;
            }
            b'*' | b'+' | b'?' | b'{' => {
                let (repeats, is_unbounded, len) = match c {
                    b'?' => (false, false, 1),
                    b'*' | b'+' => (true, true, 1),
                    _ => match parse_braces(&b[i..]) {
                        Some((min, max, len)) => {
                            if max.is_some_and(|m| m < min) {
                                return false;
                            }
                            (max != Some(0) && max != Some(1), max.is_none(), len)
                        }
                        // A `{` that isn't a quantifier is a literal.
                        None => {
                            have_atom = true;
                            last_group = None;
                            i += 1;
                            continue;
                        }
                    },
                };
                if !have_atom {
                    return false;
                }
                if repeats {
                    if let Some((rep, alt)) = last_group {
                        if rep || alt {
                            return false;
                        }
                    }
                    if let Some(top) = stack.last_mut() {
                        top.0 = true;
                    }
                }
                if is_unbounded {
                    unbounded += 1;
                    if unbounded > MAX_UNBOUNDED_QUANTIFIERS {
                        return false;
                    }
                }
                i += len;
                if b.get(i) == Some(&b'?') {
                    i += 1; // lazy
                }
                have_atom = false;
                last_group = None;
            }
            _ => {
                have_atom = true;
                last_group = None;
                i += 1;
            }
        }
    }
    stack.len() == 1
}

/// Parses `{n}`, `{n,}` or `{n,m}` at the start of `b`: (min, max, length).
fn parse_braces(b: &[u8]) -> Option<(u64, Option<u64>, usize)> {
    let end = b.iter().position(|&c| c == b'}')?;
    let inner = std::str::from_utf8(&b[1..end]).ok()?;
    let (min, max) = match inner.split_once(',') {
        None => {
            let n = inner.parse().ok()?;
            (n, Some(n))
        }
        Some((a, "")) => (a.parse().ok()?, None),
        Some((a, z)) => (a.parse().ok()?, Some(z.parse().ok()?)),
    };
    Some((min, max, end + 1))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ops(selector: &str) -> Option<String> {
        parse_filter(selector).map(|f| {
            let mut v = json!({"selector": f.ops});
            if let Some(p) = f.path {
                v["path"] = p;
            }
            if !f.watch.is_empty() {
                v["watch"] = json!(f.watch);
            }
            v.to_string()
        })
    }

    #[test]
    fn plain_selectors_are_not_procedural() {
        for s in [".ad", "div:has(> .sponsor)", "a:not(.x):nth-child(2n+1)", "[title=\":has-text(\"]"] {
            assert!(!has_procedural_syntax(s), "{s}");
        }
        for s in [".ad:has-text(x)", "div:has(span:has-text(Ad))", ":xpath(//a)", ".a:upward(2)"] {
            assert!(has_procedural_syntax(s), "{s}");
        }
    }

    #[test]
    fn parses_operators() {
        assert_eq!(
            ops(".card:has-text(/Promo(ted)?/i)").unwrap(),
            r#"{"selector":[{"arg":".card","type":"css-selector"},{"arg":{"flags":"i","re":"Promo(ted)?"},"type":"has-text"}]}"#
        );
        assert_eq!(
            ops("div.card:has(span:has-text(Sponsored))").unwrap(),
            r#"{"selector":[{"arg":"div.card","type":"css-selector"},{"arg":[{"arg":" span","type":"css-selector"},{"arg":{"text":"Sponsored"},"type":"has-text"}],"type":"has"}]}"#
        );
        assert_eq!(
            ops("h2:has(span:matches-css-before(content: /Sponsored/)) + div > a").unwrap(),
            r#"{"selector":[{"arg":"h2","type":"css-selector"},{"arg":[{"arg":" span","type":"css-selector"},{"arg":{"prop":"content","value":{"flags":"","re":"Sponsored"}},"type":"matches-css-before"}],"type":"has"},{"arg":"+ div","type":"css-selector"},{"arg":"> a","type":"css-selector"}]}"#
        );
        assert_eq!(
            ops(".x:upward(3)").unwrap(),
            r#"{"selector":[{"arg":".x","type":"css-selector"},{"arg":3,"type":"upward"}]}"#
        );
        assert_eq!(
            ops(":xpath(//span[(text()='Ad')]/../..)").unwrap(),
            r#"{"selector":[{"arg":"//span[(text()='Ad')]/../..","type":"xpath"}]}"#
        );
        assert_eq!(
            ops(":xpath('//*[(text()=\"AD\")]/..')").unwrap(),
            r#"{"selector":[{"arg":"//*[(text()=\"AD\")]/..","type":"xpath"}]}"#
        );
        // matches-attr: quoted names/values, regex or literal.
        assert_eq!(
            ops("div[id]:matches-attr(\"/trackid$/\")").unwrap(),
            r#"{"selector":[{"arg":"div[id]","type":"css-selector"},{"arg":{"name":{"flags":"","re":"trackid$"},"value":null},"type":"matches-attr"}]}"#
        );
        assert_eq!(
            ops("a:matches-attr(\"data-x\"=\"/^ad/i\")").unwrap(),
            r#"{"selector":[{"arg":"a","type":"css-selector"},{"arg":{"name":{"exact":"data-x"},"value":{"flags":"i","re":"^ad"}},"type":"matches-attr"}]}"#
        );
        // :matches-path() becomes a precondition; :watch-attr() a list.
        assert_eq!(
            ops(":matches-path(/^\\/$/) div[aria-posinset]:watch-attr(class):has-text(Ad)").unwrap(),
            r#"{"path":{"flags":"","re":"^\\/$"},"selector":[{"arg":"div[aria-posinset]","type":"css-selector"},{"arg":{"text":"Ad"},"type":"has-text"}],"watch":["class"]}"#
        );
        // Aliases and :not() with a procedural argument.
        assert_eq!(
            ops("p:-abp-contains(Ad):not(p:has-text(/MAHA/i))").unwrap(),
            r#"{"selector":[{"arg":"p","type":"css-selector"},{"arg":{"text":"Ad"},"type":"has-text"},{"arg":[{"arg":"p","type":"css-selector"},{"arg":{"flags":"i","re":"MAHA"},"type":"has-text"}],"type":"not"}]}"#
        );
        // Native :has()/:not() stay in the CSS part; -abp-has is renamed.
        assert_eq!(
            ops("div:-abp-has(> img):has-text(x)").unwrap(),
            r#"{"selector":[{"arg":"div:has(> img)","type":"css-selector"},{"arg":{"text":"x"},"type":"has-text"}]}"#
        );
        // Free text with quotes and escaped parens.
        assert_eq!(
            ops(".m:has-text(Don't miss \\(ad\\))").unwrap(),
            r#"{"selector":[{"arg":".m","type":"css-selector"},{"arg":{"text":"Don't miss \\(ad\\)"},"type":"has-text"}]}"#
        );
    }

    #[test]
    fn rejects_unsupported_or_unsafe() {
        for s in [
            ".a:others()",
            ".a:matches-media(screen)",
            ".a:has-text(/(a+)+$/)",
            ".a:has-text(/(x|xy)*z/)",
            ".a:has-text(/(a)\\1/)",
            ".a:has-text(/a.*b.*c.*d/)",
            ".a:upward(0)",
            ".a:upward(300)",
            ".a:upward(.b:has-text(x))",
            ".a:min-text-length(-1)",
            ".a:is(.b:has-text(x))",
            "> .a:has-text(x)",
            ".a:has-text(x",
            ".a:watch-attr(on<x)",
            ":matches-path(/a/) .x:has(:matches-path(/b/))",
        ] {
            assert!(ops(s).is_none(), "{s} -> {:?}", ops(s));
        }
    }

    #[test]
    fn regex_shapes() {
        for ok in ["Sponsored", "^(Ad|Sponsored)$", "Promo(ted)?", "a.*b", "\\d{2,4}", "[(]x", "(?:ab){2}", "(?<=a)b"] {
            assert!(is_safe_regex(ok), "{ok}");
        }
        for bad in ["(a+)+", "(a*)*b", "(a|b)+", "(?:x|y){2,}", "(.*a){3}", "\\1", "a**", "(", ")", "[a", "a.*b.*c.*", ""] {
            assert!(!is_safe_regex(bad), "{bad}");
        }
        assert!(!is_safe_regex(&"a".repeat(MAX_REGEX_LEN + 1)));
    }

    #[test]
    fn styles_are_sanitized() {
        assert_eq!(
            sanitize_style("height: 0 !important; overflow: hidden !important;").as_deref(),
            Some("height:0!important;overflow:hidden!important")
        );
        assert_eq!(sanitize_style("color: red").as_deref(), Some("color:red!important"));
        for bad in [
            "background: url(http://t.example/x)",
            "background-image: image-set('x.png' 1x)",
            "color: red} body{display:none",
            "content: '\\'",
            "color: red /* x */",
            "--x: 1; @import 'y'",
            "width: attr(data-x)",
            "color red",
            "color: red !important !important",
            "",
        ] {
            assert!(sanitize_style(bad).is_none(), "{bad}");
        }
    }

    #[test]
    fn process_splits_and_caps() {
        let out = process(
            [".plain", ".ad:has-text(Sponsored)", ".bad:others()"],
            [
                r#"{"selector":[{"type":"css-selector","arg":"body"}],"action":{"type":"style","arg":"overflow: auto !important"}}"#,
                r#"{"selector":[{"type":"css-selector","arg":".y"}],"action":{"type":"style","arg":"background: url(http://t/x)"}}"#,
                r#"{"selector":[{"type":"css-selector","arg":".z:has-text(Ad)"}],"action":{"type":"style","arg":"opacity: 0"}}"#,
                r#"{"selector":[{"type":"css-selector","arg":".x"}],"action":{"type":"remove"}}"#,
                r#"{"selector":[{"type":"css-selector","arg":".w"}],"action":{"type":"remove-attr","arg":"onclick"}}"#,
                r#"{"selector":[{"type":"css-selector","arg":".v"}],"action":{"type":"remove-class","arg":"a b"}}"#,
                "not json",
            ],
        );
        assert_eq!(out.plain_hide_selectors, [".plain"]);
        assert_eq!(out.style_css, "body{overflow:auto!important}\n");
        assert_eq!(out.styles, ["opacity:0!important"]);
        assert_eq!(out.count, 4);
        assert_eq!(out.dropped, 4); // :others(), url(), "a b", bad JSON
        let v: Value = serde_json::from_str(&out.filters_json).unwrap();
        let text = v.to_string();
        assert!(text.contains(r#"{"action":{"arg":0,"type":"style"}"#), "{text}");
        assert!(text.contains(r#""type":"remove""#) && text.contains(r#""remove-attr""#));

        let many: Vec<String> = (0..MAX_FILTERS + 10).map(|i| format!(".a{i}:has-text(x)")).collect();
        let out = process(many.iter().map(String::as_str), []);
        assert_eq!(out.count as usize, MAX_FILTERS);
        assert_eq!(out.dropped, 10);
    }
}
