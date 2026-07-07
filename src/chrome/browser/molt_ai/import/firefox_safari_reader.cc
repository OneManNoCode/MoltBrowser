// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Bookmark readers for Firefox and Safari, implementing the two
// import_internal functions declared in browser_importer.h. These are pure
// DATA-layer readers: they read the user's own local browser profile and
// never write anything. No UI, no passwords (bookmarks only for these two
// sources this phase — Firefox NSS is blocked on mac and Safari passwords
// require per-item keychain prompts).
//
//  * ReadFirefoxBookmarks() — cross-platform. Opens a COPY of the profile's
//    places.sqlite (Firefox holds a lock on the live file) and walks the
//    moz_bookmarks tree from the toolbar/menu/unfiled roots.
//  * ReadSafariBookmarks() — macOS only. Reads the TCC-protected
//    ~/Library/Safari/Bookmarks.plist; if the app lacks Full Disk Access the
//    read fails with EPERM and we report needs_full_disk_access instead of
//    crashing.

#include "chrome/browser/molt_ai/import/browser_importer.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_MAC)
#include <CoreFoundation/CoreFoundation.h>

#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/strings/sys_string_conversions.h"
#endif  // BUILDFLAG(IS_MAC)

namespace molt_ai {
namespace import_internal {

namespace {

// ---------------------------------------------------------------------------
// Firefox
// ---------------------------------------------------------------------------

// Firefox bookmark item types, from
// toolkit/components/places/nsINavBookmarksService.idl.
constexpr int kFirefoxTypeBookmark = 1;  // A URL row (moz_bookmarks.fk valid).
constexpr int kFirefoxTypeFolder = 2;    // A folder.

// Roots we care about, identified by their fixed GUIDs (matches
// chrome/utility/importer/firefox_importer.cc).
constexpr char kToolbarGuid[] = "toolbar_____";
constexpr char kMenuGuid[] = "menu________";
constexpr char kUnfiledGuid[] = "unfiled_____";

// Returns true for schemes we deliberately never import (internal Firefox
// URLs). Mirrors CanImportURL() in the in-tree Firefox importer.
bool CanImportFirefoxUrl(const GURL& url) {
  if (!url.is_valid()) {
    return false;
  }
  static constexpr const char* kInvalidSchemes[] = {"wyciwyg", "place",
                                                     "about", "chrome"};
  for (const char* scheme : kInvalidSchemes) {
    if (url.SchemeIs(scheme)) {
      return false;
    }
  }
  return true;
}

// Looks up the moz_bookmarks.id of a root folder by its fixed GUID. Returns
// -1 if not found.
int LookupFirefoxNodeIdByGuid(sql::Database& db, const std::string& guid) {
  sql::Statement s(
      db.GetUniqueStatement("SELECT id FROM moz_bookmarks WHERE guid = ?"));
  if (!s.is_valid()) {
    return -1;
  }
  s.BindString(0, guid);
  if (!s.Step()) {
    return -1;
  }
  return s.ColumnInt(0);
}

// Recursively walks the children of `parent_id`. `folder_path` is the '/'-
// joined path of ancestor folder names (matching the Chrome importer's folder
// path convention); the caller seeds it with the root's display name. URL
// rows are appended to `out`; folders recurse. Malformed rows are skipped.
// Self-recursive, so it's fully defined here (no forward declaration needed).
void WalkFirefoxFolderImpl(sql::Database& db,
                           int parent_id,
                           const std::string& folder_path,
                           std::vector<BrowserBookmark>& out) {
  // Snapshot this level's rows first (title, url, type, id) so the shared
  // statement isn't held across the recursive descent.
  struct Row {
    int id;
    int type;
    std::u16string title;
    std::string url;
  };
  std::vector<Row> rows;
  {
    sql::Statement s(db.GetUniqueStatement(
        "SELECT b.id, b.type, COALESCE(b.title, h.title), h.url "
        "FROM moz_bookmarks b "
        "LEFT JOIN moz_places h ON b.fk = h.id "
        "WHERE b.type IN (1, 2) AND b.parent = ? "
        "ORDER BY b.position"));
    if (!s.is_valid()) {
      return;
    }
    s.BindInt(0, parent_id);
    while (s.Step()) {
      Row row;
      row.id = s.ColumnInt(0);
      row.type = s.ColumnInt(1);
      row.title = s.ColumnString16(2);
      row.url = s.ColumnString(3);
      rows.push_back(std::move(row));
    }
  }

  for (const Row& row : rows) {
    if (row.type == kFirefoxTypeBookmark) {
      GURL gurl(row.url);
      if (!CanImportFirefoxUrl(gurl)) {
        continue;
      }
      BrowserBookmark bookmark;
      bookmark.title = row.title;
      bookmark.url = std::move(gurl);
      bookmark.folder_path = folder_path;
      out.push_back(std::move(bookmark));
    } else if (row.type == kFirefoxTypeFolder) {
      std::string child_path = folder_path;
      std::string name = base::UTF16ToUTF8(row.title);
      if (!name.empty()) {
        if (!child_path.empty()) {
          child_path.append("/");
        }
        child_path.append(name);
      }
      WalkFirefoxFolderImpl(db, row.id, child_path, out);
    }
  }
}

// Seeds a walk from one named root. `root_display_name` becomes the base
// folder path (e.g. "Bookmarks Toolbar"), matching how the Chrome importer
// uses the root's own name.
void WalkFirefoxRoot(sql::Database& db,
                     const std::string& root_guid,
                     const std::string& root_display_name,
                     std::vector<BrowserBookmark>& out) {
  int root_id = LookupFirefoxNodeIdByGuid(db, root_guid);
  if (root_id < 0) {
    return;
  }
  WalkFirefoxFolderImpl(db, root_id, root_display_name, out);
}

// ---------------------------------------------------------------------------
// Safari (macOS only)
// ---------------------------------------------------------------------------

#if BUILDFLAG(IS_MAC)

// Recursively reads a Safari bookmark folder dictionary from the parsed plist.
// Mirrors RecursiveReadBookmarksFolder() in
// chrome/utility/importer/safari_importer.mm, but using the CoreFoundation C
// API (so this file compiles as plain .cc, not .mm). `folder_path` is the
// '/'-joined ancestor folder path. Leaf entries -> BrowserBookmark.
void RecursiveReadSafariFolder(CFDictionaryRef folder,
                               const std::string& folder_path,
                               std::vector<BrowserBookmark>& out) {
  if (!folder) {
    return;
  }

  CFArrayRef children = base::apple::CFCast<CFArrayRef>(
      CFDictionaryGetValue(folder, CFSTR("Children")));
  if (!children) {
    return;
  }

  CFIndex count = CFArrayGetCount(children);
  for (CFIndex i = 0; i < count; ++i) {
    CFDictionaryRef entry = base::apple::CFCast<CFDictionaryRef>(
        CFArrayGetValueAtIndex(children, i));
    if (!entry) {
      continue;
    }

    CFStringRef entry_type = base::apple::CFCast<CFStringRef>(
        CFDictionaryGetValue(entry, CFSTR("WebBookmarkType")));
    if (!entry_type) {
      continue;
    }

    if (CFStringCompare(entry_type, CFSTR("WebBookmarkTypeList"), 0) ==
        kCFCompareEqualTo) {
      // Sub-folder: extend the path with its Title and recurse. The special
      // "BookmarksMenu" root name is omitted from the path to avoid excessive
      // nesting (matching the in-tree Safari importer).
      std::string child_path = folder_path;
      CFStringRef title = base::apple::CFCast<CFStringRef>(
          CFDictionaryGetValue(entry, CFSTR("Title")));
      if (title &&
          CFStringCompare(title, CFSTR("BookmarksMenu"), 0) !=
              kCFCompareEqualTo) {
        std::string name = base::SysCFStringRefToUTF8(title);
        if (!name.empty()) {
          if (!child_path.empty()) {
            child_path.append("/");
          }
          child_path.append(name);
        }
      }
      RecursiveReadSafariFolder(entry, child_path, out);
      continue;
    }

    if (CFStringCompare(entry_type, CFSTR("WebBookmarkTypeLeaf"), 0) !=
        kCFCompareEqualTo) {
      continue;
    }

    // Leaf bookmark: URLString + URIDictionary.title.
    CFStringRef url_string = base::apple::CFCast<CFStringRef>(
        CFDictionaryGetValue(entry, CFSTR("URLString")));
    if (!url_string) {
      continue;
    }
    CFDictionaryRef uri_dict = base::apple::CFCast<CFDictionaryRef>(
        CFDictionaryGetValue(entry, CFSTR("URIDictionary")));
    CFStringRef title = nullptr;
    if (uri_dict) {
      title = base::apple::CFCast<CFStringRef>(
          CFDictionaryGetValue(uri_dict, CFSTR("title")));
    }

    GURL gurl(base::SysCFStringRefToUTF8(url_string));
    if (!gurl.is_valid()) {
      continue;
    }

    BrowserBookmark bookmark;
    bookmark.title =
        title ? base::SysCFStringRefToUTF16(title) : std::u16string();
    bookmark.url = std::move(gurl);
    bookmark.folder_path = folder_path;
    out.push_back(std::move(bookmark));
  }
}

#endif  // BUILDFLAG(IS_MAC)

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points (declared in browser_importer.h)
// ---------------------------------------------------------------------------

std::vector<BrowserBookmark> ReadFirefoxBookmarks(
    const base::FilePath& profile_dir) {
  std::vector<BrowserBookmark> bookmarks;
  if (profile_dir.empty()) {
    return bookmarks;
  }

  base::FilePath places = profile_dir.Append("places.sqlite");
  if (!base::PathExists(places)) {
    return bookmarks;  // No Firefox profile / no places DB — not an error.
  }

  // Firefox keeps places.sqlite locked while running; work on a private copy
  // in a scoped temp dir so any WAL/SHM side files stay isolated and are
  // cleaned up automatically.
  base::ScopedTempDir temp_dir;
  if (!temp_dir.CreateUniqueTempDir()) {
    return bookmarks;
  }
  base::FilePath temp_db = temp_dir.GetPath().Append("places.sqlite");
  if (!base::CopyFile(places, temp_db)) {
    return bookmarks;
  }

  {
    sql::Database db(sql::Database::Tag("FirefoxImport"));
    if (!db.Open(temp_db)) {
      return bookmarks;  // temp_dir cleans up on scope exit.
    }

    // Walk the three standard roots. Root display names mirror Firefox's own
    // UI labels and become the base folder_path for their contents.
    WalkFirefoxRoot(db, kToolbarGuid, "Bookmarks Toolbar", bookmarks);
    WalkFirefoxRoot(db, kMenuGuid, "Bookmarks Menu", bookmarks);
    WalkFirefoxRoot(db, kUnfiledGuid, "Other Bookmarks", bookmarks);
    // db closes at scope exit, before temp_dir removal.
  }

  return bookmarks;
}

SafariResult ReadSafariBookmarks() {
  SafariResult result;

#if BUILDFLAG(IS_MAC)
  base::FilePath home = base::GetHomeDir();
  if (home.empty()) {
    result.ok = false;
    return result;
  }
  base::FilePath plist_path =
      home.Append("Library").Append("Safari").Append("Bookmarks.plist");

  // ~/Library/Safari is TCC-protected (macOS 10.14+). A non-FDA app gets EPERM
  // trying to read it. Detect that up front and report needs_full_disk_access
  // rather than surfacing an opaque failure. PathExists() itself can fail
  // under TCC, so treat "not readable" as the FDA signal whenever the parent
  // Safari directory exists but the file cannot be read.
  if (!base::PathIsReadable(plist_path)) {
    // Distinguish "Safari never created a bookmarks file" (fine, empty) from
    // "TCC denied us" (needs FDA). If the Safari dir isn't even readable, or
    // the file exists but we can't read it, assume TCC.
    base::FilePath safari_dir = home.Append("Library").Append("Safari");
    bool safari_dir_present = base::PathExists(safari_dir);
    if (safari_dir_present && !base::PathIsReadable(safari_dir)) {
      // Directory is there but unreadable -> classic TCC denial.
      result.needs_full_disk_access = true;
      result.ok = false;
      return result;
    }
    // The bookmarks file just isn't there (or the whole Safari dir is absent):
    // Safari not set up / no bookmarks. Not an FDA problem.
    result.ok = true;
    return result;
  }

  // Readable: load and parse the (binary) plist via CoreFoundation.
  std::string data_bytes;
  if (!base::ReadFileToString(plist_path, &data_bytes)) {
    // Readable stat but read failed — treat as a TCC edge case, not a crash.
    result.needs_full_disk_access = true;
    result.ok = false;
    return result;
  }

  base::apple::ScopedCFTypeRef<CFDataRef> cf_data(CFDataCreate(
      kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(data_bytes.data()),
      static_cast<CFIndex>(data_bytes.size())));
  if (!cf_data) {
    result.ok = false;
    return result;
  }

  base::apple::ScopedCFTypeRef<CFPropertyListRef> plist(
      CFPropertyListCreateWithData(kCFAllocatorDefault, cf_data.get(),
                                   kCFPropertyListImmutable,
                                   /*format=*/nullptr, /*error=*/nullptr));
  CFDictionaryRef root =
      plist ? base::apple::CFCast<CFDictionaryRef>(plist.get()) : nullptr;
  if (!root) {
    // Present and readable but unparseable — return empty-but-ok (no crash).
    result.ok = true;
    return result;
  }

  // The top-level container's Children hold the bookmark roots (BookmarksBar,
  // BookmarksMenu, and user folders). Walk from the top with an empty path.
  RecursiveReadSafariFolder(root, /*folder_path=*/std::string(),
                            result.bookmarks);
  result.ok = true;
  return result;
#else
  // Non-mac: Safari does not exist.
  result.ok = false;
  return result;
#endif  // BUILDFLAG(IS_MAC)
}

}  // namespace import_internal
}  // namespace molt_ai
