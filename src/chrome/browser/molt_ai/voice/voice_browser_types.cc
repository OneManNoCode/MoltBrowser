// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_browser_types.h"

namespace molt_ai {
namespace voice {

BrowserItem::BrowserItem() = default;
BrowserItem::~BrowserItem() = default;
BrowserItem::BrowserItem(const BrowserItem&) = default;
BrowserItem& BrowserItem::operator=(const BrowserItem&) = default;
BrowserItem::BrowserItem(BrowserItem&&) = default;
BrowserItem& BrowserItem::operator=(BrowserItem&&) = default;

BrowserEnvironment::BrowserEnvironment() = default;
BrowserEnvironment::~BrowserEnvironment() = default;
BrowserEnvironment::BrowserEnvironment(const BrowserEnvironment&) = default;
BrowserEnvironment& BrowserEnvironment::operator=(const BrowserEnvironment&) =
    default;
BrowserEnvironment::BrowserEnvironment(BrowserEnvironment&&) = default;
BrowserEnvironment& BrowserEnvironment::operator=(BrowserEnvironment&&) =
    default;

FindMatch::FindMatch() = default;
FindMatch::~FindMatch() = default;
FindMatch::FindMatch(const FindMatch&) = default;
FindMatch& FindMatch::operator=(const FindMatch&) = default;
FindMatch::FindMatch(FindMatch&&) = default;
FindMatch& FindMatch::operator=(FindMatch&&) = default;

BrowserConversation::BrowserConversation() = default;
BrowserConversation::~BrowserConversation() = default;
BrowserConversation::BrowserConversation(const BrowserConversation&) = default;
BrowserConversation& BrowserConversation::operator=(
    const BrowserConversation&) = default;
BrowserConversation::BrowserConversation(BrowserConversation&&) = default;
BrowserConversation& BrowserConversation::operator=(BrowserConversation&&) =
    default;

}  // namespace voice
}  // namespace molt_ai
