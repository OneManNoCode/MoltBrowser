# Software bill of materials (SBOM)

Every MoltBrowser installer is published with an SBOM: a machine-readable
list of what is inside it. Use it to check which Chromium and which
third-party components a release contains, which licences apply, and whether
a published vulnerability affects you.

## What you get

Each release asset gets its own SBOM, attached to the same GitHub release:

| Installer | SBOM |
|---|---|
| `MoltBrowser-macOS-arm64.dmg` | `MoltBrowser-macOS-arm64-dmg.spdx.json` |
| `MoltBrowser-Linux-x64.deb` | `MoltBrowser-Linux-x64-deb.spdx.json` |
| `MoltBrowser-Linux-x64.rpm` | `MoltBrowser-Linux-x64-rpm.spdx.json` |
| `MoltBrowser-Linux-x64.tar.gz` | `MoltBrowser-Linux-x64-tar-gz.spdx.json` |
| `MoltBrowser-Windows-x64-Setup.exe` | `MoltBrowser-Windows-x64-Setup-exe.spdx.json` |

The name is the installer's name with every `.` turned into `-`, plus
`.spdx.json`. An SBOM name never contains `.dmg`, `.deb`, `.rpm`, `.tar.gz`
or `Setup.exe`: the in-app updater picks its installer from the release
assets by those strings, so a name like `X.deb.spdx.json` could be offered
as an update.

Each SBOM lists:

- **The installer itself**, with its SHA-256. This is the package the SBOM
  describes; it contains everything below.
- **Chromium**, at the version and commit in `chromium_version.txt`, with
  the CPE that vulnerability databases use for Chrome.
- **Every third-party component of the build.** These are the components
  that Chromium's own `tools/licenses/licenses.py` reports for the `chrome`
  target that was built (`--shipped-only`). For each one the SBOM gives the
  name, version and revision, licence, homepage and CPE, taken from its
  `README.chromium`. Rust crates also carry their `pkg:cargo` package URL.
- **MoltBrowser's own vendored code**, marked "vendored by MoltBrowser".
  This is `third_party/llama_cpp` (llama.cpp b11641) and the Rust crates
  MoltBrowser adds, such as adblock-rust and anydoc and their
  dependencies.
- **The bundled helpers**, with every shipped file's SHA-1 and SHA-256:
  - tor and its pluggable transports (Tor Expert Bundle);
  - tesseract with the libraries copied next to it, each under its
    Homebrew formula and version, plus its English traineddata;
  - whisper.cpp with its tiny.en model;
  - any bundled AI model.

  Each helper also carries the version and the source-archive SHA-256 that
  the bundle scripts pin.
- **MoltNet's bundled font set**: one package for the set (its manifest
  version) and one per font file, each with its download location (the
  pinned repository, commit and path), SHA-256 and licence (OFL-1.1). The
  shipped `molt_fonts` folder is checked against the manifest file by
  file (`release.sh` refuses a release without it).
- **On Linux**, the distribution's `tor` package as an *optional dependency*
  (it is recommended, not bundled).

Licences are SPDX identifiers where `README.chromium` uses one (or a common
spelling of one). Anything else, such as Chromium's plain "BSD", becomes a
`LicenseRef-` entry that says what the README declared.

## Why SPDX 2.3

We chose SPDX 2.3 (JSON) over CycloneDX 1.6 for these reasons:

- Chromium's own licence tooling (`licenses.py`, `spdx_writer.py`) speaks
  SPDX, and `README.chromium` licence fields are SPDX identifiers.
- Our main per-component data is licensing and provenance (licences,
  download locations, pinned checksums), which is what SPDX models
  natively. SPDX is an ISO standard (ISO/IEC 5962, from 2.2.1).
- GitHub exports its dependency graph as SPDX 2.3, and Syft, Grype and
  Trivy read SPDX 2.3 JSON.
- The official validator (`pyspdxtools`, from spdx-tools) checks it in CI.

CycloneDX is stronger for vulnerability exploitability (VEX) data, which we
don't publish yet. Converting SPDX to CycloneDX is a supported path in
common tools if a consumer needs it.

## Checking an SBOM

The SBOM is itself covered by everything that protects the installers:

```bash
sha256sum -c --ignore-missing SHA256SUMS-linux-x64        # the SBOM is listed
python3 tools/release_signing/molt_provenance.py verify \
  --bundle molt-build-provenance.intoto.jsonl \
  MoltBrowser-Linux-x64-deb.spdx.json                      # signed provenance
python3 tools/sbom/molt_sbom.py validate \
  --artifact MoltBrowser-Linux-x64.deb MoltBrowser-Linux-x64-deb.spdx.json
```

- The second command checks the release's signed build provenance: the
  SBOM is a subject of its build's statement, so this proves it came out
  of that build, with the installer it describes. See
  [SIGNED_UPDATES.md, "Build provenance"](SIGNED_UPDATES.md#build-provenance).
- The last command checks the SPDX structure and that the SBOM describes
  exactly that file (same name, same SHA-256).
- Builds that ran in GitHub Actions also carry GitHub's attestation:
  `gh attestation verify <file> --repo OneManNoCode/MoltBrowser`.

The provenance and the SBOM complement each other. The SBOM lists what is
inside an installer; the provenance says what it was built from (commit,
Chromium, patches, GN args, toolchain) and on which builder.

The offline-signed release manifest (`molt-release-manifest.json`,
[SIGNED_UPDATES.md](SIGNED_UPDATES.md)) lists every release asset with its
size and SHA-256. SBOMs are release assets, so the manifest covers them, and
`publish-release-signature.sh` and the "Verify release signature" workflow
refuse a release whose SBOMs don't match it. We didn't add a separate SBOM
field to the manifest for three reasons:

- The updater never reads SBOMs.
- The SBOM already names its installer and that installer's SHA-256.
- A new field would change the signed format, which the browser parses.

To look for known vulnerabilities:

```bash
grype sbom:MoltBrowser-Linux-x64-deb.spdx.json
trivy sbom MoltBrowser-Linux-x64-deb.spdx.json
```

## How it is made

`scripts/release-sbom.sh` runs `tools/sbom/molt_sbom.py` (Python 3.9+,
standard library only) on the build machine, after packaging and code
signing (it hashes the shipped files), in every release workflow and
release script. With `MOLT_RELEASE_BUILD=1` it refuses to write an
incomplete SBOM. It stops if any of these is true:

- there is no Chromium component list;
- a macOS build gives no app to scan;
- a zip or tarball contains helpers that were not scanned;
- the checkout is not at the pinned Chromium;
- a shipped file has no pin;
- a shipped model or data file doesn't match its pinned SHA-256.
- a shipped font file is missing, changed or not in
  `tools/fonts/moltnet_fonts.json`.

Where the data comes from:

| Data | Source |
|---|---|
| Chromium version, revision | `chromium_version.txt`, cross-checked with the checkout's `chrome/VERSION` and `HEAD` |
| Third-party components | `tools/licenses/licenses.py list --verbose --shipped-only --gn-target //chrome:chrome` in the checkout, plus each `README.chromium` for Version / Revision / CPEPrefix |
| Tor | `scripts/bundle-tor.sh` (`PINNED_TOR_VERSION`, `EXPECTED_SHA256_TOR_*`); `tor/.version` in the app |
| tesseract and libraries | `ocr/.version` and `ocr/.bundle-provenance.json`, written by `scripts/bundle-tesseract.sh` through `macho_bundle_deps.py --provenance` |
| tessdata | `scripts/bundle-tesseract.sh` (`TESSDATA_COMMIT`, `EXPECTED_SHA256_TESSDATA_ENG`) |
| whisper.cpp and its model | `scripts/bundle-whisper.sh` (`WHISPER_TAG`, `EXPECTED_COMMIT_WHISPER_*`, `EXPECTED_SHA256_WHISPER_TINY_EN`) |
| Bundled models | the model registry in `browser_ai_runtime.cc` (revision and sha256 pins) |
| Windows helpers | the `$pins` table in `release-windows-selfhosted.yml` |
| MoltNet's font set | `tools/fonts/moltnet_fonts.json` (source repositories and commits, per file path, size and SHA-256, licences); installed by `scripts/bundle-fonts.sh` |

`molt_sbom.py generate --repo-only --platform macos|linux|windows` works
from this repository alone. It lists the MoltBrowser-owned parts:

- the Chromium pin;
- llama.cpp;
- the Rust crates MoltBrowser adds, with their dependencies resolved through
  the vendored `Cargo.lock` (listed as `DEPENDS_ON`, since the lock file
  can't say what links on which platform);
- the helper pins;
- the font set's pins.

It does not list Chromium's own third-party components; the release SBOMs
do.

## Tests

`tools/sbom/molt_sbom_test.py` (CI: `lint.yml`, job `sbom`) builds a fake
release from `tools/sbom/testdata/` and compares the generated SBOMs with
golden files. The fake release has a Chromium checkout whose `licenses.py`
is a stand-in, a MoltBrowser.app payload and a Windows staging folder. The
test validates every SBOM with the built-in checker and with `pyspdxtools`,
and also runs `--repo-only` against this repository, so a renamed pin in a
bundle script fails CI rather than a release. After an intended change to
the output, rewrite the golden files with
`MOLT_SBOM_UPDATE_GOLDEN=1 python3 tools/sbom/molt_sbom_test.py` and review
the diff.
