//! Word binary list tables: `PlfLst` (list definitions with per-level
//! formats) and `PlfLfo` (list format overrides referenced by `sprmPIlfo`),
//! resolved per [MS-DOC]: each list keeps its identity (`lsid`) so numbering
//! state is shared by every LFO referencing the same list, levels keep their
//! restart limits (`fNoRestart`/`ilvlRestartLim`), LFOLVL start-at overrides
//! apply when their LFO is first used, and the number text (`xst` with
//! `rgbxchNums` placeholders) is preserved for composite markers.

use crate::shared::binary::{get_u16, get_u32};
use crate::shared::list::MarkerKind;
use crate::shared::numbering::{NumberPattern, NumberText};
use std::collections::HashMap;

pub const LEVELS: usize = 9;

#[derive(Debug, Clone)]
pub struct LevelDef {
    pub marker: Option<MarkerKind>,
    pub start: u64,
    /// Restart rule, as in WordprocessingML `lvlRestart`: `None` = restart
    /// after any more significant level (`fNoRestart` clear), `Some(0)` =
    /// never restart, `Some(n)` = restart when a level with ilvl < n is used
    /// (`fNoRestart` set with `ilvlRestartLim`).
    pub restart: Option<u32>,
    /// The level's number text (`xst` with `rgbxchNums` placeholders and
    /// LVLF `fLegal`); empty for bullets and malformed levels.
    pub pattern: NumberPattern,
}

impl Default for LevelDef {
    fn default() -> Self {
        LevelDef {
            marker: Some(MarkerKind::Bullet),
            start: 1,
            restart: None,
            pattern: NumberPattern::default(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct ListDef {
    /// List identity: numbering state is keyed by this, not the ilfo, so
    /// every override referencing the same list continues its sequence.
    pub lsid: u32,
    pub levels: [LevelDef; LEVELS],
    /// LFOLVL start-at overrides; each restarts the shared sequence at its
    /// value when this LFO is first used at that level.
    pub start_override: [Option<u64>; LEVELS],
}

impl ListDef {
    /// Fallback definition for an ilfo with no parsed list behind it; the
    /// synthetic identity keeps it distinct from every real `lsid`.
    pub fn unknown(ilfo: u16) -> ListDef {
        ListDef {
            lsid: u32::MAX ^ u32::from(ilfo),
            levels: std::array::from_fn(|_| LevelDef::default()),
            start_override: [None; LEVELS],
        }
    }
}

#[derive(Debug, Default)]
pub struct Lists {
    /// 1-based ilfo -> resolved definition.
    by_ilfo: HashMap<u16, ListDef>,
}

impl Lists {
    pub fn get(&self, ilfo: u16) -> Option<&ListDef> {
        self.by_ilfo.get(&ilfo)
    }
}

/// FIB offsets (Word 97+): fcPlfLst/lcbPlfLst at 0x2E2, fcPlfLfo at 0x2EA.
pub fn parse(word_doc: &[u8], table: &[u8]) -> Lists {
    let lst_fc = get_u32(word_doc, 0x2E2).unwrap_or(0) as usize;
    let lst_lcb = get_u32(word_doc, 0x2E6).unwrap_or(0) as usize;
    let lfo_fc = get_u32(word_doc, 0x2EA).unwrap_or(0) as usize;
    let lfo_lcb = get_u32(word_doc, 0x2EE).unwrap_or(0) as usize;

    let mut lists = Lists::default();
    if lst_lcb == 0 {
        return lists;
    }
    // lcbPlfLst covers only the LSTF array; each list's LVL structures
    // follow it in the table stream, uncounted.
    let by_lsid = parse_plf_lst(table, lst_fc, lst_lcb);
    let Some(plf_lfo) = table.get(lfo_fc..lfo_fc.saturating_add(lfo_lcb)) else {
        return lists;
    };
    parse_plf_lfo(plf_lfo, &by_lsid, &mut lists);
    lists
}

const LSTF_SIZE: usize = 28;

fn parse_plf_lst(table: &[u8], fc: usize, lcb: usize) -> HashMap<u32, [LevelDef; LEVELS]> {
    let mut out = HashMap::new();
    let Some(count) = get_u16(table, fc).map(|v| v as usize) else {
        return out;
    };
    let mut lstfs: Vec<(u32, bool)> = Vec::with_capacity(count);
    let mut pos = fc + 2;
    for _ in 0..count {
        let Some(lsid) = get_u32(table, pos) else {
            return out;
        };
        let simple = table.get(pos + 26).is_some_and(|&flags| flags & 0x01 != 0);
        lstfs.push((lsid, simple));
        pos += LSTF_SIZE;
    }
    // The LVL array begins where lcbPlfLst ends.
    let mut pos = fc + lcb;
    for (lsid, simple) in lstfs {
        let n_levels = if simple { 1 } else { LEVELS };
        let mut levels: [LevelDef; LEVELS] = std::array::from_fn(|_| LevelDef::default());
        for slot in levels.iter_mut().take(n_levels) {
            match parse_lvl(table, pos) {
                Some((def, next)) => {
                    *slot = def;
                    pos = next;
                }
                None => return out,
            }
        }
        if simple {
            // A simple list applies its single level everywhere.
            let def = levels[0].clone();
            levels = std::array::from_fn(|_| def.clone());
        }
        out.insert(lsid, levels);
    }
    out
}

/// One LVL: an LVLF header, then PAPX and CHPX grpprls, then the number text.
fn parse_lvl(plf: &[u8], pos: usize) -> Option<(LevelDef, usize)> {
    const LVLF_SIZE: usize = 28;
    let start = get_u32(plf, pos)? as u64;
    let nfc = *plf.get(pos + 4)?;
    let flags = *plf.get(pos + 5)?;
    let legal = flags & 0x04 != 0;
    let no_restart = flags & 0x08 != 0;
    // rgbxchNums: one-based offsets of number placeholders within xst,
    // zero-terminated unless full.
    let placeholder_offsets: Vec<u8> =
        plf.get(pos + 6..pos + 15)?.iter().copied().take_while(|&b| b != 0).collect();
    let restart_lim = *plf.get(pos + 26)?;
    let cb_papx = *plf.get(pos + 25)? as usize;
    let cb_chpx = *plf.get(pos + 24)? as usize;
    let mut next = pos + LVLF_SIZE + cb_papx + cb_chpx;
    let xch_count = get_u16(plf, next)? as usize;
    next += 2;
    let marker = marker_for_nfc(nfc);
    let mut text: Vec<NumberText> = Vec::new();
    if marker.is_some_and(MarkerKind::ordered) {
        for i in 0..xch_count {
            let ch = get_u16(plf, next + i * 2)?;
            if ch <= 0x08 && placeholder_offsets.contains(&((i + 1) as u8)) {
                text.push(NumberText::Level(ch as u8));
            } else if let Some(c) = char::from_u32(ch as u32).filter(|c| !c.is_control()) {
                match text.last_mut() {
                    Some(NumberText::Literal(s)) => s.push(c),
                    _ => text.push(NumberText::Literal(c.to_string())),
                }
            }
        }
    }
    next += xch_count * 2;
    let restart = no_restart.then_some(restart_lim as u32);
    Some((LevelDef { marker, start, restart, pattern: NumberPattern { text, legal } }, next))
}

const LFO_SIZE: usize = 16;

fn parse_plf_lfo(plf: &[u8], by_lsid: &HashMap<u32, [LevelDef; LEVELS]>, lists: &mut Lists) {
    let Some(count) = get_u32(plf, 0).map(|v| v as usize) else {
        return;
    };
    let mut lfos: Vec<(u32, usize)> = Vec::with_capacity(count);
    let mut pos = 4;
    for _ in 0..count {
        let Some(lsid) = get_u32(plf, pos) else {
            return;
        };
        let clfolvl = plf.get(pos + 12).copied().unwrap_or(0) as usize;
        lfos.push((lsid, clfolvl));
        pos += LFO_SIZE;
    }
    for (i, (lsid, clfolvl)) in lfos.into_iter().enumerate() {
        let ilfo = (i + 1) as u16;
        let mut def = match by_lsid.get(&lsid) {
            Some(levels) => {
                ListDef { lsid, levels: levels.clone(), start_override: [None; LEVELS] }
            }
            None => ListDef::unknown(ilfo),
        };
        for _ in 0..clfolvl {
            let Some(start_at) = get_u32(plf, pos) else {
                break;
            };
            let Some(&bits) = plf.get(pos + 4) else {
                break;
            };
            let ilvl = (bits & 0x0F) as usize;
            let f_start_at = bits & 0x10 != 0;
            let f_formatting = bits & 0x20 != 0;
            pos += 8;
            if f_formatting {
                // The override embeds a full LVL; its own start wins.
                match parse_lvl(plf, pos) {
                    Some((lvl_def, next)) => {
                        if ilvl < LEVELS {
                            if f_start_at {
                                def.start_override[ilvl] = Some(lvl_def.start);
                            }
                            def.levels[ilvl] = lvl_def;
                        }
                        pos = next;
                    }
                    None => break,
                }
            } else if f_start_at && ilvl < LEVELS {
                def.levels[ilvl].start = start_at as u64;
                def.start_override[ilvl] = Some(start_at as u64);
            }
        }
        lists.by_ilfo.insert(ilfo, def);
    }
}

/// MS-OSHARED numbering format codes.
fn marker_for_nfc(nfc: u8) -> Option<MarkerKind> {
    match nfc {
        0 => Some(MarkerKind::Decimal),
        1 => Some(MarkerKind::UpperRoman),
        2 => Some(MarkerKind::LowerRoman),
        3 => Some(MarkerKind::UpperAlpha),
        4 => Some(MarkerKind::LowerAlpha),
        23 => Some(MarkerKind::Bullet),
        0xFF => None,
        _ => Some(MarkerKind::Decimal),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A hand-built LVL: LVLF header + xst "\x00-\x01)" with placeholders
    /// at one-based offsets 1 and 3.
    #[test]
    fn lvl_parses_the_complete_metadata() {
        let mut lvl = vec![0u8; 28];
        lvl[0..4].copy_from_slice(&5u32.to_le_bytes()); // iStartAt
        lvl[4] = 4; // nfc: lowerLetter
        lvl[5] = 0x08; // fNoRestart
        lvl[6] = 1; // rgbxchNums: placeholder offsets
        lvl[7] = 3;
        lvl[26] = 1; // ilvlRestartLim
        lvl.extend_from_slice(&4u16.to_le_bytes()); // xst length
        for ch in [0x0000u16, '-' as u16, 0x0001, ')' as u16] {
            lvl.extend_from_slice(&ch.to_le_bytes());
        }
        let (def, next) = parse_lvl(&lvl, 0).unwrap();
        assert_eq!(next, lvl.len());
        assert_eq!(def.start, 5);
        assert_eq!(def.marker, Some(MarkerKind::LowerAlpha));
        assert_eq!(def.restart, Some(1));
        assert!(!def.pattern.legal);
        assert_eq!(
            def.pattern.text,
            vec![
                NumberText::Level(0),
                NumberText::Literal("-".into()),
                NumberText::Level(1),
                NumberText::Literal(")".into()),
            ]
        );
    }

    #[test]
    fn default_restart_when_f_no_restart_is_clear() {
        let mut lvl = vec![0u8; 28];
        lvl[0..4].copy_from_slice(&1u32.to_le_bytes());
        lvl[4] = 0; // decimal
        lvl[26] = 3; // ilvlRestartLim is ignored without fNoRestart
        lvl.extend_from_slice(&0u16.to_le_bytes());
        let (def, _) = parse_lvl(&lvl, 0).unwrap();
        assert_eq!(def.restart, None);
    }
}
