// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_window.h"

#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/molt_ai/fingerprinting/molt_letterbox.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/tor/molt_net_routing.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/printing/background_printing_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_destroyer.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_collection_observer.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/simple_message_box.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_features.h"
#include "content/public/browser/web_contents.h"
#include "printing/buildflags/buildflags.h"
#include "ui/base/base_window.h"
#include "ui/base/mojom/window_show_state.mojom.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"

namespace molt_ai {
namespace tor {

namespace {

Profile* ProfileByPath(const base::FilePath& path) {
  if (!g_browser_process || !g_browser_process->profile_manager()) {
    return nullptr;
  }
  return g_browser_process->profile_manager()->GetProfileByPath(path);
}

// True if a browser window that hasn't closed yet uses `profile`. (The
// global collection drops a window once it has closed, before the Browser
// object itself is deleted.)
bool HasOpenBrowserWindow(const Profile* profile) {
  bool found = false;
  GlobalBrowserCollection::GetInstance()->ForEach(
      [profile, &found](BrowserWindowInterface* browser) {
        found = browser->GetProfile() == profile;
        return !found;
      });
  return found;
}

// A just-closed window's Browser is deleted by a task posted right after
// the close notification (Browser::OnWindowCloseComplete), and the profile's
// BrowserManagerService owns it, so the profile must outlive it. Re-post a
// few times until the Browser is gone; if it somehow lingers, leave the
// profile alone (a later close or New Identity retries).
constexpr int kMaxWaitsForClosedBrowser = 10;

// Destroys the MoltNet OTR profile |id| of the original profile at |path| if
// no Browser window uses it any more. Looked up again by (path, id) because
// the Profile* may already be gone when this runs. |closed_browser|, if
// set, is the window whose closing triggered this; it must be deleted
// first.
void DestroyIfUnused(base::FilePath path,
                     Profile::OTRProfileID id,
                     base::WeakPtr<BrowserWindowInterface> closed_browser,
                     int waits_left) {
  if (closed_browser) {
    if (waits_left > 0) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&DestroyIfUnused, std::move(path), id,
                                    std::move(closed_browser), waits_left - 1));
    }
    return;
  }
  Profile* original = ProfileByPath(path);
  if (!original || !original->HasOffTheRecordProfile(id)) {
    return;
  }
  Profile* moltnet = original->GetOffTheRecordProfile(id,
                                                      /*create_if_needed=*/false);
  if (!moltnet || !IsMoltNetProfile(moltnet) ||
      HasOpenBrowserWindow(moltnet)) {
    return;
  }
  MoltNetRouting::ForgetMoltNetProfile(moltnet);
#if BUILDFLAG(ENABLE_PRINT_PREVIEW)
  // As BrowserManagerService does for Incognito (only): the print-preview
  // contents of jobs that haven't finished spooling are this profile's, so
  // drop them first; otherwise their renderers can outlast the destroyer's
  // timeout below, which then frees the profile under them
  // (crbug.com/41235373).
  if (g_browser_process && g_browser_process->background_printing_manager()) {
    g_browser_process->background_printing_manager()
        ->DeletePreviewContentsForBrowserContext(moltnet);
  }
#endif
  // Waits for its renderers to go away, then frees the in-memory cookies,
  // cache and storage.
  ProfileDestroyer::DestroyOTRProfileWhenAppropriate(moltnet);
}

void PostDestroyIfUnused(
    Profile* moltnet,
    base::WeakPtr<BrowserWindowInterface> closed_browser = nullptr) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&DestroyIfUnused,
                     moltnet->GetOriginalProfile()->GetPath(),
                     moltnet->GetOTRProfileID(), std::move(closed_browser),
                     kMaxWaitsForClosedBrowser));
}

// Non-primary OTR profiles are NOT destroyed by Chromium when their last
// window closes (BrowserManagerService only does that for incognito), so
// MoltNet does it: closing the last MoltNet window ends that identity.
class MoltNetProfileReaper : public BrowserCollectionObserver {
 public:
  MoltNetProfileReaper() {
    observation_.Observe(GlobalBrowserCollection::GetInstance());
  }

  // BrowserCollectionObserver:
  void OnBrowserClosed(BrowserWindowInterface* browser) override {
    if (browser && IsMoltNetProfile(browser->GetProfile())) {
      PostDestroyIfUnused(browser->GetProfile(), browser->GetWeakPtr());
    }
  }

 private:
  base::ScopedObservation<GlobalBrowserCollection, BrowserCollectionObserver>
      observation_{this};
};

void EnsureReaper() {
  [[maybe_unused]] static base::NoDestructor<MoltNetProfileReaper> reaper;
}

// Closes MoltNet windows; |original| == nullptr means every profile.
void CloseMoltNetWindows(Profile* original) {
  for (BrowserWindowInterface* window : GetAllBrowserWindowInterfaces()) {
    Profile* profile = window->GetProfile();
    if (!IsMoltNetProfile(profile)) {
      continue;
    }
    if (original && profile->GetOriginalProfile() != original) {
      continue;
    }
    window->GetWindow()->Close();
  }
}

// New MoltNet windows open at a size whose page area is exactly one
// letterbox size, the largest that fits on the screen (1400 x 900 DIPs
// where there is room), so they start without margins
// (docs/FINGERPRINTING.md, "Letterboxing"). Only while the page is
// letterboxed, i.e. while fingerprinting protection is on.
bool OpensAtLetterboxSize() {
  return fingerprinting::IsFingerprintingProtectionEnabled();
}

// How often FitPageToLetterbox() looks again for a window whose page area
// hasn't been laid out yet.
constexpr int kMaxWaitsForLayout = 10;

// Resizes `weak_browser`'s window so that its page area (the active tab's
// contents) is WindowLetterboxSize(): the browser UI around the page keeps
// its size, and the window stays on its screen. Leaves maximized,
// minimized and fullscreen windows alone (the user, the window manager or
// --start-maximized chose those; the margins then do the rest).
void FitPageToLetterbox(base::WeakPtr<BrowserWindowInterface> weak_browser,
                        int waits_left) {
  BrowserWindowInterface* const browser = weak_browser.get();
  BrowserWindow* const window =
      browser ? BrowserWindow::FromBrowser(browser) : nullptr;
  display::Screen* const screen = display::Screen::Get();
  if (!window || !screen || window->IsMaximized() || window->IsMinimized() ||
      window->IsFullscreen()) {
    return;
  }
  const gfx::Size page = window->GetContentsSize();
  if (page.IsEmpty()) {
    if (waits_left > 0) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(&FitPageToLetterbox,
                                    std::move(weak_browser), waits_left - 1));
    }
    return;
  }
  const gfx::Rect bounds = window->GetBounds();
  const gfx::Rect work_area =
      screen->GetDisplayNearestWindow(window->GetNativeWindow()).work_area();
  const fingerprinting::LetterboxSize target =
      fingerprinting::WindowLetterboxSize(
          bounds.width(), bounds.height(), page.width(), page.height(),
          work_area.width(), work_area.height());
  if (target.IsEmpty()) {
    return;
  }
  // Grows or shrinks the window by the difference, then moves it back onto
  // the work area if it now sticks out.
  window->SetContentsSize(gfx::Size(target.width, target.height));
}

void OpenVerifiedWindow(base::FilePath path,
                        GURL url,
                        MoltNetWindow::DoneCallback done,
                        bool ok,
                        std::string error) {
  if (!ok) {
    if (Profile* moltnet = MoltNetRouting::GetMoltNetProfile(
            ProfileByPath(path))) {
      PostDestroyIfUnused(moltnet);
    }
    std::move(done).Run(false, error);
    return;
  }
  Profile* moltnet = MoltNetRouting::GetMoltNetProfile(ProfileByPath(path));
  if (!moltnet || Browser::GetCreationStatusForProfile(moltnet) !=
                      Browser::CreationStatus::kOk) {
    std::move(done).Run(false, "Could not open a MoltNet window.");
    return;
  }
  EnsureReaper();
  Browser::CreateParams params(Browser::TYPE_NORMAL, moltnet,
                               /*user_gesture=*/true);
  const bool letterbox_size = OpensAtLetterboxSize();
  if (letterbox_size) {
    // Not maximized like the last window, which would give a wide screen
    // margins: restored, at the size FitPageToLetterbox() picks.
    params.initial_show_state = ui::mojom::WindowShowState::kNormal;
  }
  Browser* browser = Browser::Create(params);
  const GURL target = url.is_valid() && url.SchemeIsHTTPOrHTTPS() ? url
                                                                   : GURL();
  // GURL() opens the new-tab page.
  chrome::AddTabAt(browser, target, /*index=*/-1, /*foreground=*/true);
  if (letterbox_size) {
    // Before showing, if the window is laid out already (no visible
    // resize); otherwise as soon as it is.
    FitPageToLetterbox(browser->GetWeakPtr(), kMaxWaitsForLayout);
  }
  browser->GetWindow()->Show();
  std::move(done).Run(true, std::string());
}

void OnTorReady(base::FilePath path,
                GURL url,
                MoltNetWindow::DoneCallback done,
                TorLaunchResult result) {
  if (!result.success) {
    // Fail closed: no window, nothing falls back to a direct connection.
    std::move(done).Run(false, result.error.empty()
                                   ? std::string("Tor could not start.")
                                   : result.error);
    return;
  }
  Profile* original = ProfileByPath(path);
  if (!original) {
    std::move(done).Run(false, "The profile went away.");
    return;
  }
  const uint16_t socks_port = TorManager::Get()->socks_port();
  std::string error;
  Profile* moltnet =
      MoltNetRouting::GetOrCreateMoltNetProfile(original, socks_port, &error);
  if (!moltnet) {
    if (Profile* existing = MoltNetRouting::GetMoltNetProfile(original)) {
      PostDestroyIfUnused(existing);
    }
    std::move(done).Run(false, error);
    return;
  }
  MoltNetRouting::VerifyRouting(
      moltnet, socks_port,
      base::BindOnce(&OpenVerifiedWindow, std::move(path), std::move(url),
                     std::move(done)));
}

}  // namespace

// static
void MoltNetWindow::Open(Profile* from, const GURL& url, DoneCallback done) {
  if (!from) {
    std::move(done).Run(false, "No profile.");
    return;
  }
  TorManager::Get()->Launch(base::BindOnce(
      &OnTorReady, from->GetOriginalProfile()->GetPath(), url,
      std::move(done)));
}

// static
void MoltNetWindow::OpenForUserClick(content::WebContents* source,
                                     const GURL& url,
                                     DoneCallback done) {
  if (!source) {
    if (done) {
      std::move(done).Run(false, "No window.");
    }
    return;
  }
  Open(Profile::FromBrowserContext(source->GetBrowserContext()), url,
       base::BindOnce(
           [](base::WeakPtr<content::WebContents> weak_source,
              DoneCallback then, bool ok, std::string error) {
             if (!ok && weak_source) {
               chrome::ShowWarningMessageBoxAsync(
                   weak_source->GetTopLevelNativeWindow(),
                   u"Couldn't open a MoltNet window",
                   base::UTF8ToUTF16(
                       error.empty() ? std::string("MoltNet couldn't start.")
                                     : error));
             }
             if (then) {
               std::move(then).Run(ok, std::move(error));
             }
           },
           source->GetWeakPtr(), std::move(done)));
}

// static
void MoltNetWindow::CloseAll(Profile* from) {
  if (from) {
    CloseMoltNetWindows(from->GetOriginalProfile());
  }
}

// static
void MoltNetWindow::NewIdentity(Profile* from, DoneCallback done) {
  if (!from) {
    std::move(done).Run(false, "No profile.");
    return;
  }
  Profile* original = from->GetOriginalProfile();
  if (Profile* moltnet = MoltNetRouting::GetMoltNetProfile(original)) {
    // Drop live sockets + cached DNS first so nothing from the old identity
    // can be reused, then make sure the next window gets a NEW profile.
    MoltNetRouting::FlushNetworkState(moltnet);
    MoltNetRouting::ForgetMoltNetProfile(moltnet);
    CloseMoltNetWindows(original);
    // Covers the no-window case; with windows, the reaper also fires.
    PostDestroyIfUnused(moltnet);
  }
  TorManager::Get()->SendNewIdentitySignal(base::BindOnce(
      [](base::FilePath path, DoneCallback done, bool ok, std::string error) {
        if (!ok) {
          std::move(done).Run(false, "New identity failed: " + error);
          return;
        }
        Profile* profile = ProfileByPath(path);
        if (!profile) {
          std::move(done).Run(false, "The profile went away.");
          return;
        }
        MoltNetWindow::Open(profile, GURL(), std::move(done));
      },
      original->GetPath(), std::move(done)));
}

// static
void MoltNetWindow::TurnOff() {
  CloseMoltNetWindows(nullptr);
  TorManager::Get()->Stop();
}

}  // namespace tor
}  // namespace molt_ai
