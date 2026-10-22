# Security Policy

MoltBrowser is a privacy and security product, so we treat vulnerability
reports as the highest-priority work in the project.

## Reporting a vulnerability

**Please do not open a public GitHub issue for security problems.**

Report privately through either channel:

1. **GitHub private vulnerability reporting** — the *Report a vulnerability*
   button on this repository's **Security** tab.
2. **Email** — `security@geneye.ai`.

Include what you can of:

- affected version (`molt://ai-settings` → About, or the release tag) and OS
- the component (MoltNet / Tor routing, MoltShield, AI chat, Agent mode,
  cloud keys, import, updater, or upstream Chromium)
- reproduction steps or a proof of concept
- the impact as you understand it

## What to expect

| Step | Target |
|------|--------|
| Acknowledge your report | 2 business days |
| Initial assessment and severity | 7 days |
| Fix for Critical / High issues | as fast as possible, normally within 30 days |
| Public advisory | when a fixed release ships, crediting you unless you prefer otherwise |

We follow coordinated disclosure: please give us a reasonable window to ship
a fix before publishing. We will keep you updated throughout.

## Supported versions

Only the **latest release** receives security fixes. MoltBrowser checks
GitHub Releases for updates and offers to install them; please stay on the
latest build, and only install MoltBrowser from this repository's
Releases page. From v0.3.0, MoltBrowser only installs updates whose installer
matches a release manifest signed with GenEye's offline release key
([how it works](docs/SIGNED_UPDATES.md)).

## Signing keys

MoltBrowser releases are signed with keys that are kept offline and never
touch CI or a cloud machine:

| Key | Signs | Where to check it |
|---|---|---|
| Release key (Ed25519) | `molt-release-manifest.json`, which the in-app updater requires | public keys compiled into the browser: `src/chrome/browser/molt_ai/update/update_signing_keys.cc` ([runbook](docs/SIGNED_UPDATES.md)) |
| Archive key (OpenPGP) | the apt and dnf/yum repositories and the repository's `.rpm` | fingerprint: *not created yet* ([docs/LINUX_REPOSITORIES.md](docs/LINUX_REPOSITORIES.md)) |

Every release also has an SPDX SBOM per installer ([docs/SBOM.md](docs/SBOM.md)),
SHA-256 checksums and a build-provenance attestation. If a signature or
checksum does not verify, don't install it, and tell us.

## Scope

**In scope** — anything in MoltBrowser's own code, for example:

- anonymity leaks while MoltNet (Tor) is on: real IP, DNS, WebRTC, QUIC,
  or any request that bypasses the Tor proxy
- fingerprinting or cross-site tracking that MoltShield or the
  fingerprinting protection claims to block but does not
  ([what it covers](docs/FINGERPRINTING.md))
- a web page opening a `molt://` page, or `.onion` names leaking outside
  MoltNet windows
- privileged `molt://` pages: XSS, unvalidated `chrome.send` handlers,
  path traversal, or anything a web page can reach
- prompt injection that makes the AI or Agent mode act without the user's
  consent, or leak page, chat, memory, or key data
- storage of API keys, imported passwords, vault, profile, or memory data
- the update channel, model downloads, and bundled binaries (tor, OCR,
  whisper)

**Upstream Chromium bugs** — please report them to the Chromium project
(<https://issues.chromium.org>). If a known upstream fix has not yet reached
MoltBrowser, tell us; shipping upstream security fixes is part of our scope.

**Out of scope** — attacks that need a compromised OS or physical access to
an unlocked machine, social engineering, denial of service by resource
exhaustion, and reports from automated scanners without a demonstrated
impact.

## Safe harbor

We will not pursue legal action against good-faith research that respects
user privacy, avoids data destruction and service disruption, and follows
this policy.
