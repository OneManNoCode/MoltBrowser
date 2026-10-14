# Signed updates — runbook

MoltBrowser installs an update only when the installer matches a **release
manifest signed with GenEye's offline release key**. GitHub is just the
delivery channel. A leaked GitHub token, a compromised CI runner or a
tampered release asset cannot push code to users, because none of them
has the key.

This page is for whoever owns releases.

## How it works

Every release carries two extra assets next to the installers:

| Asset | What it is |
|---|---|
| `molt-release-manifest.json` | Version, tag, and the exact size and SHA-256 of every installer |
| `molt-release-manifest.json.sig` | Ed25519 signature over the manifest, made offline |

When a browser finds a newer release, it:

1. Downloads the manifest and signature.
2. Verifies the signature against the public keys compiled into the build,
   in `src/chrome/browser/molt_ai/update/update_signing_keys.cc`.
3. Checks that the manifest is for that exact release tag and is newer than
   the running version. This stops downgrades and replays of old manifests.
4. Downloads the installer, accepting no more bytes than the manifest lists,
   into a fresh private folder. It then checks the size and SHA-256.
5. At install time, the helper checks the SHA-256 once more. On macOS it also
   requires a valid code signature from GenEye's Developer ID team
   (83DA5U8N7Z).

A build with no trusted key never installs updates. It sends users to the
release page instead.

Independently, the **Verify release signature** workflow re-checks every
published release on GitHub. If it fails, users' browsers will refuse that
update.

## One-time: create the release key (offline)

Do this once, before building the first signed release (v0.3.0). The build
must already contain the public key.

1. **Prepare an offline machine.** Any laptop you can keep off the network
   while signing works. A dedicated old laptop or a live-USB OS is best.
   Install Python 3 and the verifier library while still online:
   `python3 -m pip install cryptography`. Then copy this repository's
   `tools/release_signing/` folder onto it.
2. **Disconnect from the network.** Plug in an encrypted USB drive (for
   example an APFS-encrypted or LUKS volume).
3. **Generate the key:**
   ```bash
   python3 tools/release_signing/molt_release_sign.py keygen \
     --out /Volumes/MOLTKEY/molt-update-signing.key
   ```
   - Pick a strong passphrase (12+ characters) and store it in your password
     manager, not on the USB drive.
   - The tool prints the **public key** and a key id.
4. **Back up the key file** to a second encrypted USB drive and store it
   somewhere physically separate.
   - If the key is lost, installed browsers can never auto-update again.
     Users would have to reinstall by hand.
5. **Trust the public key in the build.** The public key is not secret.
   Send it to engineering, or run this on a dev machine and open a PR:
   ```bash
   python3 tools/release_signing/molt_release_sign.py add-key <PUBLIC KEY>
   ```

The private key file must never be copied to an online machine, a CI
runner, a cloud drive or this repository. `.gitignore` blocks `*.key` as a
safety net.

## Every release

1. **Build and upload as a draft.** Build as usual and create the GitHub
   release as a **draft**, with all installers attached.
2. **Download the assets (online machine).**
   ```bash
   gh release download vX.Y.Z --repo OneManNoCode/MoltBrowser --dir release-vX.Y.Z
   ```
3. **Copy `release-vX.Y.Z/` to the offline machine** on a USB drive.
4. **Write and sign the manifest (offline).**
   ```bash
   python3 tools/release_signing/molt_release_sign.py manifest \
     --tag vX.Y.Z release-vX.Y.Z/*
   python3 tools/release_signing/molt_release_sign.py sign \
     --key /Volumes/MOLTKEY/molt-update-signing.key
   ```
   - `sign` shows every asset and its hash.
   - You must type the version to confirm before it signs.
5. **Copy `molt-release-manifest.json` and `.sig` back** to the online
   machine.
6. **Upload and publish (online).**
   ```bash
   scripts/publish-release-signature.sh vX.Y.Z molt-release-manifest.json molt-release-manifest.json.sig
   gh release edit vX.Y.Z --repo OneManNoCode/MoltBrowser --draft=false
   ```
   - The script refuses to upload unless the signature verifies against the
     keys in this checkout.
   - It also refuses unless the release's assets are exactly the signed ones.
7. **Check the "Verify release signature" workflow run is green.**

Never publish a release without its manifest and signature. Browsers on
v0.3.0 or later will not install it.

## Key rotation

1. Generate a new key offline, following the steps above.
2. Add it with `add-key`, keeping the old key listed, and ship a release that
   trusts **both** keys.
3. Once most users are on that release, sign releases with the new key.
4. A few releases later, remove the old key from `update_signing_keys.cc`.

## If the key is lost or stolen

- **Stolen:**
  1. Treat it as a security incident (see `SECURITY.md`).
  2. Immediately ship a release that removes the compromised key and adds a
     new one. Sign it with the old key one last time, so installed browsers
     accept it.
  3. Publish an advisory telling users to update.
- **Lost (no backup):**
  1. Generate a new key and ship a build that trusts it.
  2. Announce that users must download and install it manually once, because
     their installed browsers can't verify anything signed with the new key.

## Transition note

v0.2.5 and earlier use the old updater, which does not check signatures.
They will install v0.3.0 like any previous update. From v0.3.0 onward
every update is verified.

## Testing

- `molt_ai_unittests --gtest_filter='ReleaseManifestTest.*'` covers signature,
  manifest and policy checks. Its vectors come from the Python tool with a
  public test key.
- `python3 tools/release_signing/molt_release_sign.py verify --help` lists
  the options for checking any release by hand.
