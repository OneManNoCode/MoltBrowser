// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/views/molt_ai/molt_exit_country_bubble.h"

#include <map>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/molt_ai/tor/tor_service.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/style/typography.h"
#include "ui/views/widget/widget.h"

namespace molt_ai {

namespace {

// Display names for the curated exit-country codes. Duplicated from
// MoltAIChatHandler's ExitCountryDisplayName (molt_ai_chat_handler_tor.cc)
// so the Views layer doesn't depend on the WebUI layer. Keep in sync with
// TorManager::GetAvailableExitCountries(); any code not found here is
// rendered as its uppercased ISO code (e.g. "xx" -> "XX").
std::string ExitCountryDisplayName(const std::string& cc) {
  static const auto* const kNames =
      new std::map<std::string, std::string>{
          {"us", "United States"}, {"gb", "United Kingdom"},
          {"de", "Germany"},       {"fr", "France"},
          {"nl", "Netherlands"},   {"ch", "Switzerland"},
          {"se", "Sweden"},        {"no", "Norway"},
          {"fi", "Finland"},       {"ca", "Canada"},
          {"jp", "Japan"},         {"sg", "Singapore"},
          {"au", "Australia"},     {"es", "Spain"},
          {"it", "Italy"},         {"at", "Austria"},
          {"pl", "Poland"},        {"cz", "Czechia"},
          {"ro", "Romania"},       {"is", "Iceland"},
      };
  auto it = kNames->find(cc);
  if (it != kNames->end())
    return it->second;
  return base::ToUpperASCII(cc);
}

// Row text for a given code ("" == any). A leading checkmark marks the
// active selection; non-selected rows are indented two spaces so the
// labels line up. The checkmark glyph (U+2713) renders reliably in Views,
// unlike flag emoji which we deliberately avoid.
std::u16string RowText(const std::string& cc, bool selected) {
  const std::string name =
      cc.empty() ? std::string("Any country (let Tor choose)")
                 : base::StrCat({ExitCountryDisplayName(cc), " (",
                                 base::ToUpperASCII(cc), ")"});
  const std::u16string prefix = selected ? u"✓ " : u"  ";
  return prefix + base::UTF8ToUTF16(name);
}

}  // namespace

MoltExitCountryBubble::MoltExitCountryBubble(views::View* anchor,
                                             Browser* browser,
                                             OnChangedCallback on_changed)
    : BubbleDialogDelegateView(views::BubbleAnchor(anchor),
                               views::BubbleBorder::TOP_RIGHT,
                               views::BubbleBorder::DIALOG_SHADOW,
                               /*autosize=*/false),
      browser_(browser),
      on_changed_(std::move(on_changed)) {
  // No OK/Cancel — selection is applied immediately on row click.
  SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
  SetShowCloseButton(true);
  SetTitle(u"Tor exit country");

  // Read the current selection synchronously on the UI thread.
  selected_country_ = molt_ai::tor::TorManager::Get()->GetExitCountry();
  country_codes_ =
      molt_ai::tor::TorManager::Get()->GetAvailableExitCountries();

  BuildContents();
}

MoltExitCountryBubble::~MoltExitCountryBubble() = default;

// static
void MoltExitCountryBubble::Show(views::View* anchor,
                                 Browser* browser,
                                 OnChangedCallback on_changed) {
  if (!anchor || !browser)
    return;
  auto* bubble =
      new MoltExitCountryBubble(anchor, browser, std::move(on_changed));
  views::BubbleDialogDelegateView::CreateBubble(bubble)->Show();
}

void MoltExitCountryBubble::BuildContents() {
  auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(8)));
  layout->set_between_child_spacing(2);
  set_fixed_width(320);

  // Build the full list of codes including the leading "" (any) sentinel
  // so row_codes_ / row_labels_ stay parallel and index-aligned.
  std::vector<std::string> all_codes;
  all_codes.reserve(country_codes_.size() + 1);
  all_codes.push_back(std::string());  // "Any country"
  for (const auto& cc : country_codes_)
    all_codes.push_back(cc);

  row_codes_.clear();
  row_buttons_.clear();
  row_codes_.reserve(all_codes.size());
  row_buttons_.reserve(all_codes.size());

  for (const auto& cc : all_codes) {
    const bool selected = (cc == selected_country_);
    auto button = std::make_unique<views::LabelButton>(
        base::BindRepeating(&MoltExitCountryBubble::OnCountryChosen,
                            weak_factory_.GetWeakPtr(), cc),
        RowText(cc, selected));
    button->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    views::LabelButton* button_ptr = button.get();
    row_codes_.push_back(cc);
    row_buttons_.push_back(button_ptr);
    AddChildView(std::move(button));
  }

  // Apparent-IP readout. Starts in a resolving state; filled by
  // OnCircuits() once GetCircuitsEnriched() replies.
  auto ip = std::make_unique<views::Label>(
      u"Apparent IP: resolving…", views::style::CONTEXT_LABEL,
      views::style::STYLE_SECONDARY);
  ip->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  ip->SetSelectable(true);
  ip_label_ = AddChildView(std::move(ip));

  // Inline note describing the effect of a selection (hidden until the
  // user picks a country).
  auto note = std::make_unique<views::Label>(
      std::u16string(), views::style::CONTEXT_LABEL,
      views::style::STYLE_SECONDARY);
  note->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  note->SetMultiLine(true);
  note->SetVisible(false);
  note_label_ = AddChildView(std::move(note));

  RefreshApparentIp();
}

void MoltExitCountryBubble::OnCountryChosen(const std::string& country_code) {
  // Apply synchronously (works whether or not Tor is currently running —
  // TorManager rewrites the torrc and reloads in place if it is).
  molt_ai::tor::TorManager::Get()->SetExitCountry(country_code);
  selected_country_ = country_code;

  UpdateSelectionHighlight();

  if (note_label_) {
    const std::string name =
        country_code.empty()
            ? std::string("any country")
            : ExitCountryDisplayName(country_code);
    note_label_->SetText(base::UTF8ToUTF16(base::StrCat(
        {"Routing through ", name,
         "; new circuits will use this exit."})));
    note_label_->SetVisible(true);
  }

  // Let the toolbar button refresh its label to match the new selection.
  if (on_changed_)
    on_changed_.Run();

  // The exit IP changes only after Tor rebuilds circuits through the new
  // exit, so show "resolving" now and re-query after a short delay.
  if (ip_label_)
    ip_label_->SetText(u"Apparent IP: resolving…");
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&MoltExitCountryBubble::RefreshApparentIp,
                     weak_factory_.GetWeakPtr()),
      base::Seconds(3));
}

void MoltExitCountryBubble::UpdateSelectionHighlight() {
  for (size_t i = 0; i < row_buttons_.size(); ++i) {
    if (!row_buttons_[i])
      continue;
    const bool selected = (row_codes_[i] == selected_country_);
    row_buttons_[i]->SetText(RowText(row_codes_[i], selected));
  }
}

void MoltExitCountryBubble::RefreshApparentIp() {
  molt_ai::tor::TorService::Get()->GetCircuitsEnriched(base::BindOnce(
      &MoltExitCountryBubble::OnCircuits, weak_factory_.GetWeakPtr()));
}

void MoltExitCountryBubble::OnCircuits(
    std::vector<molt_ai::tor::TorCircuit> circuits) {
  if (!ip_label_)
    return;

  // Pick the active circuit: the first BUILT one with hops, preferring a
  // GENERAL-purpose circuit. The apparent exit IP is its last hop's IP.
  // (Extraction logic mirrors EmitMoltnetStatusFromCircuits in
  // molt_ai_settings_ui.cc.)
  const molt_ai::tor::TorCircuit* active = nullptr;
  for (const auto& c : circuits) {
    if (c.state == "BUILT" && !c.hops.empty()) {
      active = &c;
      if (c.purpose == "GENERAL")
        break;
    }
  }

  if (active) {
    const std::string apparent_ip = active->hops.back().ip;
    if (apparent_ip.empty()) {
      ip_label_->SetText(u"Apparent IP: — (resolving exit relay)");
    } else {
      ip_label_->SetText(
          base::UTF8ToUTF16(base::StrCat({"Apparent IP: ", apparent_ip})));
    }
  } else {
    ip_label_->SetText(u"Apparent IP: — (Tor not running)");
  }
}

BEGIN_METADATA(MoltExitCountryBubble)
END_METADATA

}  // namespace molt_ai
