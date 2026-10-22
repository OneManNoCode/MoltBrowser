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
| `molt-release-manifest.json` | Version, tag, and the exact size and SHA-256 of every release asset |
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

The manifest lists **every** asset of the release, not just the installers:

- the per-platform `SHA256SUMS-*`;
- an SPDX SBOM per installer, `*.spdx.json` ([SBOM.md](SBOM.md));
- the signed Linux package repository, `MoltBrowser-Linux-repository.tar`
  ([LINUX_REPOSITORIES.md](LINUX_REPOSITORIES.md)).

The repository has its own offline key, the **archive key** (OpenPGP),
because apt and dnf can only check OpenPGP signatures. Its one-time setup
and per-release step are below.

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

## One-time: create the Linux archive key (offline)

apt and dnf check OpenPGP signatures, so the Linux repository
([LINUX_REPOSITORIES.md](LINUX_REPOSITORIES.md)) has its own key, the
**archive key**. It follows the same rules as the release key: it is created
and used only on the offline machine, never on a CI runner, a cloud machine
or in this repository.

1. **Tools (install while still online).**
   - A Linux live USB (Fedora or Debian) is easiest:
     ```bash
     sudo dnf install gnupg2 rpm-sign createrepo_c dpkg      # Fedora
     sudo apt install gnupg rpm createrepo-c dpkg-dev        # Debian/Ubuntu
     ```
   - On a Mac, use Homebrew: `brew install gnupg rpm createrepo_c dpkg`.
   - Copy `tools/linux_repo/` and `tools/sbom/` next to
     `tools/release_signing/`.
2. **Disconnect from the network.** Keep the GnuPG home on the encrypted USB
   drive:
   ```bash
   export GNUPGHOME=/Volumes/MOLTKEY/gnupg
   mkdir -m 700 "$GNUPGHOME"
   ```
3. **Create the primary key.** It only certifies (`cert`), never signs
   packages, and expires in 5 years:
   ```bash
   gpg --quick-generate-key \
     'MoltBrowser Archive Key (GenEye AI Labs) <security@geneye.ai>' rsa4096 cert 5y
   gpg --list-keys --with-fingerprint     # note the 40-hex fingerprint
   ```
   - Use a strong passphrase. Keep it in your password manager, not on the
     drive.
   - RSA-4096 rather than Ed25519, because older rpm versions (RHEL 8's)
     can't check Ed25519 signatures.
4. **Add the signing subkey** (expires in 2 years):
   ```bash
   gpg --quick-add-key <FINGERPRINT> rsa4096 sign 2y
   ```
   Signing with the primary fingerprint uses this subkey automatically.
   Users trust the primary key, so extending the subkey's expiry (see
   "Linux archive key: expiry, loss, compromise") never asks them to trust
   a new key.
5. **Keep the revocation certificate.** GnuPG wrote it when it created the
   key, in `$GNUPGHOME/openpgp-revocs.d/<FINGERPRINT>.rev`.
   - Print it, and copy it to a second, separate medium.
   - Anyone holding it can revoke the key, so store it like the key itself.
   - If the key is lost, it is the only way to tell users to stop trusting
     it.
6. **Back up** `$GNUPGHOME` to a second encrypted USB drive, stored
   somewhere physically separate, as for the release key.
7. **Publish the public key.**
   ```bash
   gpg --armor --export --export-options export-minimal <FINGERPRINT> \
     > moltbrowser-archive-keyring.asc
   ```
   - Carry that file to a dev machine. Commit it as
     `installer/linux/moltbrowser-archive-keyring.asc` in a PR.
   - Until it is committed, nothing publishes a Linux repository:
     `linux-repo-publish.sh` refuses and the Pages deploy skips it.
   - Publish the fingerprint in several separate places:
     - the table in `docs/LINUX_REPOSITORIES.md`;
     - `SECURITY.md`;
     - the website's download page;
     - every release's notes.

The archive key's secret parts must never be copied to an online machine.
`.gitignore` blocks `*.key`; the GnuPG home has no business in this
repository at all.

## Every release

1. **Build and upload as a draft.** Build as usual and create the GitHub
   release as a **draft**, with all installers attached. The release jobs
   also attach an SBOM per installer and, for Linux, the **unsigned**
   `MoltBrowser-Linux-repository.tar`.
2. **Download the assets (online machine).**
   ```bash
   gh release download vX.Y.Z --repo OneManNoCode/MoltBrowser --dir release-vX.Y.Z
   ```
3. **Copy `release-vX.Y.Z/` to the offline machine** on a USB drive.
4. **Check the SBOMs (offline).** Each one must describe its installer,
   with the same SHA-256, and name the pinned Chromium:
   ```bash
   python3 tools/sbom/molt_sbom.py validate \
     --artifacts-dir release-vX.Y.Z release-vX.Y.Z/*.spdx.json
   ```
5. **Sign the Linux repository (offline).** Skip this step if the release
   has no Linux packages.
   ```bash
   export GNUPGHOME=/Volumes/MOLTKEY/gnupg GPG_TTY="$(tty)"
   python3 tools/linux_repo/molt_linux_repo.py sign \
     --bundle release-vX.Y.Z/MoltBrowser-Linux-repository.tar \
     --key <ARCHIVE KEY FINGERPRINT>
   ```
   - It takes the `.deb`/`.rpm` from the same folder, by SHA-256.
   - It checks that CI's `Packages`/`Release` match those packages.
   - It shows what it will sign; you type the version to confirm.
   - It then:
     - signs the `.rpm` with `rpmsign` (only the signature header changes;
       it checks that);
     - rebuilds the rpm metadata with `createrepo_c`;
     - signs `repomd.xml`, `InRelease` and `Release.gpg`;
     - verifies every signature with `gpgv`.
   - It **replaces the unsigned bundle in place** with the signed one. Do
     this before the next step, so the manifest covers the signed bundle.
6. **Write and sign the manifest (offline).**
   ```bash
   python3 tools/release_signing/molt_release_sign.py manifest \
     --tag vX.Y.Z release-vX.Y.Z/*
   python3 tools/release_signing/molt_release_sign.py sign \
     --key /Volumes/MOLTKEY/molt-update-signing.key
   ```
   - `sign` shows every asset and its hash.
   - You must type the version to confirm before it signs.
7. **Copy back** to the online machine:
   - `molt-release-manifest.json` and `.sig`;
   - the signed `MoltBrowser-Linux-repository.tar`.
8. **Upload and publish (online).**
   ```bash
   scripts/linux-repo-publish.sh --bundle MoltBrowser-Linux-repository.tar --tag vX.Y.Z
   scripts/publish-release-signature.sh vX.Y.Z molt-release-manifest.json molt-release-manifest.json.sig
   gh release edit vX.Y.Z --repo OneManNoCode/MoltBrowser --draft=false
   ```
   - `linux-repo-publish.sh` checks the bundle against the archive key
     committed in `installer/linux/`, then replaces CI's unsigned bundle on
     the draft.
   - `publish-release-signature.sh` refuses to upload unless the signature
     verifies against the keys in this checkout.
   - It also refuses unless the release's assets are exactly the signed ones.
9. **Check the workflow runs are green:**
   - "Verify release signature";
   - "Deploy website to GitHub Pages". Publishing the release redeploys the
     site, which serves the repository once its signatures verify.

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

## Linux archive key: expiry, loss, compromise

- **Before it expires.** Extend the expiry at least 3 months ahead, offline:
  ```bash
  gpg --quick-set-expire <FINGERPRINT> 5y          # the primary key
  gpg --quick-set-expire <FINGERPRINT> 2y '*'      # its subkeys
  ```
  - Export and commit the public key again (step 7 above). The next signed
    bundle carries it too.
  - The fingerprint doesn't change, but users keep a copy of the key. They
    must download it again, or apt reports `EXPKEYSIG` once the old
    expiry passes. Announce it with the release before the old date.
- **Renew rather than rotate the signing subkey.** gpg always signs with the
  newest signing subkey. A new subkey would therefore sign the very next
  release, before users have it. If you must add one, publish the updated
  public key one release ahead and keep signing with the old subkey:
  `--key <OLD SUBKEY FINGERPRINT>!` (the `!` forces that subkey). The tools
  only accept primary fingerprints, so do that signing by hand.
- **Lost (no backup):**
  1. Publish the revocation certificate. Import it into a copy of the
     public key, export, commit and announce.
  2. Create a new key.
  3. Users must trust the new key by hand. The in-app update channel,
     signed with the release key, can tell them.
- **Stolen:**
  1. Treat it as a security incident (see `SECURITY.md`).
  2. Publish the revocation certificate the same way. apt and dnf then
     refuse everything it signed.
  3. Create a new key, and announce the new fingerprint everywhere the old
     one was published.

**Why no `Valid-Until`.** `Release` carries no `Valid-Until` by default.
With it, apt refuses metadata that hasn't been re-signed recently, which
stops a hostile mirror from serving an old, validly signed repository
forever (a "freeze" attack). But every re-signing is an offline session, and
a missed one would break updates for every user. The release cadence (each
Chrome security release within 7 days) and the in-app update check, which
reads the signed release manifest, make a long freeze visible. To use it,
CI builds with `molt_linux_repo.py build --valid-days N`, and the owner
re-signs before it expires.

## Transition note

v0.2.5 and earlier use the old updater, which does not check signatures.
They will install v0.3.0 like any previous update. From v0.3.0 onward
every update is verified.

## Testing

- `molt_ai_unittests --gtest_filter='ReleaseManifestTest.*'` covers signature,
  manifest and policy checks. Its vectors come from the Python tool with a
  public test key.
- `python3 tools/linux_repo/molt_linux_repo_test.py` runs the whole Linux
  repository flow with a throwaway OpenPGP key it creates and deletes. It
  checks the result with `gpgv`, `rpmkeys` and `apt-get update`, and checks
  that tampering is caught.
- `python3 tools/sbom/molt_sbom_test.py` checks the SBOM generator against
  fixtures and the official SPDX validator.
- `python3 tools/release_signing/molt_release_sign.py verify --help` lists
  the options for checking any release by hand.
