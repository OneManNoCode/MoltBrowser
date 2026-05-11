// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_recorder.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/molt_ai/memory/memory_service.h"
#include "chrome/browser/molt_ai/memory/memory_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace molt_ai {
namespace memory {

MemoryRecorderTabHelper::MemoryRecorderTabHelper(
    content::WebContents* contents)
    : content::WebContentsObserver(contents),
      content::WebContentsUserData<MemoryRecorderTabHelper>(*contents) {}

MemoryRecorderTabHelper::~MemoryRecorderTabHelper() = default;

void MemoryRecorderTabHelper::DocumentOnLoadCompletedInPrimaryMainFrame() {
  CapturePage();
}

void MemoryRecorderTabHelper::CapturePage() {
  content::WebContents* wc = web_contents();
  if (!wc || !wc->GetPrimaryMainFrame()) return;

  GURL url = wc->GetLastCommittedURL();
  if (!url.is_valid()) return;

  Profile* profile = Profile::FromBrowserContext(wc->GetBrowserContext());
  if (!profile) return;
  MemoryService* svc = MemoryServiceFactory::GetForProfile(profile);
  if (!svc) return;

  // Privacy gate runs synchronously so we don't even bother extracting
  // text for an excluded URL.
  if (!svc->ShouldCapture(url)) return;

  std::u16string title = wc->GetTitle();
  std::string title_utf8 = base::UTF16ToUTF8(title);

  // Ask the page for its visible innerText. We cap server-side at
  // ~200KB to avoid pathological infinite-scroll DOMs. The JS isolated
  // world is the same one the automation recorder uses — keeps page
  // scripts from observing our extraction.
  wc->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      u"(document.body && document.body.innerText.slice(0, 204800)) || ''",
      base::BindOnce(
          [](base::WeakPtr<MemoryRecorderTabHelper> self, GURL url,
             std::string title, base::Value v) {
            if (!self || !self->web_contents()) return;
            if (!v.is_string()) return;
            Profile* p = Profile::FromBrowserContext(
                self->web_contents()->GetBrowserContext());
            if (!p) return;
            MemoryService* s = MemoryServiceFactory::GetForProfile(p);
            if (!s) return;
            s->IngestPage(std::move(url), std::move(title), v.GetString());
          },
          weak_factory_.GetWeakPtr(), url, std::move(title_utf8)),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(MemoryRecorderTabHelper);

}  // namespace memory
}  // namespace molt_ai
