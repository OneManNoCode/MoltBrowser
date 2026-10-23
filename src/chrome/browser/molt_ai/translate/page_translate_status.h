// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Per tab: what the on-device translation of the tab's page is doing, for
// the Translate bubble's footnote (page_translate_footnote_view.h), and the
// Stop it offers. MoltTranslateDriver writes it; the footnote reads it.
// Design: docs/design/PAGE_TRANSLATION.md §4.5.

#ifndef CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_STATUS_H_
#define CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_STATUS_H_

#include <cstddef>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "chrome/browser/molt_ai/translate/page_translate_messages.h"
#include "content/public/browser/web_contents_user_data.h"

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai::page_translate {

class PageTranslateStatus
    : public content::WebContentsUserData<PageTranslateStatus> {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnPageTranslateStatusChanged() = 0;
    // The tab is going away; drop the pointer.
    virtual void OnPageTranslateStatusDestroyed() = 0;
  };

  PageTranslateStatus(const PageTranslateStatus&) = delete;
  PageTranslateStatus& operator=(const PageTranslateStatus&) = delete;
  ~PageTranslateStatus() override;

  const StatusSnapshot& snapshot() const { return snapshot_; }
  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // The driver.
  void SetIdle();
  void SetPreparing(base::RepeatingClosure stop);
  void SetProgress(size_t settled, size_t total);
  void SetResult(const Report& report, size_t failed_units, bool truncated);

  // The footnote's Stop.
  void Stop();

  // Asks the on-device runtime whether this device can run a model and has
  // one downloaded; observers hear when the answer arrives. Reads files on
  // disk only: never downloads anything.
  void RefreshDeviceFacts();

 private:
  friend class content::WebContentsUserData<PageTranslateStatus>;
  explicit PageTranslateStatus(content::WebContents* web_contents);

  void Changed();

  StatusSnapshot snapshot_;
  base::RepeatingClosure stop_;
  base::ObserverList<Observer> observers_;
  base::WeakPtrFactory<PageTranslateStatus> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

// Whether Translate may pop its bubble up by itself on a page in another
// language: only once a model is known to be downloaded, so a user who
// never set one up isn't offered a translation that can't run. The menu
// items and the address bar icon stay. Answers from the last check and
// starts a new one. ChromeTranslateClient::ShowTranslateUI() asks it.
bool ShouldOfferTranslationAutomatically();

}  // namespace molt_ai::page_translate

#endif  // CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_STATUS_H_
