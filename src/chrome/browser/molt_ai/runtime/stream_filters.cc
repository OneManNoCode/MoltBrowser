// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/stream_filters.h"

#include <algorithm>

namespace molt_ai {

namespace {

bool IsSpace(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

// Largest k in [1, tag.size()-1] such that |s| ends with tag[0, k).
size_t PartialTagSuffix(std::string_view s, std::string_view tag) {
  const size_t max_k = std::min(s.size(), tag.size() - 1);
  for (size_t k = max_k; k > 0; --k) {
    if (s.substr(s.size() - k) == tag.substr(0, k)) return k;
  }
  return 0;
}

}  // namespace

// ---- HarmonyChannelFilter -------------------------------------------------

std::string HarmonyChannelFilter::Feed(const std::string& piece) {
  // Harmony control tokens arrive as whole special-token pieces.
  bool is_ctrl = true;
  if (piece == "<|channel|>") {
    harmony_ = true;
    state_ = kChannelName;
    channel_.clear();
    emit_ = false;
  } else if (piece == "<|message|>") {
    if (harmony_) {
      state_ = kBody;
      emit_ = channel_.find("final") != std::string::npos;
    } else {
      is_ctrl = false;
    }
  } else if (piece == "<|start|>" || piece == "<|end|>" ||
             piece == "<|return|>" || piece == "<|constrain|>" ||
             piece == "<|call|>") {
    if (harmony_) {
      state_ = kOutside;  // message boundary; next channel decides
      emit_ = false;
    } else {
      is_ctrl = false;
    }
  } else {
    is_ctrl = false;
  }

  if (is_ctrl) return std::string();
  if (!harmony_) return piece;  // normal model — pass everything through
  if (state_ == kChannelName) {
    channel_ += piece;  // building the channel name; don't surface
    return std::string();
  }
  if (state_ == kBody && emit_) return piece;  // final channel body
  // analysis/commentary body or a role name between messages — drop
  return std::string();
}

// ---- ThinkTagFilter -------------------------------------------------------

ThinkTagFilter::ThinkTagFilter(bool start_inside)
    : in_think_(start_inside), saw_think_(start_inside) {}

std::string ThinkTagFilter::EmitVisible(std::string_view text) {
  if (strip_leading_ws_) {
    size_t i = 0;
    while (i < text.size() && IsSpace(text[i])) ++i;
    text.remove_prefix(i);
    if (!text.empty()) strip_leading_ws_ = false;
  }
  if (!visible_started_) {
    for (char c : text) {
      if (!IsSpace(c)) {
        visible_started_ = true;
        break;
      }
    }
  }
  return std::string(text);
}

std::string ThinkTagFilter::Feed(std::string_view piece) {
  std::string out;
  pending_.append(piece);
  const std::string_view open_tag(kOpenTag);
  const std::string_view close_tag(kCloseTag);
  while (!pending_.empty()) {
    if (in_think_) {
      const size_t pos = pending_.find(close_tag);
      if (pos != std::string::npos) {
        pending_.erase(0, pos + close_tag.size());
        in_think_ = false;
        strip_leading_ws_ = true;
        continue;
      }
      // Hidden: drop everything except a possible partial "</think>".
      const size_t hold = PartialTagSuffix(pending_, close_tag);
      pending_.erase(0, pending_.size() - hold);
      break;
    }

    // Tags only act as reasoning delimiters before the answer has started;
    // a model that later writes "<think>" in prose or code just shows it.
    if (!visible_started_) {
      const size_t open_pos = pending_.find(open_tag);
      const size_t close_pos = pending_.find(close_tag);
      const size_t first = std::min(open_pos, close_pos);
      if (first != std::string::npos) {
        std::string before = EmitVisible(std::string_view(pending_).substr(
            0, first));
        if (visible_started_) {
          // Real text preceded the tag: from here on tags are plain text.
          out += before;
          pending_.erase(0, first);
          continue;
        }
        out += before;  // whitespace only
        if (first == open_pos) {
          in_think_ = true;
          saw_think_ = true;
          pending_.erase(0, first + open_tag.size());
        } else {
          // Orphan "</think>" before any answer text: the block was opened
          // by the prompt. Drop the tag and the whitespace after it.
          pending_.erase(0, first + close_tag.size());
          strip_leading_ws_ = true;
        }
        continue;
      }
      const size_t hold = std::max(PartialTagSuffix(pending_, open_tag),
                                   PartialTagSuffix(pending_, close_tag));
      out += EmitVisible(
          std::string_view(pending_).substr(0, pending_.size() - hold));
      pending_.erase(0, pending_.size() - hold);
      // If visible text just started, the held-back bytes can no longer be
      // a reasoning tag; loop once more to release them as text.
      if (visible_started_ && hold > 0) continue;
      break;
    }

    out += EmitVisible(pending_);
    pending_.clear();
  }
  return out;
}

std::string ThinkTagFilter::Flush() {
  if (in_think_) {
    pending_.clear();
    return std::string();
  }
  std::string out = EmitVisible(pending_);
  pending_.clear();
  return out;
}

// ---- UTF-8 ----------------------------------------------------------------

size_t Utf8SafePrefixLength(std::string_view s) {
  if (s.empty()) return 0;
  size_t i = s.size();
  while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) {
    --i;  // skip UTF-8 continuation bytes
  }
  if (i == 0) return s.size();  // only continuation bytes: nothing to wait for
  const unsigned char lead = static_cast<unsigned char>(s[i - 1]);
  const size_t need = (lead < 0x80)             ? 1
                      : ((lead & 0xE0) == 0xC0) ? 2
                      : ((lead & 0xF0) == 0xE0) ? 3
                      : ((lead & 0xF8) == 0xF0) ? 4
                                                : 1;
  const size_t have = s.size() - (i - 1);
  return have >= need ? s.size() : (i - 1);  // hold back an incomplete char
}

std::string Utf8Assembler::Push(std::string_view bytes) {
  pending_.append(bytes);
  const size_t safe = Utf8SafePrefixLength(pending_);
  std::string out = pending_.substr(0, safe);
  pending_.erase(0, safe);
  return out;
}

std::string Utf8Assembler::Flush() {
  std::string out;
  out.swap(pending_);
  return out;
}

}  // namespace molt_ai
