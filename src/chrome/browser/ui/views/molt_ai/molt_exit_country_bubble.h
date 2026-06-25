// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltExitCountryBubble — a toolbar bubble that lets the user pick which
// country Tor uses for its exit relay and shows the apparent exit IP
// (the IP of the last hop of the active BUILT circuit).
//
// Threading: TorManager getters/setters are synchronous on the UI thread
// and are called inline. The apparent IP comes from
// TorService::GetCircuitsEnriched(), which is asynchronous; its reply
// runs on the UI thread and is guarded by a WeakPtr so a dismissed
// bubble never touches freed memory.

#ifndef CHROME_BROWSER_UI_VIEWS_MOLT_AI_MOLT_EXIT_COUNTRY_BUBBLE_H_
#define CHROME_BROWSER_UI_VIEWS_MOLT_AI_MOLT_EXIT_COUNTRY_BUBBLE_H_

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/molt_ai/tor/tor_service.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"

class Browser;

namespace views {
class Label;
class LabelButton;
class View;
}  // namespace views

namespace molt_ai {

// A vertical list of selectable exit-country rows plus an "Apparent IP"
// readout. Anchored to the toolbar globe button.
class MoltExitCountryBubble : public views::BubbleDialogDelegateView {
  METADATA_HEADER(MoltExitCountryBubble, views::BubbleDialogDelegateView)

 public:
  // Invoked (on the UI thread) after the user picks a new exit country so
  // the anchor toolbar button can refresh its label.
  using OnChangedCallback = base::RepeatingClosure;

  MoltExitCountryBubble(views::View* anchor,
                        Browser* browser,
                        OnChangedCallback on_changed);
  MoltExitCountryBubble(const MoltExitCountryBubble&) = delete;
  MoltExitCountryBubble& operator=(const MoltExitCountryBubble&) = delete;
  ~MoltExitCountryBubble() override;

  // Creates, anchors, and shows the bubble. |on_changed| (may be empty) is
  // run whenever the selection changes so the caller can refresh its UI.
  static void Show(views::View* anchor,
                   Browser* browser,
                   OnChangedCallback on_changed);

 private:
  // Builds the row list + IP label into the bubble's contents.
  void BuildContents();

  // Handler for a row click: applies the new exit country, refreshes the
  // selection highlight + note, fires |on_changed_|, and re-queries the IP.
  void OnCountryChosen(const std::string& country_code);

  // Repaints the checkmark/selection state on every row to match
  // |selected_country_|.
  void UpdateSelectionHighlight();

  // Kicks off an async IP refresh (GetCircuitsEnriched), WeakPtr-guarded.
  void RefreshApparentIp();

  // Reply from TorService::GetCircuitsEnriched. Extracts the exit IP of
  // the active BUILT circuit and writes it into |ip_label_|.
  void OnCircuits(std::vector<molt_ai::tor::TorCircuit> circuits);

  const raw_ptr<Browser> browser_;
  OnChangedCallback on_changed_;

  // Lowercase ISO alpha-2 of the current selection ("" == any country).
  std::string selected_country_;

  // Codes rendered as rows, in display order ("" sentinel is handled
  // separately as the first "Any country" row, not stored here).
  std::vector<std::string> country_codes_;

  // Per-row buttons keyed by index into a parallel list of codes, where
  // index 0 is the "Any country" row (code "") and index i>0 maps to
  // country_codes_[i-1]. Used to redraw the selection checkmark via SetText.
  std::vector<raw_ptr<views::LabelButton>> row_buttons_;
  std::vector<std::string> row_codes_;  // parallel to row_buttons_

  raw_ptr<views::Label> ip_label_ = nullptr;
  raw_ptr<views::Label> note_label_ = nullptr;

  base::WeakPtrFactory<MoltExitCountryBubble> weak_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_VIEWS_MOLT_AI_MOLT_EXIT_COUNTRY_BUBBLE_H_
