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
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "ui/base/page_transition_types.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

namespace {

// The compact popover only shows a few hundred rows; cap the walk so a huge
// bookmark tree can't bloat the IPC payload or the scroll list.
constexpr size_t kMaxBookmarkEntries = 300;

// Depth-first append of every URL node under |node| into |out| as
// {title,url,host,folder}. |folder| is the title of the immediate containing
// folder ("" for bookmarks sitting directly under a permanent root such as the
// bookmark bar). Folders themselves are skipped but recursed into. Stops once
// the cap is reached.
void CollectBookmarks(const bookmarks::BookmarkNode* node,
                      const std::string& folder,
                      base::ListValue* out) {
  if (!node)
    return;
  for (const auto& child : node->children()) {
    if (out->size() >= kMaxBookmarkEntries)
      return;
    if (child->is_url()) {
      base::DictValue entry;
      entry.Set("title", base::UTF16ToUTF8(child->GetTitle()));
      entry.Set("url", child->url().spec());
      entry.Set("host", child->url().host());
      entry.Set("folder", folder);
      // For the popover's "Recent" section: ms-since-epoch of last open and of
      // add. |used| is 0 when the bookmark has never been opened.
      entry.Set("used",
                child->date_last_used().is_null()
                    ? 0.0
                    : static_cast<double>(
                          child->date_last_used().InMillisecondsSinceUnixEpoch()));
      entry.Set("added", static_cast<double>(
                             child->date_added().InMillisecondsSinceUnixEpoch()));
      out->Append(std::move(entry));
    } else if (child->is_folder()) {
      CollectBookmarks(child.get(), base::UTF16ToUTF8(child->GetTitle()), out);
    }
  }
}

}  // namespace

MoltImportHandler::MoltImportHandler() = default;
MoltImportHandler::~MoltImportHandler() = default;

void MoltImportHandler::RegisterMessages() {
  // getBookmarks: read this profile's own bookmarks for the default popover
  // view.
  web_ui()->RegisterMessageCallback(
      "getBookmarks",
      base::BindRepeating(&MoltImportHandler::HandleGetBookmarks,
                          base::Unretained(this)));
  // openBookmark: open one of the user's bookmarks in a new tab.
  web_ui()->RegisterMessageCallback(
      "openBookmark",
      base::BindRepeating(&MoltImportHandler::HandleOpenBookmark,
                          base::Unretained(this)));
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

void MoltImportHandler::HandleGetBookmarks(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string callback_id = args[0].GetString();

  base::DictValue out;
  base::ListValue bookmark_list;

  bookmarks::BookmarkModel* model =
      BookmarkModelFactory::GetForBrowserContext(Profile::FromWebUI(web_ui()));
  if (model && model->loaded()) {
    // Bookmark bar first, then "Other", then mobile (if present). Bookmarks
    // sitting directly under a permanent root carry an empty folder label.
    CollectBookmarks(model->bookmark_bar_node(), std::string(), &bookmark_list);
    CollectBookmarks(model->other_node(), std::string(), &bookmark_list);
    if (model->mobile_node())
      CollectBookmarks(model->mobile_node(), std::string(), &bookmark_list);
  }

  out.Set("bookmarks", std::move(bookmark_list));
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));
}

void MoltImportHandler::HandleOpenBookmark(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string callback_id = args[0].GetString();
  const std::string url =
      args[1].is_string() ? args[1].GetString() : std::string();

  base::DictValue out;

  // Re-validate here: only navigate to a well-formed http(s) URL. A bad or
  // non-http(s) string is rejected rather than "fixed up".
  GURL gurl(url);
  if (!gurl.is_valid() || !gurl.SchemeIsHTTPOrHTTPS()) {
    out.Set("ok", false);
    out.Set("error", "unsupported url");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Resolve the browser window that should receive the new tab. This popover's
  // WebContents lives in a bubble widget, not a tab strip, so FindBrowserWithTab
  // returns null — fall back to the most-recently-active browser window.
  content::WebContents* webui_wc = web_ui()->GetWebContents();
  Browser* browser = chrome::FindBrowserWithTab(webui_wc);
  if (!browser)
    browser = chrome::FindLastActive();
  if (!browser) {
    out.Set("ok", false);
    out.Set("error", "no active browser window");
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
    return;
  }

  // Open the bookmark in the CURRENT tab (the one the popover was opened over),
  // not a new tab — otherwise every bookmark click stacks up another tab and you
  // end up with a chain of MoltSearch/blank tabs.
  NavigateParams nav_params(browser, gurl, ui::PAGE_TRANSITION_AUTO_BOOKMARK);
  nav_params.disposition = WindowOpenDisposition::CURRENT_TAB;
  Navigate(&nav_params);
  out.Set("ok", true);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(out)));

  // Dismiss the popover after opening a bookmark. It's close_on_deactivate=false
  // (for the Keychain import flow), so it won't close on its own and would
  // otherwise sit over the page we just navigated to. Close is async, so doing
  // it after resolving the callback is safe.
  if (views::Widget* bubble = views::Widget::GetTopLevelWidgetForNativeView(
          webui_wc->GetNativeView())) {
    bubble->Close();
  }
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
