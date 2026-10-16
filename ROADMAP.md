# MoltBrowser Roadmap

MoltBrowser's goal: **the most private and most capable open-source
browser**. It should have the security of a current Chromium, Tor-grade
anonymity when you ask for it, and an AI that runs on your own machine and
works for you, not for an ad network.

This roadmap is public so you can see what we're building, hold us to it,
and pick something to help with. Status is updated as work lands.

**Status key:** ✅ done · 🧪 code complete, in release testing for v0.3 · 🔨 in progress · 📋 planned

---

## Principles

1. **Security before features.** A browser that is behind on engine
   security fixes is not private, whatever else it does. Engine currency,
   signed updates and sandboxing come first.
2. **Private by default.** Protections are on out of the box, not buried
   in settings.
3. **Local-first AI.** On-device models are the default. Cloud models are
   opt-in, clearly labeled, and use your own key.
4. **Honest claims.** We only call a feature done when it works, and we
   say plainly what it does not do yet.
5. **The agent works for you.** Web content is never allowed to give the
   AI instructions. Sensitive actions always ask first.

---

## Now — security foundation (next release, v0.3)

| | Item |
|---|---|
| 🔨 | **Current Chromium engine.** Move from the current development snapshot to the latest stable / Extended Stable Chromium, and adopt a patch series so every Chrome security release ships in MoltBrowser within days. *The patch series and rebase tooling are done ([REBASING.md](docs/REBASING.md)); the rebase onto Extended Stable 152 is next, on a build machine.* |
| 🧪 | **Signed updates.** Verify every downloaded update against a pinned signing key and the OS code-signing check before installing. |
| 🧪 | **AI chat rendering hardened** against markup injection from model output, with a fuzzing regression test in CI. |
| 🧪 | **Locked-down internal pages.** No inline script, Trusted Types enforced, and secrets never handed to the chat page. |
| 🧪 | **Agent guardrails.** Web pages can never instruct the agent. Navigation is limited to http(s) and to the sites you approve, and the agent asks before submitting forms, crossing to another site, or touching credentials. |
| 🧪 | **Vault-backed secrets in Agent mode.** Recorded workflows reference your encrypted vault instead of storing what you typed. |
| 🧪 | **Smooth automations.** AI-assisted workflow steps run off the UI thread, so the browser never pauses during a run. |
| 🧪 | **MoltNet in its own window.** Tor browsing gets a separate, throwaway profile (its own cookies, storage and cache) that never falls back to a direct connection. Requests no longer carry a MoltBrowser brand. |
| 🧪 | **MoltShield no longer blocks sites you navigate to on purpose.** |
| ✅ | **Working PR checks** that run without a Chromium checkout. |
| ✅ | **Security policy** with a private disclosure channel ([SECURITY.md](SECURITY.md)). |

## Next — real privacy (30–60 days)

| | Item |
|---|---|
| 🧪 | **MoltShield on adblock-rust** (Brave's engine, MPL-2.0). Blocks every request type, uses the EasyList / EasyPrivacy / uBlock filter lists with signed updates, cosmetic filtering and scriptlets, a per-site Shields toggle, and blocked counts. |
| 🧪 | **Private-by-default settings.** Third-party cookies blocked, HTTPS-First, Global Privacy Control, tracking-parameter stripping, no network prediction. *(Global Privacy Control comes with the Chromium 152 rebase.)* |
| 🧪 | **Fingerprinting protection** in the engine, not in page scripts: per-site, per-session randomization (Brave-style) in normal windows, and uniformity (UTC, en-US, letterboxing) in MoltNet windows. *v1 in v0.3: canvas, WebGL, audio and CPU-count farbling; MoltNet uniformity for time zone, language and screen size. Still to do: fonts, real window letterboxing, WebGPU and video-frame readback, a per-site off switch. Details and known limits: [FINGERPRINTING.md](docs/FINGERPRINTING.md).* |
| 🔨 | **Tor-grade MoltNet.** A real "New identity", a separate Tor circuit per site, HTTPS-Only, bridges (snowflake / obfs4) for censored networks, a verified Tor binary, and `.onion` support. *In v0.3: real New Identity, per-site circuits, HTTPS-Only, bridges, `.onion` support with Onion-Location, and the system `tor` on Linux. Still to do: pinned Tor binary hashes, bridges with the system `tor`, and treating `.onion` as a secure context.* |
| 🔨 | **De-Googled background traffic.** Audit and remove remaining Google service calls, and add phishing / malware protection that doesn't send your browsing to Google. *Source audit done ([NETWORK_TRAFFIC.md](docs/NETWORK_TRAFFIC.md)) and the unneeded calls are off in v0.3. Next: a traffic capture on a real build, and the Safe Browsing decision ([memo](docs/SAFE_BROWSING_DECISION.md)).* |
| 🧪 | **Better on-device AI.** Context windows sized to the model, faster multi-turn chat, reasoning models that show only the answer, and a refreshed model catalog by RAM tier with verified downloads. *llama.cpp is updated to b11641, with Gemma 4 support.* |
| 🧪 | **AI memory you control.** Opt-in with a pause switch and retention setting, never records MoltNet browsing, and encrypted at rest. |
| 🧪 | **Clear cloud labeling.** Every message sent to a cloud model is marked, and you see exactly what page or tab context goes with it. |

## Later — leapfrog (this quarter and beyond)

| | Item |
|---|---|
| 📋 | **Sandboxed AI.** On-device inference and document parsing move into sandboxed utility processes. |
| 📋 | **Web-standard AI APIs, without Google.** The Prompt, Summarizer, Writer, Rewriter and Translator APIs, backed by the open model of your choice and running fully offline. |
| 📋 | **A private agent.** An on-device planner combined with record/replay, screenshot understanding (vision models), grammar-constrained actions and WebMCP site tools. |
| 📋 | **Semantic history search** with real on-device embeddings. |
| 📋 | **On-device translation.** |
| 📋 | **GPU inference on Windows and Linux** (Vulkan), with automatic CPU fallback. |
| 🔨 | **Signed builds everywhere.** A signed Windows v0.3, signed apt/rpm repositories, published checksums, SBOM and build provenance. *Checksums and build provenance are done; Windows signing and apt/rpm repositories are next.* |
| 📋 | **Android**, built from Chromium for Android with MoltBrowser's features. |
| 📋 | **Independent security audit and bug bounty** before 1.0. |

---

## How to help

- **Report security issues privately:** see [SECURITY.md](SECURITY.md).
- **Pick an item:** open an issue saying which roadmap item you'd like to
  take, and we'll help you scope it. Items that need no Chromium build
  (for example the WebUI pages in
  `src/chrome/browser/ui/webui/molt_ai/` and the checks in
  `tools/webui_checks/`) are a good place to start.
- **Test privacy claims:** if MoltBrowser leaks something we say it
  protects, tell us. That's a security report, and we'll credit you.
