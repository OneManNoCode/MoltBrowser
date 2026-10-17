// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// FFI bridge exposing the on-device `anydoc` crate (document -> clean
// GitHub-Flavored Markdown) to MoltBrowser's C++ attachment-extraction path.
// 100% local: no network, no cloud, no ML models. Turns user-attached
// Office/PDF/e-book files into structured Markdown the on-device LLM grounds on.
//
// Parsing runs in the browser process, at parity with the existing native
// pdf/docx scrapers this augments. anydoc is memory-safe Rust and fuzz-tested,
// so it is more robust than those hand-rolled scrapers; but Chromium builds
// Rust with panic=immediate-abort, so a parser *panic* on a malformed file
// cannot be caught here and would abort the process. Moving document parsing
// into a sandboxed utility process is the planned hardening (it would also
// sandbox the existing scrapers).

#[cxx::bridge(namespace = "molt_ai::anydoc_ffi")]
mod ffi {
    // Outcome of a conversion. `ok` separates success from a *handled* failure
    // (unsupported format, corrupt archive, image-only PDF, ...); on failure
    // `markdown` is empty and `error` holds a short diagnostic string.
    struct ConvertResult {
        ok: bool,
        markdown: String,
        error: String,
    }

    extern "Rust" {
        // Convert an in-memory document to GitHub-Flavored Markdown. `ext` is a
        // lowercase filename extension WITHOUT the dot (e.g. "docx"); it is used
        // only as a fallback, since the format is detected from the bytes first.
        // `ext` may be empty.
        fn convert_to_markdown(bytes: &[u8], ext: &str) -> ConvertResult;
    }
}

fn convert_to_markdown(bytes: &[u8], ext: &str) -> ffi::ConvertResult {
    // Content sniff first (robust to a wrong/missing extension), then fall back
    // to the extension hint.
    let format = anydoc::Format::from_bytes(bytes).or_else(|| {
        if ext.is_empty() {
            None
        } else {
            anydoc::Format::from_extension(ext)
        }
    });
    match anydoc::to_markdown_bytes(bytes, format) {
        Ok(markdown) => ffi::ConvertResult {
            ok: true,
            markdown,
            error: String::new(),
        },
        Err(error) => ffi::ConvertResult {
            ok: false,
            markdown: String::new(),
            error: format!("{error:?}"),
        },
    }
}
