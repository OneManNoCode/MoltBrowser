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

The same session also signs the release's **build provenance**,
`molt-build-provenance.intoto.jsonl`: what each installer was built from,
on which machine, with which toolchain. Browsers don't read it; people and
tools do ([Build provenance](#build-provenance)).

Independently, the **Verify release signature** workflow re-checks every
published release on GitHub, provenance included. If it fails, users'
browsers will refuse that update.

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

## One-time: label your build machines

Every release's build provenance names the machine that built each
installer by a **label** you choose, never by its hostname. The release
scripts and workflows refuse to start a release build without one.

1. **Pick a label per build machine,** e.g. `geneye-mac-1` or
   `geneye-win-1`.
   - Lower-case letters, digits, `.`, `_` and `-`; at most 64 characters.
   - It is published in every release. It must not be the machine's
     hostname or your user name (the tools refuse both), and should say
     nothing about who you are or where the machine is.
   - Keep the list of which label is which machine privately.
2. **Set it where releases are built.**
   - Release scripts: `export MOLT_BUILDER_ID=geneye-mac-1` in the shell
     profile of the build account.
   - A self-hosted Actions runner: add the line `MOLT_BUILDER_ID=geneye-win-1`
     to the `.env` file in the runner's folder, then restart the runner.
   - GitHub-hosted runners are labelled in their workflow
     (`release-windows-fast.yml`: `github-hosted-windows-16core`).
3. **A new or reinstalled machine gets a new label.** The label then always
   means one toolchain setup you can account for.

## Every release

1. **Build and upload as a draft.** Build as usual and create the GitHub
   release as a **draft**, with all installers attached. The release jobs
   also attach an SBOM per installer, a build provenance statement per
   build (`molt-provenance-<platform>-<arch>.json`) and, for Linux, the
   **unsigned** `MoltBrowser-Linux-repository.tar`.
   - Build from the tag, with a clean checkout. The scripts check both,
     and the label, before they start building.
   - If you compile first and release later with `--skip-build`, run
     `export MOLT_BUILD_STARTED_ON="$(scripts/release-provenance.sh --start --version X.Y.Z)"`
     before the compile, so the statement says when the build started.
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
6. **Write and sign the manifest and the build provenance (offline).**
   ```bash
   python3 tools/release_signing/molt_release_sign.py manifest \
     --tag vX.Y.Z release-vX.Y.Z/*
   python3 tools/release_signing/molt_release_sign.py sign \
     --key /Volumes/MOLTKEY/molt-update-signing.key --assets-dir release-vX.Y.Z
   ```
   - `sign` shows every asset and its hash, and for each build its commit,
     Chromium version, builder label and build times.
   - It refuses unless the provenance statements cover every installer,
     SBOM and checksum file, with the hashes the manifest lists, and all
     say the same commit and Chromium.
   - You must type the version to confirm before it signs.
   - It writes `molt-release-manifest.json.sig` and
     `molt-build-provenance.intoto.jsonl`, with one passphrase prompt.
7. **Copy back** to the online machine:
   - `molt-release-manifest.json` and `.sig`;
   - `molt-build-provenance.intoto.jsonl`;
   - the signed `MoltBrowser-Linux-repository.tar`.
8. **Upload and publish (online).**
   ```bash
   scripts/linux-repo-publish.sh --bundle MoltBrowser-Linux-repository.tar --tag vX.Y.Z
   scripts/publish-release-signature.sh vX.Y.Z molt-release-manifest.json \
     molt-release-manifest.json.sig molt-build-provenance.intoto.jsonl
   gh release edit vX.Y.Z --repo OneManNoCode/MoltBrowser --draft=false
   ```
   - `linux-repo-publish.sh` checks the bundle against the archive key
     committed in `installer/linux/`, then replaces CI's unsigned bundle on
     the draft.
   - `publish-release-signature.sh` refuses to upload unless the signature
     verifies against the keys in this checkout.
   - It also refuses unless the release's assets are exactly the signed ones.
   - It refuses without the signed build provenance, or if the provenance
     doesn't cover every build output, or says another commit than the
     one the tag points to.
9. **Check the workflow runs are green:**
   - "Verify release signature" (manifest and provenance);
   - "Deploy website to GitHub Pages". Publishing the release redeploys the
     site, which serves the repository once its signatures verify.

Never publish a release without its manifest, signature and build
provenance. Browsers on v0.3.0 or later will not install a release without
the first two.

## Build provenance

Each platform build writes a statement of what it built from what:
`molt-provenance-<platform>-<arch>.json`, an
[in-toto Statement v1](https://github.com/in-toto/attestation/blob/main/spec/v1/statement.md)
with a [SLSA Provenance v1](https://slsa.dev/spec/v1.0/provenance)
predicate. `scripts/release-provenance.sh` (which runs
`tools/release_signing/molt_provenance.py generate`) writes it after the
SBOMs. Given the same inputs it writes the same bytes.

In step 6 the owner signs every statement with the release key, into
`molt-build-provenance.intoto.jsonl`.

| Asset | What it is |
|---|---|
| `molt-provenance-<platform>-<arch>.json` | One build's statement, readable JSON. The manifest lists it like any asset. |
| `molt-build-provenance.intoto.jsonl` | All the statements, signed: one DSSE envelope per line (an in-toto bundle). Like the `.sig`, it is made in the signing step, so the manifest doesn't list it. |

### What a statement records

| Field | What it holds |
|---|---|
| `subject` | The build's installers, their SBOMs and its `SHA256SUMS-<platform>`, each with its SHA-256. |
| `externalParameters` | Tag and version, platform and arch, the `configs/` GN args file, and `releaseBuild`. |
| `internalParameters.gnArgs` | The effective `args.gn` of each out dir the build used (both for a universal Mac build), and whether it matches the `configs/` file. |
| `resolvedDependencies` | `moltbrowser`: the commit, at the tag. `chromium`: the commit of the checkout that was built (it must be the pin), at its version tag. `patches` and `src/third_party/rust` (the vendored Rust crates): git tree id and SHA-256 tree digest. The `configs/` file: git blob id and SHA-256. `model:<id>`: each bundled model's SHA-256 and pinned download. |
| `runDetails.builder` | `id`: `https://github.com/OneManNoCode/MoltBrowser/builders/<label>`. `version`: `clang`, `rustc`, and `xcode` and `macosSdk` on macOS or `msvc` and `windowsSdk` on Windows. `builderDependencies`: `depot_tools` at its commit, and for a build in Docker (`release-linux.sh --docker`, `release-win.sh --docker`) the container image id; its toolchain is then probed inside the container. |
| `runDetails.metadata` | `startedOn` and `finishedOn` (UTC), and the Actions run URL for a workflow build. |

Two release assets are not subjects:

- **The manifest**, because no build produces it: it is written in the
  signing session, from the assets, statements included. The manifest
  covers the statements and the statements cover the builds' outputs, so
  each binds the other.
- **The Linux repository bundle**, because it is re-signed offline. Its
  packages are subjects.

### Provenance format v1

This section is what the statements' `buildType`
(`…/docs/SIGNED_UPDATES.md#provenance-format-v1`) refers to.

- **Tree digests.** The `sha256` of a directory is the SHA-256 of a
  `sha256sum`-style listing of its committed files: one `<sha256>  <path>`
  line per file, the path relative to the directory, sorted bytewise. On a
  Linux checkout of the tag:
  ```bash
  (cd patches && find . -type f | sed 's|^\./||' | LC_ALL=C sort \
     | xargs -d '\n' sha256sum) | sha256sum
  git rev-parse vX.Y.Z:patches        # the gitTree
  ```
  It is computed from git's objects, so a Windows checkout with CRLF
  conversion gives the same digest.
- **GN args.** One entry per non-comment line, as `name = value`.
  - Paths are rewritten: the Chromium checkout becomes `<chromium-src>`,
    the repository `<repo>`, a home directory `~`.
  - An argument whose name looks like a key, secret, token, password or
    client id is `"<redacted>"` unless it is empty.
  - `matchesArgsFile` ignores `target_cpu` and `target_os`, which
    `build-universal.sh` adds per architecture.
- **Nothing personal.** `generate` refuses a statement that contains a
  home directory path, your user name in a path, the machine's hostname or
  anything that looks like a credential.
- **A release statement** (`releaseBuild: true`) needs everything above.
  `generate --release` refuses:
  - uncommitted changes (except under `docs/`, `website/` and `*.md`);
  - a HEAD that isn't the tag;
  - a Chromium checkout that isn't at the pin;
  - a missing `args.gn`, toolchain version or depot_tools revision;
  - a bundled model that doesn't match its pin.

### Why DSSE, not a detached signature

The manifest's signature is a detached `.sig` over
`MoltBrowser release manifest v1\n` plus the manifest. The provenance uses
a DSSE envelope instead:

- **Other tools can check it.** DSSE is the envelope in-toto and SLSA
  specify. Anyone with the public key can verify the bundle with standard
  in-toto tooling, not only with ours. CI checks every test bundle with
  securesystemslib's DSSE verifier, and every statement against the in-toto
  SLSA v1 schema.
- **One key can sign both safely.** DSSE signs
  `DSSEv1 28 application/vnd.in-toto+json <length> <statement>`. A
  manifest signature covers bytes that start `MoltBrowser`. Neither
  signature can be passed off as the other.
- **One self-contained file.** Each envelope carries its statement and its
  signature, so there is nothing to pair up. The `keyid` is the same
  16-hex key id as the `.sig`'s.

### Why the browser doesn't require it

The in-app updater doesn't read the provenance, on purpose:

- The signed manifest already binds every installer's name, size and
  SHA-256, which is all the updater decides on. The provenance is signed
  by the same key, so it adds no protection against a forged installer.
- The browser would have to parse more untrusted input (DSSE, in-toto,
  base64) for no decision it could make with it.
- A late or broken provenance must never hold back a security update.
  The release process enforces it instead: `sign` and
  `publish-release-signature.sh` refuse without it, and the workflow
  re-checks every published release.

### Check it yourself

You need Python 3 and `python3 -m pip install cryptography`, and a copy of
this repository (for the tool and the public keys in
`update_signing_keys.cc`).

```bash
python3 tools/release_signing/molt_provenance.py verify \
  --bundle molt-build-provenance.intoto.jsonl MoltBrowser-Linux-x64.deb
```

- It checks every signature against the trusted keys, then the file's
  SHA-256 against its statement, and prints what the file was built from.
- `--json` prints the verified statements. To read one without checking
  it: `head -n1 molt-build-provenance.intoto.jsonl | jq -r .payload | base64 -d | jq .`
- For a whole downloaded release, as the workflow does:
  ```bash
  python3 tools/release_signing/molt_provenance.py verify \
    --bundle release-vX.Y.Z/molt-build-provenance.intoto.jsonl \
    --tag vX.Y.Z --expect-commit "$(git rev-parse 'vX.Y.Z^{commit}')" \
    --require-all release-vX.Y.Z/*
  ```

Workflow builds also carry GitHub's own build attestation
(`gh attestation verify <file> --repo OneManNoCode/MoltBrowser`). That
covers only builds that ran in GitHub Actions; the signed provenance
covers every release, wherever it was built.

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
- `python3 tools/release_signing/molt_provenance_test.py` covers the build
  provenance end to end with a throwaway Ed25519 key it creates:
  - generation from fake checkouts: a golden statement, determinism, and
    the scrubbing of paths, user names and secrets;
  - the signing step and its refusals;
  - verification of a full release folder, and tampering with subjects,
    predicates, keys and assets;
  - `release-provenance.sh` and `publish-release-signature.sh`.

  With securesystemslib and in-toto-attestation installed (as in CI) it
  also checks the bundles with those.
- `python3 tools/release_signing/molt_release_sign.py verify --help` lists
  the options for checking any release by hand.
