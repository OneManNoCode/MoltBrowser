// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Translate's driver in MoltBrowser: upstream's ContentTranslateDriver
// (page registration, language detection, navigation, the bubble and the
// prefs are all upstream's), except that "Translate" and "Show original"
// run on the user's own on-device model instead of Google's translate
// script. ChromeTranslateClient creates it instead of
// ContentTranslateDriver. Design: docs/design/PAGE_TRANSLATION.md.
//
// TranslatesOnDevice() makes TranslateManager skip Google's script, the
// API-key and offline checks, partial translate and every automatic
// trigger but "always translate" (translate_manager.cc's MoltBrowser
// hooks). The page's text never leaves the device, in every window.
//
// In MoltNet windows page translation is off: TranslationOff() makes
// TranslateManager offer nothing by itself (no bubble, no icon, no
// automatic translation), and TranslatePage() refuses anything the menus
// still ask for, with an error the bubble's footnote explains. Google's
// script stays unreachable there too (TranslatesOnDevice() is true).

#ifndef CHROME_BROWSER_MOLT_AI_TRANSLATE_MOLT_TRANSLATE_DRIVER_H_
#define CHROME_BROWSER_MOLT_AI_TRANSLATE_MOLT_TRANSLATE_DRIVER_H_

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "base/memory/weak_ptr.h"
#include "chrome/browser/molt_ai/translate/page_translate_plan.h"
#include "components/translate/content/browser/content_translate_driver.h"

namespace content {
class NavigationHandle;
class WebContents;
}  // namespace content

namespace language {
class UrlLanguageHistogram;
}  // namespace language

namespace molt_ai::page_translate {

class PageTranslationJob;
class PageTranslateStatus;

class MoltTranslateDriver : public ::translate::ContentTranslateDriver {
 public:
  MoltTranslateDriver(content::WebContents& web_contents,
                      language::UrlLanguageHistogram* url_language_histogram);
  MoltTranslateDriver(const MoltTranslateDriver&) = delete;
  MoltTranslateDriver& operator=(const MoltTranslateDriver&) = delete;
  ~MoltTranslateDriver() override;

  // translate::TranslateDriver:
  bool TranslatesOnDevice() const override;
  // True in MoltNet windows (PageTranslationOff()): Translate offers
  // nothing there, and TranslatePage() refuses with an error.
  bool TranslationOff() const override;
  void PrepareToTranslatePage(int page_seq_no,
                              std::string_view original_source_lang,
                              std::string_view target_lang,
                              bool triggered_from_menu) override;
  void TranslatePage(int page_seq_no,
                     std::string_view translate_script,
                     std::string_view source_lang,
                     std::string_view target_lang) override;
  void RevertTranslation(int page_seq_no) override;

  // content::WebContentsObserver:
  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override;
  void WebContentsDestroyed() override;

 private:
  PageTranslateStatus* Status();
  void StopJob();
  void OnProgress(size_t settled, size_t total);
  void OnJobDone(std::string source, std::string target, JobResult result);
  // Tells TranslateManager (and the bubble) how it went, later.
  void PostReport(std::string source, std::string target, TranslateError error);
  void ReportToManager(std::string source,
                       std::string target,
                       TranslateError error);

  // A MoltNet window (decided once: a tab never changes profile).
  const bool moltnet_;
  // The page's language as TranslatePage()'s caller had it, before it
  // turned an unsupported one into "und".
  std::string prepared_source_;
  // Groups this tab's generations in the web AI backend's scheduler.
  const std::string schedule_key_;
  std::unique_ptr<PageTranslationJob> job_;

  base::WeakPtrFactory<MoltTranslateDriver> weak_factory_{this};
};

}  // namespace molt_ai::page_translate

#endif  // CHROME_BROWSER_MOLT_AI_TRANSLATE_MOLT_TRANSLATE_DRIVER_H_
