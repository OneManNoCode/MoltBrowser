// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryRecorder — per-tab WebContentsObserver. Fires once per
// successful main-frame load, asks the page for innerText, hands it
// off to the MemoryService for chunking/embedding/storage.
//
// One MemoryRecorder per WebContents (attached as a TabHelper). The
// service holds a weak ref to the recorder rather than owning it.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_RECORDER_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_RECORDER_H_

#include "base/memory/weak_ptr.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

class Profile;

namespace molt_ai {
namespace memory {

class MemoryService;

class MemoryRecorderTabHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<MemoryRecorderTabHelper> {
 public:
  ~MemoryRecorderTabHelper() override;

  // content::WebContentsObserver
  void DocumentOnLoadCompletedInPrimaryMainFrame() override;

 private:
  friend class content::WebContentsUserData<MemoryRecorderTabHelper>;
  explicit MemoryRecorderTabHelper(content::WebContents* contents);

  // Fetch innerText from the page and forward to the service.
  void CapturePage();

  base::WeakPtrFactory<MemoryRecorderTabHelper> weak_factory_{this};
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_RECORDER_H_
