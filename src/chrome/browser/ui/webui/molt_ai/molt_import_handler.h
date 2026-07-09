// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// WebUIMessageHandler backing chrome://molt-import/. Bridges the glass Import
// popover's chrome.send() calls to molt_ai::BrowserImporter (detect installed
// browsers, read a chosen profile off-thread) and writes the harvested
// bookmarks/passwords into this profile's BookmarkModel and PasswordStore.
// This is the same backend the molt://ai-settings import section uses; the
// message names + payload shapes are kept identical so it is a drop-in.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_IMPORT_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_IMPORT_HANDLER_H_

#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/import/browser_importer.h"
#include "content/public/browser/web_ui_message_handler.h"

class Profile;

namespace bookmarks {
class BookmarkModel;
class BookmarkNode;
}  // namespace bookmarks

class MoltImportHandler : public content::WebUIMessageHandler {
 public:
  MoltImportHandler();
  ~MoltImportHandler() override;

  MoltImportHandler(const MoltImportHandler&) = delete;
  MoltImportHandler& operator=(const MoltImportHandler&) = delete;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;

 private:
  // Stable lowercase wire id <-> BrowserId. The popover and the importer agree
  // on exactly one wire format for the id.
  static std::string BrowserIdToString(molt_ai::BrowserId id);
  static bool StringToBrowserId(const std::string& s, molt_ai::BrowserId* out);

  // getBookmarks(callback_id) -> {bookmarks:[{title,url,host,folder}]}.
  // Reads THIS profile's own BookmarkModel so the popover's default view can
  // show the user's existing bookmarks. Resolves an empty list if the model is
  // absent or not yet loaded. Capped at a few hundred entries for the compact
  // popover.
  void HandleGetBookmarks(const base::ListValue& args);
  // openBookmark(callback_id, url) -> {ok:true}. Opens an http(s) URL in a new
  // tab of the active browser window (mirrors the chat handler's open-in-tab).
  void HandleOpenBookmark(const base::ListValue& args);

  // getImportableBrowsers -> {browsers:[{id,display_name,installed,
  // has_bookmarks,has_passwords_store}]}. Stat-only, cheap enough inline.
  void HandleGetImportableBrowsers(const base::ListValue& args);
  // importFromBrowser(callback_id, browser_id_string, include_passwords):
  // reads the chosen browser's profile off-thread, then writes it in.
  void HandleImportFromBrowser(const base::ListValue& args);
  // Legacy importFromChrome(callback_id, include_passwords) — delegates to the
  // generalized path. Kept for drop-in parity with the settings handler.
  void HandleImportFromChrome(const base::ListValue& args);

  void OnBrowserProfileRead(const std::string& callback_id,
                            bool include_passwords,
                            molt_ai::BrowserImportData data);

  const bookmarks::BookmarkNode* GetOrCreateFolderPath(
      bookmarks::BookmarkModel* model,
      const bookmarks::BookmarkNode* root,
      const std::string& folder_path);
  int ImportBookmarks(Profile* profile,
                      const std::string& source_display_name,
                      const std::vector<molt_ai::BrowserBookmark>& bookmarks);
  void ImportPasswords(
      Profile* profile,
      const std::vector<molt_ai::BrowserCredential>& credentials,
      int* imported,
      int* failures);
  void FireImportProgress(const std::string& phase, int done, int total);

  base::WeakPtrFactory<MoltImportHandler> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_IMPORT_HANDLER_H_
