// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_window.h"

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/molt_ai/fingerprinting/molt_letterbox.h"
#include "chrome/browser/molt_ai/tor/molt_net_live_guard.h"
#include "chrome/browser/molt_ai/tor/molt_net_live_policy.h"
#include "chrome/browser/molt_ai/tor/molt_net_parental_controls.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/tor/molt_net_routing.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/printing/background_printing_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_destroyer.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_collection_observer.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/simple_message_box.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/unload_controller.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_features.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/result_codes.h"
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

// ---- Ending an identity ----
//
// New Identity and the live guard end an identity the same way, in step
// with its network contexts (molt_net_identity_end_policy.h):
//   1. At once, its network contexts are sent the refusal of every request
//      (MoltNetRouting::EndIdentity(): a proxy configuration with no proxy
//      and no DIRECT, ERR_NO_SUPPORTED_PROXIES), its connections are
//      dropped, it stops being the current identity, and the browser
//      refuses its navigations (MoltNetEndedIdentityThrottle).
//   2. At once, its renderer processes are ended (StopPages()): no page,
//      frame or worker of it can make another request while the refusal is
//      on its way to the network service.
//   3. Its network contexts are asked until each confirms the refusal
//      (MoltNetRouting::WhenIdentityRefuses()), which then drops its
//      connections once more. The rest waits for that:
//   4. Every DevTools session on its pages and workers is detached, which
//      closes the DevTools windows attached to them, in any profile
//      (chrome://inspect from a regular window too), and ends DevTools'
//      interception of "Leave site?".
//   5. Every window of it (normal, popup, app, DevTools, picture-in-picture)
//      closes, whatever is open in it, as BrowserCloseManager closes
//      windows at logoff and WebAppUiManagerImpl::CloseAppWindows() closes
//      an app's: its UnloadController skips every warning and every
//      beforeunload and unload handler
//      (set_force_skip_warning_user_on_close()), so no "Leave site?"
//      (with or without DevTools), download warning or hung handler holds
//      it. Tab-modal dialogs and prompts (a JavaScript alert, an
//      authentication prompt, print preview, a file chooser, a download's
//      "Save as") go with their tabs.
//   6. If a window is still open kForceCloseGrace later, its renderer
//      processes are ended again, its tabs closed and the window closed
//      again, a few times over.
// Upstream's chrome::CloseAllBrowsersWithIncognitoProfile() runs every
// "Leave site?" again as soon as one DevTools window of the profile exists,
// and then waits for the user; MoltNet doesn't use it any more.

constexpr base::TimeDelta kForceCloseGrace = base::Seconds(2);
constexpr int kMaxForceCloseRounds = 5;

// The MoltNet profile |id| of the original profile at |path|, or null.
Profile* LookUpMoltNetProfile(const base::FilePath& path,
                              const Profile::OTRProfileID& id) {
  Profile* original = ProfileByPath(path);
  if (!original || !original->HasOffTheRecordProfile(id)) {
    return nullptr;
  }
  Profile* moltnet =
      original->GetOffTheRecordProfile(id, /*create_if_needed=*/false);
  return IsMoltNetProfile(moltnet) ? moltnet : nullptr;
}

// Detaches every DevTools session (DevTools windows of any profile, remote
// debugging clients) from |moltnet|'s pages and workers. A DevTools window
// whose target is detached closes (DevToolsWindow::InspectedContentsClosing).
void DetachDevTools(Profile* moltnet) {
  for (const scoped_refptr<content::DevToolsAgentHost>& host :
       content::DevToolsAgentHost::GetOrCreateAll()) {
    if (host->GetBrowserContext() == moltnet) {
      host->ForceDetachAllSessions();
    }
  }
}

// Every window of |moltnet|, of any type.
std::vector<BrowserWindowInterface*> WindowsOf(Profile* moltnet) {
  std::vector<BrowserWindowInterface*> windows;
  for (BrowserWindowInterface* window : GetAllBrowserWindowInterfaces()) {
    if (window->GetProfile() == moltnet) {
      windows.push_back(window);
    }
  }
  return windows;
}

// Step 2: every renderer process of |moltnet| ends now, so nothing in its
// pages runs, or sends anything, any more.
void StopPages(Profile* moltnet) {
  for (content::RenderProcessHost::iterator it =
           content::RenderProcessHost::AllHostsIterator();
       !it.IsAtEnd(); it.Advance()) {
    content::RenderProcessHost* host = it.GetCurrentValue();
    if (host->GetBrowserContext() == moltnet &&
        host->IsInitializedAndNotDead()) {
      host->Shutdown(content::RESULT_CODE_KILLED);
    }
  }
}

// Step 5: no warning, no beforeunload or unload handler, for any window.
void CloseWindowsSkippingHandlers(Profile* moltnet) {
  for (BrowserWindowInterface* window : WindowsOf(moltnet)) {
    if (UnloadController* unload = UnloadController::From(window)) {
      unload->set_force_skip_warning_user_on_close(true);
    }
    if (window->GetWindow()) {
      window->GetWindow()->Close();
    }
  }
}

// Step 6, for whatever is still open: the renderers go first, so nothing in
// a page runs any more, then the tabs, then the window.
void ForceCloseRemainingWindows(base::FilePath path,
                                Profile::OTRProfileID id,
                                int rounds_left) {
  Profile* moltnet = LookUpMoltNetProfile(path, id);
  if (!moltnet || !HasOpenBrowserWindow(moltnet)) {
    return;
  }
  LOG(WARNING) << "[MoltNet] a window of an ended identity is still open; "
                  "ending its renderer processes";
  MoltNetRouting::FlushNetworkState(moltnet);
  DetachDevTools(moltnet);
  StopPages(moltnet);
  for (BrowserWindowInterface* window : WindowsOf(moltnet)) {
    if (UnloadController* unload = UnloadController::From(window)) {
      unload->set_force_skip_warning_user_on_close(true);
    }
    if (TabStripModel* tabs = window->GetTabStripModel()) {
      tabs->CloseAllTabs();
    }
    if (window->GetWindow()) {
      window->GetWindow()->Close();
    }
  }
  if (rounds_left > 0) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&ForceCloseRemainingWindows, std::move(path), id,
                       rounds_left - 1),
        kForceCloseGrace);
  }
}

// Steps 4 and 5, once the refusal is in place (or its deadline passed),
// step 6 later if needed; the profile is destroyed once its windows are gone
// (the reaper, or PostDestroyIfUnused() here).
void CloseEndedWindows(base::FilePath path,
                       Profile::OTRProfileID id,
                       bool /*refusal_confirmed*/) {
  Profile* moltnet = LookUpMoltNetProfile(path, id);
  if (!moltnet) {
    return;
  }
  DetachDevTools(moltnet);
  CloseWindowsSkippingHandlers(moltnet);
  // Covers the no-window case; with windows, the reaper also fires.
  PostDestroyIfUnused(moltnet);
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&ForceCloseRemainingWindows, std::move(path), id,
                     kMaxForceCloseRounds),
      kForceCloseGrace);
}

// Steps 1 to 3 now, the rest once the identity's network contexts confirm
// the refusal.
void EndIdentityAndCloseWindows(Profile* moltnet) {
  if (!IsMoltNetProfile(moltnet)) {
    return;
  }
  EnsureReaper();
  MoltNetRouting::EndIdentity(moltnet);
  StopPages(moltnet);
  MoltNetRouting::WhenIdentityRefuses(
      moltnet, base::BindOnce(&CloseEndedWindows,
                              moltnet->GetOriginalProfile()->GetPath(),
                              moltnet->GetOTRProfileID()));
}

// ---- A setting changed while MoltNet was open ----
//
// MoltNetLiveGuard (molt_net_live_guard.h) runs the open check again
// whenever one of the settings it reads changes (a policy's proxy or
// override rules, a leak control, blob URL partitioning, Incognito's
// availability). When it refuses, that identity ends as above, and the user
// is told why. MoltNet's network context never got the new proxy
// configuration (MoltNetRouting::ProxyConfigFor()), and from the end on it
// refuses every request, so nothing of that identity leaves outside Tor.

// Tells the user why the MoltNet windows of |original| closed, over one of
// its regular windows if there is one.
void ShowEndedMessage(Profile* original, const std::string& error) {
  gfx::NativeWindow parent = gfx::NativeWindow();
  for (BrowserWindowInterface* window : GetAllBrowserWindowInterfaces()) {
    if (window->GetProfile() == original && window->GetWindow()) {
      parent = window->GetWindow()->GetNativeWindow();
      break;
    }
  }
  chrome::ShowWarningMessageBoxAsync(parent,
                                     std::u16string(kMoltNetEndedTitle),
                                     base::UTF8ToUTF16(error));
}

// Closes the windows of the MoltNet profile |id| of the original profile at
// |path| whatever is open in them (EndIdentityAndCloseWindows()) and
// destroys the profile once they are gone.
void CloseEndedIdentity(base::FilePath path,
                        Profile::OTRProfileID id,
                        bool tell_user,
                        std::string error) {
  Profile* original = ProfileByPath(path);
  if (!original) {
    return;
  }
  if (Profile* moltnet = LookUpMoltNetProfile(path, id)) {
    EndIdentityAndCloseWindows(moltnet);
  }
  if (tell_user) {
    ShowEndedMessage(original, error);
  }
}

// MoltNetLiveGuard's callback. Runs inside the settings-change
// notification, so only the parts that destroy nothing happen here: the
// identity's network contexts refuse every request from now on, its
// connections are dropped and it stops being the current one (no window
// opens in it any more). The windows close, and the profile is destroyed,
// in a task of their own.
void EndForChangedSettings(Profile* moltnet, const std::string& error) {
  if (!IsMoltNetProfile(moltnet)) {
    return;
  }
  MoltNetRouting::EndIdentity(moltnet);
  // Only an identity the user had windows of needs explaining; one that
  // was still opening reports the error to whatever opened it.
  const bool tell_user = HasOpenBrowserWindow(moltnet);
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&CloseEndedIdentity,
                     moltnet->GetOriginalProfile()->GetPath(),
                     moltnet->GetOTRProfileID(), tell_user, error));
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

// ---- New Identity ----
//
// The old identity must be gone before Tor is asked for new circuits, and
// Tor must have switched before the new identity sends anything. Every
// MoltNet profile uses the same SOCKS credentials for a given site
// (net/socket/socks_connect_job.cc), so a request of the old identity made
// after NEWNYM (a pagehide beacon, a keepalive fetch, a window a page kept
// open) would share the new identity's circuit and exit for that site, and
// a new-identity request made before Tor applied NEWNYM would share the old
// one's.

// How long New Identity waits for the old identity's windows to close and
// its profile to be destroyed. Normally about a second (ProfileDestroyer
// waits that long for renderers); nothing a page or DevTools does can hold
// a window open (EndIdentityAndCloseWindows()), so this only guards against
// the unexpected.
constexpr base::TimeDelta kRetireTimeout = base::Seconds(60);
constexpr base::TimeDelta kRetirePollInterval = base::Milliseconds(100);

// Original profiles (by path) whose New Identity is under way. No MoltNet
// window opens for them meanwhile, except New Identity's own.
std::set<base::FilePath>& ProfilesSwitchingIdentity() {
  static base::NoDestructor<std::set<base::FilePath>> paths;
  return *paths;
}

bool IsSwitchingIdentity(const base::FilePath& path) {
  return ProfilesSwitchingIdentity().count(path) > 0;
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
  const display::Display display =
      screen->GetDisplayNearestWindow(window->GetNativeWindow());
  const gfx::Rect work_area = display.work_area();
  // At the screen's scale factor (the one the page's renderer gets) and
  // 100% zoom, where MoltNet pages start.
  const fingerprinting::LetterboxSize target =
      fingerprinting::WindowLetterboxSize(
          bounds.width(), bounds.height(), page.width(), page.height(),
          work_area.width(), work_area.height(),
          {display.device_scale_factor(), 1.0f});
  if (target.IsEmpty()) {
    return;
  }
  // Grows or shrinks the window by the difference, then moves it back onto
  // the work area if it now sticks out.
  window->SetContentsSize(gfx::Size(target.width, target.height));
}

void OpenVerifiedWindow(base::FilePath path,
                        Profile::OTRProfileID id,
                        GURL url,
                        bool for_new_identity,
                        MoltNetWindow::DoneCallback done,
                        bool ok,
                        std::string error) {
  if (!for_new_identity && IsSwitchingIdentity(path)) {
    // New Identity started while this window was on its way; it would
    // belong to neither identity cleanly.
    std::move(done).Run(false,
                        "MoltNet is switching to a new identity. Try again "
                        "in a moment.");
    return;
  }
  // The MoltNet profile whose routing was just verified.
  Profile* original = ProfileByPath(path);
  Profile* moltnet =
      original && original->HasOffTheRecordProfile(id)
          ? original->GetOffTheRecordProfile(id, /*create_if_needed=*/false)
          : nullptr;
  if (!ok) {
    if (moltnet) {
      PostDestroyIfUnused(moltnet);
    }
    std::move(done).Run(false, error);
    return;
  }
  // A setting the check read may have changed while the routing check ran
  // (MoltNetLiveGuard has then ended this identity, or is about to).
  if (moltnet) {
    if (std::string settings_error =
            MoltNetRouting::SettingsError(moltnet, std::nullopt);
        !settings_error.empty()) {
      PostDestroyIfUnused(moltnet);
      std::move(done).Run(false, std::move(settings_error));
      return;
    }
  }
  if (!moltnet || MoltNetRouting::GetMoltNetProfile(original) != moltnet ||
      GetBrowserWindowCreationStatusForProfile(*moltnet) !=
          BrowserWindowInterface::CreationStatus::kOk) {
    std::move(done).Run(false, "Could not open a MoltNet window.");
    return;
  }
  EnsureReaper();
  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_NORMAL,
                                   *moltnet, /*from_user_gesture=*/true);
  const bool letterbox_size = OpensAtLetterboxSize();
  if (letterbox_size) {
    // Not maximized like the last window, which would give a wide screen
    // margins: restored, at the size FitPageToLetterbox() picks.
    params.initial_show_state = ui::mojom::WindowShowState::kNormal;
  }
  BrowserWindowInterface* browser = CreateBrowserWindow(std::move(params));
  if (!browser) {
    PostDestroyIfUnused(moltnet);
    std::move(done).Run(false, "Could not open a MoltNet window.");
    return;
  }
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
                bool for_new_identity,
                MoltNetWindow::DoneCallback done,
                TorLaunchResult result) {
  if (!for_new_identity && IsSwitchingIdentity(path)) {
    std::move(done).Run(false,
                        "MoltNet is switching to a new identity. Try again "
                        "in a moment.");
    return;
  }
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
  // From now until this identity ends, the settings just checked are
  // watched: if one changes so that this check would refuse, the identity
  // ends (molt_net_live_guard.h).
  MoltNetLiveGuard::Watch(moltnet, base::BindRepeating(&EndForChangedSettings));
  MoltNetRouting::VerifyRouting(
      moltnet, socks_port,
      base::BindOnce(&OpenVerifiedWindow, std::move(path),
                     moltnet->GetOTRProfileID(), std::move(url),
                     for_new_identity, std::move(done)));
}

void OpenImpl(Profile* from,
              const GURL& url,
              bool for_new_identity,
              MoltNetWindow::DoneCallback done) {
  if (!from) {
    std::move(done).Run(false, "No profile.");
    return;
  }
  const base::FilePath path = from->GetOriginalProfile()->GetPath();
  if (!for_new_identity && IsSwitchingIdentity(path)) {
    std::move(done).Run(false,
                        "MoltNet is switching to a new identity. Try again "
                        "in a moment.");
    return;
  }
  // Before Tor starts: there is nothing to wait for if sites can't be kept
  // apart, or if Incognito is off (GetOrCreateMoltNetProfile() checks
  // again).
  if (std::string error = MoltNetRouting::ProcessWidePartitioningError();
      !error.empty()) {
    std::move(done).Run(false, std::move(error));
    return;
  }
  if (std::string error = MoltNetRouting::IncognitoPolicyError(from);
      !error.empty()) {
    std::move(done).Run(false, std::move(error));
    return;
  }
  // Windows' parental controls as they are now, not as at startup
  // (molt_net_parental_controls.h).
  CheckMoltNetParentalControls(base::BindOnce(
      [](base::FilePath path, GURL url, bool for_new_identity,
         MoltNetWindow::DoneCallback done, bool allowed) {
        if (!allowed) {
          std::move(done).Run(
              false, MoltNetSettingsError(MoltNetSettingsCheck::kIncognitoOff));
          return;
        }
        TorManager::Get()->Launch(base::BindOnce(
            &OnTorReady, path, url, for_new_identity, std::move(done)));
      },
      path, url, for_new_identity, std::move(done)));
}

void FinishNewIdentity(base::FilePath path,
                       MoltNetWindow::DoneCallback done,
                       bool ok,
                       std::string error) {
  ProfilesSwitchingIdentity().erase(path);
  std::move(done).Run(ok, std::move(error));
}

void OnNewCircuits(base::FilePath path,
                   MoltNetWindow::DoneCallback done,
                   bool ok,
                   std::string error) {
  if (!ok) {
    FinishNewIdentity(std::move(path), std::move(done), false,
                      "New identity failed: " + error);
    return;
  }
  Profile* profile = ProfileByPath(path);
  if (!profile) {
    FinishNewIdentity(std::move(path), std::move(done), false,
                      "The profile went away.");
    return;
  }
  OpenImpl(profile, GURL(), /*for_new_identity=*/true,
           base::BindOnce(&FinishNewIdentity, path, std::move(done)));
}

// Waits until none of the MoltNet profiles `retiring` of the original
// profile at `path` exists any more, then asks Tor for new circuits.
void WaitForRetiredProfiles(base::FilePath path,
                            std::vector<Profile::OTRProfileID> retiring,
                            base::TimeTicks deadline,
                            MoltNetWindow::DoneCallback done) {
  Profile* original = ProfileByPath(path);
  if (!original) {
    FinishNewIdentity(std::move(path), std::move(done), false,
                      "The profile went away.");
    return;
  }
  // OTRProfileID can't be assigned (its id is const), so keep the ones still
  // open in a new vector instead of erasing in place.
  std::vector<Profile::OTRProfileID> still_open;
  for (const Profile::OTRProfileID& id : retiring) {
    if (original->HasOffTheRecordProfile(id)) {
      still_open.push_back(id);
    }
  }
  retiring = std::move(still_open);
  if (!retiring.empty()) {
    if (base::TimeTicks::Now() < deadline) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&WaitForRetiredProfiles, std::move(path),
                         std::move(retiring), deadline, std::move(done)),
          kRetirePollInterval);
      return;
    }
    // A window of the old identity stayed open, although it was closed
    // forcibly (EndIdentityAndCloseWindows()). Its network context refuses
    // every request, so nothing of it can be sent; don't open a second
    // identity next to it. The next Open() makes a new profile (fresh
    // circuits: its network context has a SOCKS password of its own).
    FinishNewIdentity(std::move(path), std::move(done), false,
                      "New Identity stopped because a MoltNet window didn't "
                      "close. Close it and try again.");
    return;
  }
  // The old identity's profile, and with it its network context and every
  // connection it had, is gone. Only now ask Tor for new circuits; the new
  // window opens once Tor confirms the switch.
  TorManager::Get()->SendNewIdentitySignal(
      base::BindOnce(&OnNewCircuits, std::move(path), std::move(done)));
}

}  // namespace

// static
void MoltNetWindow::Open(Profile* from, const GURL& url, DoneCallback done) {
  OpenImpl(from, url, /*for_new_identity=*/false, std::move(done));
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
  const base::FilePath path = original->GetPath();
  if (!ProfilesSwitchingIdentity().insert(path).second) {
    std::move(done).Run(false, "New Identity is already under way.");
    return;
  }
  EnsureReaper();
  // Every MoltNet profile of `original` ends now: the current one, and an
  // older one whose window is still open (normally there is none).
  std::vector<Profile::OTRProfileID> retiring;
  for (Profile* moltnet : original->GetAllOffTheRecordProfiles()) {
    if (!IsMoltNetProfile(moltnet)) {
      continue;
    }
    retiring.push_back(moltnet->GetOTRProfileID());
    // From now on its network contexts refuse every request, and the next
    // window gets a NEW profile; then every window of it closes, whatever
    // is open in it ("Leave site?", also with DevTools open).
    EndIdentityAndCloseWindows(moltnet);
  }
  WaitForRetiredProfiles(path, std::move(retiring),
                         base::TimeTicks::Now() + kRetireTimeout,
                         std::move(done));
}

// static
void MoltNetWindow::TurnOff() {
  CloseMoltNetWindows(nullptr);
  TorManager::Get()->Stop();
}

}  // namespace tor
}  // namespace molt_ai
