// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's promise: whatever a MoltNet window causes goes through MoltNet's
// own, Tor-proxied network context, or doesn't go out at all.
//
// Most of what a MoltNet page, tab or chat does already uses that context:
// navigations, subresources, workers, downloads and "Save page as",
// favicons, certificate (AIA) fetches (each on a one-off network key,
// molt_net_network_context_policy.h), payment manifests, Lens, the chat's
// /pdf fetch and model downloads
// (docs/MOLTNET_COMPLIANCE.md §2, "Requests outside Tor"). This file
// decides the rest: browser features that, for a MoltNet window, would
// send something through another context (the system network context, the
// regular profile's, a service's own) or to a party that ties it to the
// user. For MoltNet each of them is off; every other profile keeps
// Chromium's and MoltBrowser's behaviour.
//
// The only input is whether a profile is MoltNet's (IsMoltNetProfile()).
// Standard library only and header-only, so molt_ai_unittests tests it and
// the upstream hooks that ask it (media_router_feature.cc,
// chrome_authenticator_request_delegate.cc) need no new link dependency.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_REQUESTS_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_REQUESTS_POLICY_H_

namespace molt_ai::tor {

// What a profile's windows may use. Each field is true for every profile
// except MoltNet's.
struct MoltNetRequestAccess {
  // MoltBrowser's AI chat (side panel, molt://ai) sending a prompt, with
  // the page text, PDF, attachment and conversation it carries, to a cloud
  // AI provider the user connected (OpenAI, Anthropic, Gemini, a custom
  // endpoint). The request would use the chat's own network context, but
  // it carries the user's API key, which names their account, so the
  // provider could tie every MoltNet page sent to it to that person.
  // On-device models need no network and stay available.
  bool cloud_ai = true;

  // Page watchers and saved plans made from the chat. They are written to
  // the shared workflow store and run later by the regular profile's
  // scheduler or Agent studio: the watched URL would be fetched again and
  // again outside Tor, with the regular profile's cookies.
  bool saved_workflows = true;

  // Safe Browsing: the full-hash request when a URL matches a local list,
  // download pings (the download URL, the pages that led to it, the file's
  // name and SHA-256) and password-reuse pings with the page URL. They go
  // through Safe Browsing's own network context, which an off-the-record
  // profile shares with its regular profile, proxy settings included.
  bool safe_browsing = true;

  // "Get image descriptions from Google" (screen readers): the URLs and
  // pixels of a page's images are sent through the regular profile's
  // network context (AccessibilityLabelsService is the original
  // profile's).
  bool image_descriptions = true;

  // navigator.geolocation: the browser asks the system's location service
  // (on Linux Google's, through the system network context, with the
  // Wi-Fi networks around the machine), outside Tor.
  bool geolocation = true;

  // Cast, the Presentation API and the Remote Playback API: device
  // discovery on the local network, and a Cast device loads what it is
  // given from the user's own network.
  bool cast = true;

  // WebAuthn with a phone (hybrid, caBLE) and Google Password Manager
  // passkeys: both reach their relay servers through the system network
  // context, and the phone connects from the user's own network. Security
  // keys and the platform's own authenticator stay available.
  bool relay_authenticators = true;

  // Shopping features (product info, price insights, discounts) on the
  // pages a tab shows. An off-the-record tab gets the regular profile's
  // ShoppingService, which looks the page's URL up with the regular
  // profile's optimization guide and commerce servers, through the regular
  // profile's network context. Chromium keeps the tab's own UI for them
  // out of Incognito only (IsIncognitoProfile()), not out of MoltNet.
  bool commerce = true;

  friend constexpr bool operator==(const MoltNetRequestAccess&,
                                   const MoltNetRequestAccess&) = default;
};

constexpr MoltNetRequestAccess MoltNetRequestAccessForProfile(
    bool is_moltnet_profile) {
  if (!is_moltnet_profile) {
    return MoltNetRequestAccess();
  }
  MoltNetRequestAccess access;
  access.cloud_ai = false;
  access.saved_workflows = false;
  access.safe_browsing = false;
  access.image_descriptions = false;
  access.geolocation = false;
  access.cast = false;
  access.relay_authenticators = false;
  access.commerce = false;
  return access;
}

// Where the chat's "sandbox tab" (/sandbox) opens. Elsewhere: an Incognito
// window. From MoltNet that would leave Tor, so it opens a MoltNet window,
// which is just as throwaway.
enum class SandboxTabWindow {
  kIncognito,
  kMoltNet,
};

constexpr SandboxTabWindow SandboxTabWindowFor(bool chat_in_moltnet) {
  return chat_in_moltnet ? SandboxTabWindow::kMoltNet
                         : SandboxTabWindow::kIncognito;
}

// Whether a chat may act in a window: navigate or click or type in its
// active tab, or open a link from a reply there. The chat's window first,
// but a side panel can fall back to the last active window. Never across
// MoltNet's boundary, either way: a MoltNet chat's actions and links come
// from MoltNet pages and would run outside Tor in a regular window, and a
// regular chat driving a MoltNet tab would carry that tab's content back
// to a chat that may use cloud AI. `same_profile` is whether the chat and
// the window belong to the same profile; inconsistent input (the same
// profile, but only one side MoltNet's) is refused.
constexpr bool MayChatActInWindow(bool chat_in_moltnet,
                                  bool window_in_moltnet,
                                  bool same_profile) {
  if (!chat_in_moltnet && !window_in_moltnet) {
    return true;
  }
  return same_profile && chat_in_moltnet == window_in_moltnet;
}

// What the chat says when MoltNet refuses. Plain text; the page escapes it.
inline constexpr char kCloudAiRefusedInMoltNet[] =
    "Cloud AI is off in MoltNet windows. Use an on-device model.";
inline constexpr char kSavedWorkflowRefusedInMoltNet[] =
    "Watchers and saved plans aren't available in MoltNet windows: they "
    "would run later outside Tor.";
inline constexpr char kOtherWindowRefusedInMoltNet[] =
    "MoltNet and regular windows are kept apart. Use the chat in that "
    "window.";

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_REQUESTS_POLICY_H_
