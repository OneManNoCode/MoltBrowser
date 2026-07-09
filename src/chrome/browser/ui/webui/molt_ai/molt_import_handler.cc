// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_import_handler.h"

#include <string>
#include <utility>
#include <vector>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "base/strings/string_split.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/molt_ai/import/browser_importer.h"
#include "chrome/browser/password_manager/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"
#include "content/public/browser/web_ui.h"
#include "url/gurl.h"

MoltImportHandler::MoltImportHandler() = default;
MoltImportHandler::~MoltImportHandler() = default;

void MoltImportHandler::RegisterMessages() {
  // getImportableBrowsers: stat-only detection of installed browsers.
  web_ui()->RegisterMessageCallback(
      "getImportableBrowsers",
      base::BindRepeating(&MoltImportHandler::HandleGetImportableBrowsers,
                          base::Unretained(this)));
  // importFromBrowser: read+write one detected browser's profile.
  web_ui()->RegisterMessageCallback(
      "importFromBrowser",
      base::BindRepeating(&MoltImportHandler::HandleImportFromBrowser,
                          base::Unretained(this)));
  // importFromChrome: legacy message kept working for drop-in parity — it
  // delegates to importFromBrowser("chrome", include_passwords).
  web_ui()->RegisterMessageCallback(
      "importFromChrome",
      base::BindRepeating(&MoltImportHandler::HandleImportFromChrome,
                          base::Unretained(this)));
}

// Map a stable lowercase string id (used by the WebUI) to a BrowserId, and
// back. Keeping the mapping local means the popover and the importer agree on
// exactly one wire format for the id.
// static
std::string MoltImportHandler::BrowserIdToString(molt_ai::BrowserId id) {
  switch (id) {
    case molt_ai::BrowserId::kChrome:
      return "chrome";
    case molt_ai::BrowserId::kChromium:
      return "chromium";
    case molt_ai::BrowserId::kEdge:
      return "edge";
    case molt_ai::BrowserId::kBrave:
      return "brave";
    case molt_ai::BrowserId::kOpera:
      return "opera";
    case molt_ai::BrowserId::kVivaldi:
      return "vivaldi";
    case molt_ai::BrowserId::kFirefox:
      return "firefox";
    case molt_ai::BrowserId::kSafari:
      return "safari";
  }
  return "chrome";
}

// Returns false (and leaves |out| untouched) for an unknown id string.
// static
bool MoltImportHandler::StringToBrowserId(const std::string& s,
                                          molt_ai::BrowserId* out) {
  if (s == "chrome") {
    *out = molt_ai::BrowserId::kChrome;
  } else if (s == "chromium") {
    *out = molt_ai::BrowserId::kChromium;
  } else if (s == "edge") {
    *out = molt_ai::BrowserId::kEdge;
  } else if (s == "brave") {
    *out = molt_ai::BrowserId::kBrave;
  } else if (s == "opera") {
    *out = molt_ai::BrowserId::kOpera;
  } else if (s == "vivaldi") {
    *out = molt_ai::BrowserId::kVivaldi;
  } else if (s == "firefox") {
    *out = molt_ai::BrowserId::kFirefox;
  } else if (s == "safari") {
    *out = molt_ai::BrowserId::kSafari;
  } else {
    return false;
  }
  return true;
}

void MoltImportHandler::HandleGetImportableBrowsers(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  base::DictValue out;
  base::ListValue browsers;
  for (const molt_ai::DetectedBrowser& b :
       molt_ai::BrowserImporter::DetectInstalled()) {
    base::DictValue entry;
    entry.Set("id", BrowserIdToString(b.id));
    entry.Set("display_name", b.display_name);
    entry.Set("installed", b.installed);
    entry.Set("has_bookmarks", b.has_bookmarks);
    entry.Set("has_passwords_store", b.has_passwords_store);
    browsers.Append(std::move(entry));
  }
  out.Set("browsers", std::move(browsers));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

void MoltImportHandler::HandleImportFromBrowser(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 3u);
  const std::string callback_id = args[0].GetString();
  const std::string browser_id_string =
      args[1].is_string() ? args[1].GetString() : std::string();
  const bool include_passwords = args[2].is_bool() && args[2].GetBool();

  molt_ai::BrowserId browser_id;
  if (!StringToBrowserId(browser_id_string, &browser_id)) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("browser_id", browser_id_string);
    result.Set("error", "Unknown browser");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&molt_ai::BrowserImporter::ReadProfile, browser_id,
                     include_passwords),
      base::BindOnce(&MoltImportHandler::OnBrowserProfileRead,
                     weak_ptr_factory_.GetWeakPtr(), callback_id,
                     include_passwords));
}

void MoltImportHandler::HandleImportFromChrome(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  const bool include_passwords = args[1].is_bool() && args[1].GetBool();

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&molt_ai::BrowserImporter::ReadProfile,
                     molt_ai::BrowserId::kChrome, include_passwords),
      base::BindOnce(&MoltImportHandler::OnBrowserProfileRead,
                     weak_ptr_factory_.GetWeakPtr(), callback_id,
                     include_passwords));
}

void MoltImportHandler::OnBrowserProfileRead(const std::string& callback_id,
                                             bool include_passwords,
                                             molt_ai::BrowserImportData data) {
  // The handler could have outlived its WebContents; re-guard against a
  // torn-down page.
  if (!IsJavascriptAllowed())
    return;

  const std::string browser_id = BrowserIdToString(data.source_id);

  if (!data.profile_found) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("browser_id", browser_id);
    result.Set("needs_full_disk_access", data.needs_full_disk_access);
    result.Set("error",
               data.error.empty()
                   ? ("No " + data.display_name + " profile found")
                   : data.error);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
    return;
  }

  Profile* profile = Profile::FromWebUI(web_ui());

  // ---- Phase 1: bookmarks ----
  FireImportProgress("bookmarks", 0, static_cast<int>(data.bookmarks.size()));
  int bookmarks_imported =
      ImportBookmarks(profile, data.display_name, data.bookmarks);
  FireImportProgress("bookmarks", bookmarks_imported,
                     static_cast<int>(data.bookmarks.size()));

  // ---- Phase 2: passwords ----
  int passwords_imported = 0;
  int password_failures = 0;
  if (include_passwords && data.passwords_supported && !data.keychain_denied) {
    FireImportProgress("passwords", 0,
                       static_cast<int>(data.credentials.size()));
    ImportPasswords(profile, data.credentials, &passwords_imported,
                    &password_failures);
    FireImportProgress("passwords", passwords_imported,
                       static_cast<int>(data.credentials.size()));
  }

  base::DictValue result;
  result.Set("success", true);
  result.Set("browser_id", browser_id);
  result.Set("bookmarks_imported", bookmarks_imported);
  result.Set("passwords_imported", passwords_imported);
  result.Set("password_failures", password_failures);
  result.Set("keychain_denied", data.keychain_denied);
  result.Set("passwords_supported", data.passwords_supported);
  result.Set("needs_full_disk_access", data.needs_full_disk_access);
  if (!data.error.empty())
    result.Set("error", data.error);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

// Find-or-create the nested folder chain described by |folder_path| (e.g.
// "Bookmarks Bar/Tech/News") under |root|, creating any missing folders, and
// return the leaf folder. Empty/whitespace-only segments are skipped; an empty
// path returns |root| itself.
const bookmarks::BookmarkNode* MoltImportHandler::GetOrCreateFolderPath(
    bookmarks::BookmarkModel* model,
    const bookmarks::BookmarkNode* root,
    const std::string& folder_path) {
  const bookmarks::BookmarkNode* current = root;
  for (const std::string& segment :
       base::SplitString(folder_path, "/", base::TRIM_WHITESPACE,
                         base::SPLIT_WANT_NONEMPTY)) {
    const std::u16string seg16 = base::UTF8ToUTF16(segment);
    const bookmarks::BookmarkNode* next = nullptr;
    for (const auto& child : current->children()) {
      if (child->is_folder() && child->GetTitle() == seg16) {
        next = child.get();
        break;
      }
    }
    if (!next) {
      next = model->AddFolder(current, current->children().size(), seg16);
    }
    current = next;
  }
  return current;
}

// Creates (or reuses) a top-level "Imported from <Browser>" folder under the
// bookmark bar and RE-CREATES the source browser's nested folder hierarchy
// inside it, placing each bookmark in the same folder it had in the source
// browser (from bm.folder_path). Folders are found-or-created so bookmarks
// sharing a path group together, and repeat imports reuse existing folders
// instead of duplicating them.
int MoltImportHandler::ImportBookmarks(
    Profile* profile,
    const std::string& source_display_name,
    const std::vector<molt_ai::BrowserBookmark>& bookmarks) {
  bookmarks::BookmarkModel* model =
      BookmarkModelFactory::GetForBrowserContext(profile);
  if (!model || !model->loaded())
    return 0;

  const bookmarks::BookmarkNode* bar = model->bookmark_bar_node();
  const std::u16string folder_title =
      u"Imported from " + base::UTF8ToUTF16(source_display_name);

  // Reuse an existing "Imported from <Browser>" folder if the user has imported
  // from this source before, so repeated imports don't stack up duplicate
  // top-level folders.
  const bookmarks::BookmarkNode* root_folder = nullptr;
  for (const auto& child : bar->children()) {
    if (child->is_folder() && child->GetTitle() == folder_title) {
      root_folder = child.get();
      break;
    }
  }
  if (!root_folder)
    root_folder = model->AddFolder(bar, bar->children().size(), folder_title);

  int imported = 0;
  for (const auto& bm : bookmarks) {
    GURL url(bm.url);
    if (!url.is_valid())
      continue;
    const bookmarks::BookmarkNode* target =
        GetOrCreateFolderPath(model, root_folder, bm.folder_path);
    model->AddURL(target, target->children().size(), bm.title, url);
    ++imported;
  }
  return imported;
}

void MoltImportHandler::ImportPasswords(
    Profile* profile,
    const std::vector<molt_ai::BrowserCredential>& credentials,
    int* imported,
    int* failures) {
  scoped_refptr<password_manager::PasswordStoreInterface> store =
      ProfilePasswordStoreFactory::GetForProfile(
          profile, ServiceAccessType::EXPLICIT_ACCESS);
  if (!store) {
    *failures = static_cast<int>(credentials.size());
    return;
  }

  for (const auto& cred : credentials) {
    GURL url(cred.url);
    if (!url.is_valid() || cred.signon_realm.empty()) {
      ++(*failures);
      continue;
    }
    password_manager::PasswordForm form;
    form.signon_realm = cred.signon_realm;
    form.url = url;
    form.username_value = cred.username;
    form.password_value = cred.password;
    // AddLogin is asynchronous (writes on the store's own sequence); we count
    // it as accepted for import here. The store dedupes on its primary key, so
    // re-importing existing logins is a no-op there.
    store->AddLogin(form);
    ++(*imported);
  }
}

void MoltImportHandler::FireImportProgress(const std::string& phase,
                                           int done,
                                           int total) {
  base::DictValue progress;
  progress.Set("phase", phase);
  progress.Set("done", done);
  progress.Set("total", total);
  FireWebUIListener("import-progress", base::Value(std::move(progress)));
}
