// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What Skia's Fontations backend (Chromium 156) reports for MoltNet's
// bundled fonts, computed with the skrifa version Chromium vendors:
// SkFontMetrics as SkTypeface_fontations.cpp's generateFontMetrics() fills
// it (unhinted, the size as the scale), and, per sample glyph, the unhinted
// advance and the bounds of its path's points (SkPath::getBounds(), y down),
// which getBounds() rounds out for an outline glyph.
//
//   skrifa_dump DIR FILE... > cases.txt
//
// One line per case: whitespace-separated fields, see `emit`.

use skrifa::instance::{LocationRef, Size};
use skrifa::outline::{DrawSettings, OutlinePen};
use skrifa::prelude::*;
use skrifa::raw::TableProvider;
use std::fs;

const SIZES: [f32; 9] = [9.0, 12.0, 13.0, 16.0, 17.5, 21.333334, 32.0, 48.0, 100.0];
const SAMPLE: &str = "xHgjyAW0\u{e9}";

#[derive(Default)]
struct Bounds {
    any: bool,
    l: f32,
    t: f32,
    r: f32,
    b: f32,
}

impl Bounds {
    fn add(&mut self, x: f32, y: f32) {
        // Skia flips y: path coordinates grow downwards.
        let y = -y;
        if !self.any {
            *self = Bounds { any: true, l: x, t: y, r: x, b: y };
        } else {
            self.l = self.l.min(x);
            self.r = self.r.max(x);
            self.t = self.t.min(y);
            self.b = self.b.max(y);
        }
    }
}

impl OutlinePen for Bounds {
    fn move_to(&mut self, x: f32, y: f32) {
        self.add(x, y);
    }
    fn line_to(&mut self, x: f32, y: f32) {
        self.add(x, y);
    }
    fn quad_to(&mut self, cx0: f32, cy0: f32, x: f32, y: f32) {
        self.add(cx0, cy0);
        self.add(x, y);
    }
    fn curve_to(&mut self, cx0: f32, cy0: f32, cx1: f32, cy1: f32, x: f32, y: f32) {
        self.add(cx0, cy0);
        self.add(cx1, cy1);
        self.add(x, y);
    }
    fn close(&mut self) {}
}

fn path_bounds(font: &FontRef, gid: GlyphId, size: f32, loc: LocationRef) -> Option<Bounds> {
    let glyph = font.outline_glyphs().get(gid)?;
    let mut pen = Bounds::default();
    glyph
        .draw(DrawSettings::unhinted(Size::new(size), loc), &mut pen)
        .ok()?;
    if pen.any {
        Some(pen)
    } else {
        None
    }
}

fn contour_height(font: &FontRef, ch: char, size: f32, loc: LocationRef) -> Option<f32> {
    let gid = font.charmap().map(ch)?;
    if gid.to_u32() == 0 {
        return None;
    }
    path_bounds(font, gid, size, loc).map(|b| b.b - b.t)
}

fn emit(name: &str, index: u32, weight: f32, font: &FontRef, loc: LocationRef) {
    let has_fvar = font.fvar().is_ok();
    let mut has_trak = false;
    let mut has_sbix = false;
    for record in font.table_directory.table_records() {
        let tag = record.tag();
        has_trak |= tag == Tag::new(b"trak");
        has_sbix |= tag == Tag::new(b"sbix");
    }
    for size in SIZES {
        let m = font.metrics(Size::new(size), loc);
        // generateFontMetrics(): signs as SkFontMetrics has them.
        let (top, bottom, xmin, xmax) = m
            .bounds
            .map_or((0.0, 0.0, 0.0, 0.0), |b| (-b.y_max, -b.y_min, b.x_min, b.x_max));
        let mut x_height = m.x_height.unwrap_or(0.0);
        if x_height == 0.0 {
            x_height = contour_height(font, 'x', size, loc).unwrap_or(m.ascent);
        }
        let mut cap_height = m.cap_height.unwrap_or(0.0);
        if cap_height == 0.0 {
            cap_height = contour_height(font, 'H', size, loc).unwrap_or(m.ascent);
        }
        let (under_pos, under_valid) = m.underline.map_or((0.0, 0), |u| (-u.offset, 1));
        let (under_thick, thick_valid) = m.underline.map_or((0.0, 0), |u| (u.thickness, 1));
        print!(
            "case {} {} {} {} fvar={} trak={} sbix={} m {} {} {} {} {} {} {} {} {} {} {} {} {} {} {}",
            name,
            index,
            weight,
            size,
            has_fvar as u8,
            has_trak as u8,
            has_sbix as u8,
            top,
            -m.ascent,
            -m.descent,
            bottom,
            m.leading,
            m.average_width.unwrap_or(0.0),
            m.max_width.unwrap_or(0.0),
            xmin,
            xmax,
            x_height,
            cap_height,
            under_pos,
            under_valid,
            under_thick,
            thick_valid
        );
        let gm = font.glyph_metrics(Size::new(size), loc);
        for ch in SAMPLE.chars() {
            let gid = font.charmap().map(ch).map_or(0, |g| g.to_u32());
            let advance = if gid != 0 {
                gm.advance_width(GlyphId::new(gid)).unwrap_or(0.0)
            } else {
                0.0
            };
            let pb = if gid != 0 {
                path_bounds(font, GlyphId::new(gid), size, loc)
            } else {
                None
            };
            match pb {
                Some(b) => print!(" g {} {} {} 1 {} {} {} {}", ch as u32, gid, advance, b.l, b.t, b.r, b.b),
                None => print!(" g {} {} {} 0 0 0 0 0", ch as u32, gid, advance),
            }
        }
        println!();
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let dir = &args[1];
    for name in &args[2..] {
        let data = fs::read(format!("{}/{}", dir, name)).expect("read");
        let count = match skrifa::raw::FileRef::new(&data).expect("font") {
            skrifa::raw::FileRef::Font(_) => 1,
            skrifa::raw::FileRef::Collection(c) => c.len(),
        };
        for index in 0..count {
            let font = FontRef::from_index(&data, index).expect("face");
            let axes = font.axes();
            let wght = axes.iter().find(|a| a.tag() == Tag::new(b"wght"));
            let mut weights: Vec<f32> = Vec::new();
            match wght {
                Some(a) => {
                    for w in [a.default_value(), 400.0, 700.0] {
                        if w >= a.min_value() && w <= a.max_value() && !weights.contains(&w) {
                            weights.push(w);
                        }
                    }
                }
                None => weights.push(0.0),
            }
            for w in weights {
                let location = if w > 0.0 {
                    axes.location([("wght", w)])
                } else {
                    axes.location(std::iter::empty::<(&str, f32)>())
                };
                emit(name, index, w, &font, (&location).into());
            }
        }
    }
}
