#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""SPDX 2.3 SBOMs for MoltBrowser release artifacts.

One SBOM per release artifact (the .dmg, .deb, .rpm, .tar.gz, AppImage,
Setup.exe, portable .zip), named after it with every "." turned into "-":
    MoltBrowser-Linux-x64.deb  ->  MoltBrowser-Linux-x64-deb.spdx.json
The name must not contain ".dmg", ".deb", ".rpm", ".tar.gz" or "Setup.exe":
the in-app updater picks its installer from the release assets by those
substrings (UpdateManager::PlatformAssetMatcher), so an SBOM called
"X.deb.spdx.json" could be offered as the update.

An SBOM lists what the artifact contains:
  * the artifact itself, with its SHA-256 (this is the package the SBOM
    DESCRIBES; it CONTAINS everything below);
  * Chromium, at the version and revision of the checkout that was built
    (chromium_version.txt pins it);
  * every third-party component the built GN target links, as the
    checkout's tools/licenses/licenses.py reports them (`list --verbose
    --shipped-only --gn-target`), with Version, Revision and CPEPrefix
    taken from each README.chromium (licenses.py reads only Name, URL,
    License and License File). MoltBrowser's own vendored code, copied into
    the checkout from src/ (third_party/llama_cpp, the Rust crates gnrt
    vendored), is part of that list and is marked as such;
  * the helpers bundled into the artifact (tor and its pluggable
    transports, tesseract and the libraries copied with it, whisper.cpp,
    their data files, a bundled AI model): every file with its SHA-1 and
    SHA-256, and the version and source hash that the bundle scripts pin.
    A shipped data file whose pin is known must match it.

Modes:
  generate --artifact FILE... --platform P --version V
           --chromium-src DIR --gn-out-dir DIR [--payload DIR] [--release]
      On the build machine, after packaging. --release (what the release
      scripts pass) refuses anything incomplete: no Chromium component list,
      no payload for a platform that bundles helpers, a checkout that is not
      at the pin, a shipped file with no pin.
  generate --repo-only --platform P [--out FILE]
      From this repository alone: the MoltBrowser-owned parts (Chromium pin,
      llama.cpp, the Rust crates MoltBrowser adds and their dependencies
      from the vendored Cargo.lock, the helper pins). Chromium's own
      third-party components are not listed.
  validate SBOM... [--artifact FILE]
      Structural SPDX 2.3 checks plus MoltBrowser's own rules.

Python 3.9+, standard library only. docs/SBOM.md has the details.
"""

import argparse
import ast
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import uuid
import zipfile

TOOL_VERSION = '1'
PRODUCT = 'MoltBrowser'
SUPPLIER = 'Organization: GenEye AI Labs Inc.'
GITHUB_REPO = 'OneManNoCode/MoltBrowser'
REPO_URL = 'https://github.com/' + GITHUB_REPO
NAMESPACE_BASE = REPO_URL + '/spdx'
MOLTBROWSER_LICENSE = 'GPL-3.0-only'
CHROMIUM_GIT = 'https://chromium.googlesource.com/chromium/src.git'
DEFAULT_GN_TARGET = '//chrome:chrome'

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

# What the updater looks for in asset names (update_manager.cc). An SBOM's
# own name must contain none of them.
UPDATER_ASSET_MATCHERS = ('.dmg', 'Setup.exe', '.deb', '.rpm', '.tar.gz')

PLATFORMS = ('macos', 'linux', 'windows')
# Platforms whose release artifacts always bundle helper programs (and so
# need --payload in a release). Windows builds bundle them only in the
# self-hosted workflow; a zip without --payload is checked instead
# (helper_hints).
BUNDLING_PLATFORMS = ('macos',)
HELPER_HINT = re.compile(r'(^|/)(tor/tor(\.exe)?|ocr/tesseract(\.exe)?|'
                         r'whisper/whisper-cli(\.exe)?|molt_models/[^/]+)$')


class Error(Exception):
    pass


# --------------------------------------------------------------------------
# Small helpers


def read_text(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def file_digests(path):
    """{'SHA1': hex, 'SHA256': hex} of a file."""
    sha1, sha256 = hashlib.sha1(), hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            sha1.update(chunk)
            sha256.update(chunk)
    return {'SHA1': sha1.hexdigest(), 'SHA256': sha256.hexdigest()}


def sbom_name_for(artifact_name):
    """MoltBrowser-Linux-x64.tar.gz -> MoltBrowser-Linux-x64-tar-gz.spdx.json"""
    name = artifact_name.replace('.', '-') + '.spdx.json'
    check_sbom_name(name)
    return name


def check_sbom_name(name):
    for m in UPDATER_ASSET_MATCHERS:
        if m in name:
            raise Error('SBOM name %r contains %r, which the in-app updater '
                        'would take for an installer' % (name, m))
    if not re.fullmatch(r'[A-Za-z0-9_-][A-Za-z0-9._-]{0,127}', name):
        raise Error('SBOM name %r is not a plain release asset name' % name)


def normalize_version(tag):
    return tag[1:] if tag[:1] in ('v', 'V') else tag


def created_timestamp():
    """ISO 8601 UTC; honours SOURCE_DATE_EPOCH for reproducible output."""
    epoch = os.environ.get('SOURCE_DATE_EPOCH')
    if epoch:
        when = datetime.datetime.fromtimestamp(int(epoch),
                                               tz=datetime.timezone.utc)
    else:
        when = datetime.datetime.now(tz=datetime.timezone.utc)
    return when.strftime('%Y-%m-%dT%H:%M:%SZ')


def git_head(path):
    """HEAD of the git checkout rooted at PATH (not of a repository that
    merely contains PATH)."""
    try:
        out = subprocess.run(
            ['git', '-C', path, 'rev-parse', '--show-toplevel', 'HEAD'],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            universal_newlines=True, check=False)
    except OSError:
        return None
    lines = out.stdout.split('\n')
    if out.returncode != 0 or len(lines) < 2 or \
            os.path.realpath(lines[0]) != os.path.realpath(path):
        return None
    rev = lines[1].strip()
    return rev if re.fullmatch(r'[0-9a-f]{40}', rev) else None


def is_url(value):
    return bool(value) and re.match(r'^(https?|git|ftp)://\S+$', value) is not None


# --------------------------------------------------------------------------
# Inputs from this repository


def read_chromium_pin(repo):
    pin = {}
    for line in read_text(os.path.join(repo, 'chromium_version.txt')).splitlines():
        key, sep, value = line.partition('=')
        if sep and key.strip() in ('CHROMIUM_VERSION', 'CHROMIUM_REVISION'):
            pin[key.strip()] = value.strip()
    if not re.fullmatch(r'\d+\.\d+\.\d+\.\d+', pin.get('CHROMIUM_VERSION', '')) \
            or not re.fullmatch(r'[0-9a-f]{40}', pin.get('CHROMIUM_REVISION', '')):
        raise Error('chromium_version.txt has no valid CHROMIUM_VERSION / '
                    'CHROMIUM_REVISION')
    return pin['CHROMIUM_VERSION'], pin['CHROMIUM_REVISION']


SHELL_ASSIGNMENT = re.compile(
    r'^\s*([A-Z][A-Z0-9_]*)="([^"$`\\]*)"\s*(?:#.*)?$')


def shell_vars(path):
    """NAME="literal" assignments of a shell script (first one wins)."""
    found = {}
    if not os.path.isfile(path):
        return found
    for line in read_text(path).splitlines():
        m = SHELL_ASSIGNMENT.match(line)
        if m and m.group(1) not in found:
            found[m.group(1)] = m.group(2)
    return found


WINDOWS_PIN = re.compile(r'^\s*"([A-Za-z0-9._-]+)"\s*=\s*"([0-9a-fA-F]*)"\s*$')


def windows_pins(path):
    """The $pins = @{ "name" = "sha256" } table of the Windows workflow."""
    pins = {}
    if not os.path.isfile(path):
        return pins
    inside = False
    for line in read_text(path).splitlines():
        if re.match(r'^\s*\$pins\s*=\s*@\{', line):
            inside = True
            continue
        if inside:
            if line.strip() == '}':
                break
            m = WINDOWS_PIN.match(line)
            if m:
                pins[m.group(1)] = m.group(2).lower()
    return pins


def model_registry(path):
    """The on-device model registry (browser_ai_runtime.cc), by model id."""
    models = {}
    if not os.path.isfile(path):
        return models
    text = read_text(path)
    start = text.find('kModelRegistry[] = {')
    if start < 0:
        return models
    body = text[start:]
    # Each entry starts with {"<id>", and holds its strings in a fixed order:
    # id, display name, repo, file, revision, sha256, quantization, company,
    # license.
    for m in re.finditer(r'\{"([^"]+)",(.*?)\n\s*(?:true|false)\}', body, re.S):
        entry = re.sub(r'//[^\n]*', '', m.group(2))
        strings = re.findall(r'"([^"]*)"', entry)
        size = re.search(r'/\*size=\*/(\d+)ULL', entry)
        if len(strings) < 8:
            continue
        models[m.group(1)] = {
            'id': m.group(1), 'display': strings[0], 'repo': strings[1],
            'file': strings[2], 'revision': strings[3], 'sha256': strings[4],
            'license': strings[7], 'size': int(size.group(1)) if size else 0,
        }
    return models


class Pins:
    """Every helper pin, read from where it lives (the bundle scripts, the
    Windows workflow, the model registry)."""

    def __init__(self, repo):
        s = os.path.join(repo, 'scripts')
        self.tor = shell_vars(os.path.join(s, 'bundle-tor.sh'))
        self.whisper = shell_vars(os.path.join(s, 'bundle-whisper.sh'))
        self.tesseract = shell_vars(os.path.join(s, 'bundle-tesseract.sh'))
        self.bundled_model = shell_vars(os.path.join(s, 'download-bundled-model.sh'))
        self.windows = windows_pins(os.path.join(
            repo, '.github', 'workflows', 'release-windows-selfhosted.yml'))
        self.models = model_registry(os.path.join(
            repo, 'src', 'chrome', 'browser', 'molt_ai', 'runtime',
            'browser_ai_runtime.cc'))

    def whisper_commit(self, tag):
        key = 'EXPECTED_COMMIT_WHISPER_' + re.sub(r'[^A-Za-z0-9]', '_', tag.upper())
        return self.whisper.get(key, '')

    def windows_version(self, prefix):
        """("tor-" -> "15.0.15", its sha256) from the Windows pin table."""
        for key, sha in sorted(self.windows.items()):
            if key.startswith(prefix):
                return key[len(prefix):], sha
        return '', ''


# ---------------------------------------------------------- README.chromium

README_FIELDS = ('Name', 'Short Name', 'URL', 'Version', 'Revision', 'License',
                 'License File', 'Shipped', 'Shipped in Chromium', 'CPEPrefix',
                 'Security Critical', 'Update Mechanism')
README_DIVIDER = re.compile(r'^-{20} DEPENDENCY DIVIDER -{20}$')
README_NAMES = ('README.chromium', 'README.crashpad', 'README.v8',
                'README.pdfium', 'README.angle')


def parse_readme_chromium(text):
    """The dependencies a README.chromium describes (one dict each)."""
    lookup = {f.lower(): f for f in README_FIELDS}
    deps, cur = [], {}
    for raw in text.splitlines():
        line = raw.strip()
        if README_DIVIDER.match(line):
            if cur:
                deps.append(cur)
            cur = {}
            continue
        key, sep, value = line.partition(':')
        field = lookup.get(key.strip().lower()) if sep else None
        if field and field not in cur:
            cur[field] = value.strip()
    if cur:
        deps.append(cur)
    return deps


def readme_metadata(directory, name=None):
    """README.chromium metadata in DIRECTORY, for the dependency NAME."""
    for fname in README_NAMES:
        path = os.path.join(directory, fname)
        if not os.path.isfile(path):
            continue
        deps = parse_readme_chromium(read_text(path))
        for dep in deps:
            if name is None or dep.get('Name') == name:
                return dep
        if len(deps) == 1:
            return deps[0]
    return {}


def meaningful(value):
    if not value:
        return None
    if value.strip().lower() in ('n/a', 'na', 'none', 'unknown', '0', '-'):
        return None
    return value.strip()


RUST_DIR = re.compile(r'^third_party/rust/([^/]+)/(v[^/]+)$')


# ------------------------------------------------------------ Cargo files

def parse_cargo_lock(text):
    """[{name, version, source, dependencies}] from a Cargo.lock."""
    packages, cur, in_deps = [], None, False
    for raw in text.splitlines():
        line = raw.strip()
        if line == '[[package]]':
            cur = {'name': '', 'version': '', 'source': '', 'dependencies': []}
            packages.append(cur)
            in_deps = False
            continue
        if cur is None:
            continue
        if in_deps:
            if line.startswith(']'):
                in_deps = False
            else:
                m = re.match(r'^"([^"]+)",?$', line)
                if m:
                    cur['dependencies'].append(m.group(1))
            continue
        m = re.match(r'^(name|version|source|checksum) = "([^"]*)"$', line)
        if m:
            cur[m.group(1)] = m.group(2)
        elif line.startswith('dependencies = ['):
            in_deps = not line.endswith(']')
            for dep in re.findall(r'"([^"]+)"', line):
                cur['dependencies'].append(dep)
    return [p for p in packages if p['name'] and p['version']]


def molt_cargo_deps(text):
    """Dependencies that Cargo.toml marks with a "# MoltBrowser:" comment:
    {name: version requirement}."""
    deps = {}
    pending = False
    table = None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith('#'):
            if line.startswith('# MoltBrowser:'):
                pending = True
            continue
        if not line:
            pending = False
            continue
        m = re.match(r'^\[dependencies\.([A-Za-z0-9_-]+)\]$', line)
        if m:
            table = m.group(1) if pending else None
            pending = False
            continue
        if line.startswith('['):
            table = None
            pending = False
            continue
        if table:
            v = re.match(r'^version\s*=\s*"([^"]+)"', line)
            if v:
                deps[table] = v.group(1)
            continue
        m = re.match(r'^([A-Za-z0-9_-]+)\s*=\s*"([^"]+)"', line)
        if m and pending:
            deps[m.group(1)] = m.group(2)
        pending = False
    return deps


def crate_epoch(version):
    """Chromium's per-epoch directory name: 0.13.3 -> v0_13, 1.2.0 -> v1."""
    parts = version.split('.')
    if parts[0] != '0':
        return 'v' + parts[0]
    if len(parts) > 1 and parts[1] != '0':
        return 'v0_' + parts[1]
    return 'v0_0_' + (parts[2] if len(parts) > 2 else '0')


# ----------------------------------------------------------------- licences

# A working subset of the SPDX License List (3.2x): the identifiers this
# generator emits and the ones Chromium's README.chromium files use.
SPDX_LICENSES = frozenset("""
0BSD AFL-2.0 AFL-2.1 AGPL-3.0-only AGPL-3.0-or-later Apache-1.0 Apache-1.1
Apache-2.0 APSL-2.0 Artistic-1.0 Artistic-2.0 Beerware blessing BSD-1-Clause
BSD-2-Clause BSD-2-Clause-Patent BSD-2-Clause-Views BSD-3-Clause
BSD-3-Clause-Clear BSD-3-Clause-Attribution BSD-4-Clause BSD-Source-Code
BSL-1.0 bzip2-1.0.6 CC-BY-3.0 CC-BY-4.0 CC-BY-SA-3.0 CC-BY-SA-4.0 CC0-1.0
CDDL-1.0 CDDL-1.1 curl EPL-1.0 EPL-2.0 EUPL-1.2 FTL GFDL-1.3-only
GPL-2.0-only GPL-2.0-or-later GPL-3.0-only GPL-3.0-or-later HPND ICU IJG
Info-ZIP ISC JSON LGPL-2.0-only LGPL-2.0-or-later LGPL-2.1-only
LGPL-2.1-or-later LGPL-3.0-only LGPL-3.0-or-later Libpng libpng-2.0
libtiff MIT MIT-0 MIT-CMU MIT-Modern-Variant MPL-1.1 MPL-2.0
MPL-2.0-no-copyleft-exception MS-PL NAIST-2003 NCSA OFL-1.1 OpenSSL
PostgreSQL PSF-2.0 Python-2.0 Ruby SGI-B-2.0 Spencer-94 SSPL-1.0
Unicode-3.0 Unicode-DFS-2015 Unicode-DFS-2016 Unlicense UPL-1.0 W3C
W3C-20150513 WTFPL X11 Zlib zlib-acknowledgement ZPL-2.1
""".split())

SPDX_EXCEPTIONS = frozenset("""
LLVM-exception Classpath-exception-2.0 GCC-exception-3.1 Bison-exception-2.2
Autoconf-exception-3.0 Font-exception-2.0 Linux-syscall-note
""".split())

# Common spellings in README.chromium / registries, mapped to SPDX.
LICENSE_ALIASES = {
    'apache 2.0': 'Apache-2.0', 'apache-2': 'Apache-2.0',
    'apache license 2.0': 'Apache-2.0', 'apache v2.0': 'Apache-2.0',
    'apache license, version 2.0': 'Apache-2.0', 'apache 2': 'Apache-2.0',
    'apache-2.0': 'Apache-2.0', 'apache2': 'Apache-2.0',
    'bsd 3-clause': 'BSD-3-Clause', 'bsd-3': 'BSD-3-Clause',
    'new bsd': 'BSD-3-Clause', '3-clause bsd': 'BSD-3-Clause',
    'bsd 2-clause': 'BSD-2-Clause', 'simplified bsd': 'BSD-2-Clause',
    'mit license': 'MIT', 'mit/x11': 'MIT', 'expat': 'MIT', 'mit': 'MIT',
    'mpl 2.0': 'MPL-2.0', 'mpl 1.1': 'MPL-1.1',
    'lgpl 2.1': 'LGPL-2.1-only', 'lgpl v2.1': 'LGPL-2.1-only',
    'lgpl-2.1': 'LGPL-2.1-only', 'lgpl-2.1+': 'LGPL-2.1-or-later',
    'lgpl 2.0': 'LGPL-2.0-only', 'lgpl-2.0': 'LGPL-2.0-only',
    'lgpl-3.0': 'LGPL-3.0-only', 'gpl v2': 'GPL-2.0-only',
    'gpl-2.0': 'GPL-2.0-only', 'gpl-3.0': 'GPL-3.0-only',
    'gplv3': 'GPL-3.0-only', 'isc license': 'ISC', 'zlib': 'Zlib',
    'zlib license': 'Zlib', 'boost': 'BSL-1.0', 'cc0': 'CC0-1.0',
    'cc-by-4.0': 'CC-BY-4.0', 'cc-by-sa-4.0': 'CC-BY-SA-4.0',
    'unicode': 'Unicode-3.0', 'ms-pl': 'MS-PL',
}


class Licenses:
    """Turns free-form licence names into SPDX expressions, collecting the
    LicenseRef-s it had to invent (SPDX needs each one declared)."""

    def __init__(self):
        self.refs = {}   # LicenseRef id -> (name, extracted text)

    def _ref(self, raw, origin):
        base = re.sub(r'[^A-Za-z0-9.-]+', '-', raw).strip('-.')[:60] or 'Unknown'
        ref = 'LicenseRef-' + base
        known = self.refs.get(ref)
        if known is not None and known[0] != raw:
            ref += '-' + hashlib.sha256(raw.encode()).hexdigest()[:8]
        if ref not in self.refs:
            text = ('Declared as "%s"%s. This SBOM does not carry the licence '
                    'text; see the component\'s licence files.'
                    % (raw, (' in ' + origin) if origin else ''))
            self.refs[ref] = (raw, text)
        return ref

    def _id(self, token, origin):
        plus = token.endswith('+')
        core = token[:-1] if plus else token
        if core in SPDX_LICENSES:
            return token
        alias = LICENSE_ALIASES.get(token.lower())
        if alias:
            return alias
        if token.startswith('LicenseRef-') and re.fullmatch(
                r'LicenseRef-[A-Za-z0-9.-]+', token):
            return self._ref(token[len('LicenseRef-'):], origin)
        return None

    def expression(self, raw, origin=''):
        """SPDX licence expression for RAW, or NOASSERTION."""
        raw = (raw or '').strip()
        if not raw or raw.upper() in ('NOASSERTION', 'UNKNOWN'):
            return 'NOASSERTION'
        if raw.upper() == 'NONE':
            return 'NONE'
        whole = self._id(raw, origin)
        if whole:
            return whole
        expr = self._as_expression(raw, origin)
        if expr:
            return expr
        # A comma-separated list: every licence applies.
        if ',' in raw:
            parts = [p.strip() for p in raw.split(',') if p.strip()]
            out = []
            for p in parts:
                e = self.expression(p, origin)
                out.append('(%s)' % e if ' ' in e else e)
            return ' AND '.join(out)
        return self._ref(raw, origin)

    def _as_expression(self, raw, origin):
        text = raw.replace('/', ' OR ')
        tokens = re.findall(r'\(|\)|[^\s()]+', text)
        if not any(t.upper() in ('AND', 'OR', 'WITH') for t in tokens):
            return None
        out, after_with = [], False
        for t in tokens:
            if t in ('(', ')'):
                out.append(t)
            elif t.upper() in ('AND', 'OR', 'WITH'):
                out.append(t.upper())
                after_with = t.upper() == 'WITH'
                continue
            elif after_with:
                if t not in SPDX_EXCEPTIONS:
                    return None
                out.append(t)
            else:
                lid = self._id(t, origin)
                if not lid:
                    return None
                out.append(lid)
            after_with = False
        expr = ' '.join(out).replace('( ', '(').replace(' )', ')')
        return expr if not check_license_expression(expr, set(self.refs)) else None

    def extracted(self):
        return [{'licenseId': ref, 'name': name, 'extractedText': text}
                for ref, (name, text) in sorted(self.refs.items())]


def check_license_expression(expr, declared_refs):
    """Problems with an SPDX licence expression (empty list if fine)."""
    if expr in ('NOASSERTION', 'NONE'):
        return []
    tokens = re.findall(r'\(|\)|[^\s()]+', expr)
    if not tokens:
        return ['empty licence expression']
    problems, depth, expect_operand, after_with = [], 0, True, False
    for t in tokens:
        if t == '(':
            if not expect_operand:
                problems.append('unexpected "(" in %r' % expr)
            depth += 1
        elif t == ')':
            depth -= 1
            if depth < 0 or expect_operand:
                problems.append('unbalanced ")" in %r' % expr)
        elif t in ('AND', 'OR', 'WITH'):
            if expect_operand:
                problems.append('operator %s without operand in %r' % (t, expr))
            expect_operand = True
            after_with = t == 'WITH'
        else:
            if not expect_operand:
                problems.append('missing operator before %r in %r' % (t, expr))
            if after_with:
                if t not in SPDX_EXCEPTIONS:
                    problems.append('unknown licence exception %r' % t)
            elif t.startswith('LicenseRef-'):
                if t not in declared_refs:
                    problems.append('%s is not declared in '
                                    'hasExtractedLicensingInfos' % t)
            elif (t[:-1] if t.endswith('+') else t) not in SPDX_LICENSES:
                problems.append('unknown SPDX licence id %r' % t)
            expect_operand, after_with = False, False
    if depth != 0 or expect_operand:
        problems.append('incomplete licence expression %r' % expr)
    return problems


# ------------------------------------------------------------- the packages


class Package:
    def __init__(self, key, name, version=None, *, purpose='LIBRARY',
                 license_raw=None, license_origin='', supplier=None,
                 download='NOASSERTION', homepage=None, file_name=None,
                 checksums=None, purl=None, cpe=None, source_info=None,
                 comment=None):
        self.key = key
        self.name = name
        self.version = version
        self.purpose = purpose
        self.license_raw = license_raw
        self.license_origin = license_origin
        self.supplier = supplier
        self.download = download
        self.homepage = homepage
        self.file_name = file_name
        self.checksums = checksums or {}
        self.purl = purl
        self.cpe = cpe
        self.source_info = source_info
        self.comment = comment
        self.files = []        # (display path, digests)
        self.spdx_id = None
        self.shipped = True
        # How the described package relates to this one.
        self.relation = 'CONTAINS'


def cpe_ref(cpe):
    if not cpe:
        return None
    if cpe.startswith('cpe:2.3:'):
        return {'referenceCategory': 'SECURITY', 'referenceType': 'cpe23Type',
                'referenceLocator': cpe}
    if cpe.startswith('cpe:/'):
        return {'referenceCategory': 'SECURITY', 'referenceType': 'cpe22Type',
                'referenceLocator': cpe}
    return None


def chromium_package(version, revision):
    return Package(
        'chromium', 'Chromium', version, purpose='FRAMEWORK',
        license_raw='BSD-3-Clause', supplier='Organization: The Chromium Authors',
        download='git+%s@%s' % (CHROMIUM_GIT, revision),
        homepage='https://www.chromium.org',
        purl='pkg:generic/chromium@%s?vcs_url=git%%2B%s%%40%s'
             % (version, CHROMIUM_GIT.replace(':', '%3A'), revision),
        # NVD files Chromium's security fixes against Google Chrome.
        cpe='cpe:2.3:a:google:chrome:%s:*:*:*:*:*:*:*' % version,
        source_info='Chromium %s at revision %s (chromium_version.txt). Its '
                    'security fixes are tracked against Google Chrome %s in '
                    'NVD, hence the CPE.' % (version, revision, version))


def third_party_package(meta, readme, vendored_by_molt, origin,
                        in_checkout=True):
    """A Chromium-checkout component from licenses.py metadata + README."""
    d = meta['dir'].replace(os.sep, '/').strip('/')
    name = meta.get('Name') or readme.get('Name') or d
    version = meaningful(readme.get('Version'))
    revision = meaningful(readme.get('Revision'))
    url = meta.get('URL') or readme.get('URL') or ''
    homepage = url if is_url(url) else None
    purl = download = None
    m = RUST_DIR.match(d)
    if m and version:
        purl = 'pkg:cargo/%s@%s' % (name, version)
        download = 'https://crates.io/api/v1/crates/%s/%s/download' % (name, version)
    elif revision and homepage and re.match(
            r'^https://(github\.com|gitlab\.com|[a-z0-9.-]+\.googlesource\.com)/',
            homepage):
        download = 'git+%s@%s' % (homepage, revision)
    if in_checkout:
        info = '//%s in the Chromium checkout' % d
        if vendored_by_molt:
            info += '; vendored by MoltBrowser (src/%s)' % d
    else:
        info = 'src/%s in the MoltBrowser repository (vendored by MoltBrowser)' % d
    if revision and version:
        info += '; revision %s' % revision
    pkg = Package(
        'tp:' + d + ':' + name, name, version or revision, purpose='LIBRARY',
        license_raw=meta.get('License') or readme.get('License'),
        license_origin=origin, homepage=homepage,
        download=download or 'NOASSERTION', purl=purl,
        cpe=meaningful(readme.get('CPEPrefix')), source_info=info)
    return pkg


# ------------------------------------------------- Chromium component list


def run_licenses_py(chromium_src, gn_out_dir, gn_target, target_os, python):
    script = os.path.join(chromium_src, 'tools', 'licenses', 'licenses.py')
    if not os.path.isfile(script):
        raise Error('%s not found (is --chromium-src a Chromium checkout?)'
                    % script)
    if not os.path.isfile(os.path.join(gn_out_dir, 'args.gn')):
        raise Error('%s has no args.gn (is --gn-out-dir the build directory?)'
                    % gn_out_dir)
    cmd = [python, script, 'list', '--verbose', '--shipped-only',
           '--gn-out-dir', os.path.abspath(gn_out_dir), '--gn-target', gn_target]
    if target_os:
        cmd += ['--target-os', target_os]
    proc = subprocess.run(cmd, cwd=chromium_src, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True)
    if proc.returncode != 0:
        raise Error('licenses.py failed (exit %d):\n%s'
                    % (proc.returncode, proc.stderr.strip()[-4000:]))
    return proc.stdout


def parse_licenses_list(text):
    """Metadata dicts from `licenses.py list --verbose` (one repr per line)."""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith('{'):
            continue
        try:
            meta = ast.literal_eval(line)
        except (ValueError, SyntaxError) as e:
            raise Error('cannot parse licenses.py output line: %s' % line[:200]) from e
        if not isinstance(meta, dict) or 'Name' not in meta or 'dir' not in meta:
            raise Error('unexpected licenses.py entry: %s' % line[:200])
        out.append(meta)
    if not out:
        raise Error('licenses.py listed no components')
    return out


def chromium_checkout_version(chromium_src):
    path = os.path.join(chromium_src, 'chrome', 'VERSION')
    if not os.path.isfile(path):
        return None
    parts = {}
    for line in read_text(path).splitlines():
        k, sep, v = line.partition('=')
        if sep:
            parts[k.strip()] = v.strip()
    try:
        return '.'.join(parts[k] for k in ('MAJOR', 'MINOR', 'BUILD', 'PATCH'))
    except KeyError:
        return None


# ------------------------------------------------- MoltBrowser's own vendoring


def overlay_readmes(repo):
    """{dir: [deps]} for every README.chromium MoltBrowser vendors in src/."""
    src = os.path.join(repo, 'src')
    found = {}
    tp = os.path.join(src, 'third_party')
    if not os.path.isdir(tp):
        return found
    candidates = []
    for name in sorted(os.listdir(tp)):
        candidates.append(os.path.join(tp, name))
    rust = os.path.join(tp, 'rust')
    if os.path.isdir(rust):
        for crate in sorted(os.listdir(rust)):
            cdir = os.path.join(rust, crate)
            if os.path.isdir(cdir):
                for epoch in sorted(os.listdir(cdir)):
                    candidates.append(os.path.join(cdir, epoch))
    for d in candidates:
        path = os.path.join(d, 'README.chromium')
        if os.path.isfile(path):
            rel = os.path.relpath(d, src).replace(os.sep, '/')
            found[rel] = parse_readme_chromium(read_text(path))
    return found


def repo_only_components(repo):
    """The MoltBrowser-vendored components, from src/ alone."""
    pkgs = []
    readmes = overlay_readmes(repo)
    crates_with_readme = {}
    for d, deps in sorted(readmes.items()):
        for dep in deps:
            meta = {'dir': d, 'Name': dep.get('Name', d),
                    'URL': dep.get('URL', ''), 'License': dep.get('License', '')}
            pkg = third_party_package(meta, dep, True, 'src/%s/README.chromium' % d,
                                      in_checkout=False)
            pkg.shipped = (dep.get('Shipped') or dep.get('Shipped in Chromium')
                           or 'yes').lower() != 'no'
            pkgs.append(pkg)
            if RUST_DIR.match(d):
                crates_with_readme[(dep.get('Name', ''), RUST_DIR.match(d).group(2))] = pkg

    # The Rust crates MoltBrowser adds, and what they pull in, from the
    # vendored Cargo.lock.
    cio = os.path.join(repo, 'src', 'third_party', 'rust', 'chromium_crates_io')
    lock_path = os.path.join(cio, 'Cargo.lock')
    toml_path = os.path.join(cio, 'Cargo.toml')
    lock = parse_cargo_lock(read_text(lock_path)) if os.path.isfile(lock_path) else []
    declared = molt_cargo_deps(read_text(toml_path)) if os.path.isfile(toml_path) else {}
    by_name = {}
    for p in lock:
        by_name.setdefault(p['name'], []).append(p)

    def lock_entry(ref):
        # "name", "name version" or "name version (source)"
        name, _, ver = ref.partition(' ')
        ver = ver.split(' ')[0]
        cands = by_name.get(name, [])
        if ver:
            cands = [c for c in cands if c['version'] == ver]
        return cands[0] if len(cands) == 1 else None

    roots, declared_roots = set(), set()
    for (name, epoch) in crates_with_readme:
        for c in by_name.get(name, []):
            if crate_epoch(c['version']) == epoch:
                roots.add((c['name'], c['version']))
    missing = []
    for name, req in sorted(declared.items()):
        exact = req[1:] if req.startswith('=') else None
        cands = by_name.get(name, [])
        hit = [c for c in cands if exact is None or c['version'] == exact]
        if hit:
            roots.add((hit[0]['name'], hit[0]['version']))
            declared_roots.add((hit[0]['name'], hit[0]['version']))
        else:
            missing.append((name, req))
    seen, todo = set(), sorted(roots)
    while todo:
        name, ver = todo.pop()
        if (name, ver) in seen:
            continue
        seen.add((name, ver))
        entry = lock_entry(name + ' ' + ver)
        for dep in (entry or {}).get('dependencies', []):
            e = lock_entry(dep)
            if e and (e['name'], e['version']) not in seen:
                todo.append((e['name'], e['version']))
    readme_versions = {(p.name, p.version) for p in crates_with_readme.values()}
    for name, ver in sorted(seen):
        if (name, ver) in readme_versions:
            continue
        if (name, ver) in declared_roots:
            info = ('Rust crate MoltBrowser adds (src/third_party/rust/'
                    'chromium_crates_io/Cargo.toml, %s = "%s"), resolved in '
                    'its Cargo.lock; vendored by scripts/vendor-rust-crates.sh.'
                    % (name, declared[name]))
        else:
            info = ('Rust crate required by a crate MoltBrowser adds '
                    '(src/third_party/rust/chromium_crates_io/Cargo.lock); '
                    'vendored by upstream Chromium or at build time.')
        pkg = Package(
            'crate:%s@%s' % (name, ver), name, ver, purpose='LIBRARY',
            license_raw=None, download='https://crates.io/api/v1/crates/%s/%s/download'
            % (name, ver), purl='pkg:cargo/%s@%s' % (name, ver),
            source_info=info,
            comment='Licence not recorded in this repository; the build '
                    'machine SBOM takes it from the vendored README.chromium. '
                    'Cargo.lock does not say whether it is linked on every '
                    'platform or only used at build time, hence DEPENDS_ON.')
        pkg.relation = 'DEPENDS_ON'
        pkgs.append(pkg)
    for name, req in missing:
        exact = req[1:] if req.startswith('=') else None
        pkg = Package(
            'crate:%s' % name, name, exact, purpose='LIBRARY',
            download=('https://crates.io/api/v1/crates/%s/%s/download' % (name, exact))
            if exact else 'NOASSERTION',
            purl=('pkg:cargo/%s@%s' % (name, exact)) if exact else None,
            source_info='Declared in src/third_party/rust/chromium_crates_io/'
                        'Cargo.toml (%s = "%s") and not yet vendored in this '
                        'checkout: scripts/vendor-rust-crates.sh vendors it '
                        'and its dependencies on the build machine.' % (name, req))
        pkg.relation = 'DEPENDS_ON'
        pkgs.append(pkg)
    return [p for p in pkgs if p.shipped]


# ------------------------------------------------------------- the helpers


def macho_arch(path):
    """'aarch64' / 'x86_64' / 'universal' / None for a Mach-O file."""
    try:
        with open(path, 'rb') as f:
            head = f.read(8)
    except OSError:
        return None
    if len(head) < 8:
        return None
    if head[:4] in (b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
        return 'universal'
    if head[:4] == b'\xcf\xfa\xed\xfe':
        cpu = int.from_bytes(head[4:8], 'little')
        return {0x0100000c: 'aarch64', 0x01000007: 'x86_64'}.get(cpu)
    return None


def list_files(root, skip_hidden_meta=True):
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for fn in sorted(filenames):
            if skip_hidden_meta and fn in ('.version', '.bundle-provenance.json'):
                continue
            path = os.path.join(dirpath, fn)
            if os.path.isfile(path) and not os.path.islink(path):
                out.append(path)
    return out


def read_first_line(path):
    if not os.path.isfile(path):
        return ''
    lines = read_text(path).splitlines()
    return lines[0].strip() if lines else ''


class Payload:
    """The shipped tree of an artifact: a MoltBrowser.app, the Windows
    staging folder, or the Linux /opt/moltbrowser staging folder."""

    def __init__(self, path):
        self.path = os.path.abspath(path)
        if not os.path.isdir(self.path):
            raise Error('--payload %s is not a directory' % path)
        res = os.path.join(self.path, 'Contents', 'Resources')
        self.helpers_root = res if os.path.isdir(res) else self.path
        self.label = os.path.basename(self.path.rstrip(os.sep))

    def display(self, path):
        rel = os.path.relpath(path, self.path).replace(os.sep, '/')
        return './%s/%s' % (self.label, rel)

    def add_files(self, pkg, paths):
        for p in paths:
            pkg.files.append((self.display(p), file_digests(p)))


class HelperScan:
    def __init__(self, platform, pins, release, warn):
        self.platform = platform
        self.pins = pins
        self.release = release
        self.warn = warn
        self.packages = []

    def unpinned(self, what):
        msg = '%s has no pinned SHA-256' % what
        if self.release:
            raise Error(msg + ' (a release must not ship unverified files)')
        self.warn(msg)

    def check_pin(self, what, path, pin):
        if not pin:
            self.unpinned(what)
            return
        actual = file_digests(path)['SHA256']
        if actual != pin.lower():
            raise Error('%s: SHA-256 %s does not match the pin %s'
                        % (path, actual, pin))

    # ---- tor
    def tor(self, payload, d):
        exe = 'tor.exe' if self.platform == 'windows' else 'tor'
        if not os.path.isfile(os.path.join(d, exe)):
            raise Error('%s has no %s binary' % (d, exe))
        if self.platform == 'windows':
            version, pin = self.pins.windows_version('tor-')
            arch = 'x86_64'
            where = '.github/workflows/release-windows-selfhosted.yml ($pins)'
        else:
            version = read_first_line(os.path.join(d, '.version')) \
                or self.pins.tor.get('TOR_VERSION', '')
            arch = macho_arch(os.path.join(d, exe)) or 'aarch64'
            pin = ''
            if version and version == self.pins.tor.get('PINNED_TOR_VERSION'):
                pin = self.pins.tor.get('EXPECTED_SHA256_TOR_' + arch.upper(), '')
            where = 'scripts/bundle-tor.sh (PINNED_TOR_VERSION, EXPECTED_SHA256_TOR_%s)' \
                % arch.upper()
        os_name = 'windows' if self.platform == 'windows' else 'macos'
        archive = 'tor-expert-bundle-%s-%s-%s.tar.gz' % (os_name, arch, version)
        pkg = Package(
            'helper:tor', 'Tor Expert Bundle', version or None,
            purpose='APPLICATION',
            license_raw='BSD-3-Clause AND BSD-2-Clause AND CC-BY-SA-4.0',
            supplier='Organization: The Tor Project, Inc.',
            download=('https://archive.torproject.org/tor-package-archive/'
                      'torbrowser/%s/%s' % (version, archive)) if version
            else 'NOASSERTION',
            homepage='https://www.torproject.org', file_name=archive,
            checksums={'SHA256': pin} if pin else None,
            source_info='Pinned in %s; the checksum is the pinned SHA-256 of '
                        'the downloaded archive.' % where,
            comment='tor and libevent (BSD-3-Clause), the lyrebird '
                    '(BSD-2-Clause) and snowflake (BSD-3-Clause) pluggable '
                    'transports, and the geoip databases derived from the '
                    'IPFire location database (CC-BY-SA-4.0). Bundled by '
                    'scripts/bundle-tor.sh; MoltNet runs only this copy.')
        if not pin:
            self.unpinned('Tor Expert Bundle %s (%s)' % (version or '?', arch))
        payload.add_files(pkg, list_files(d))
        self.packages.append(pkg)

    # ---- tesseract
    def ocr(self, payload, d):
        if self.platform == 'windows':
            version, pin = self.pins.windows_version('tesseract-')
            pkg = Package(
                'helper:tesseract', 'Tesseract OCR (UB Mannheim build)',
                version or None, purpose='APPLICATION', license_raw='Apache-2.0',
                download=('https://github.com/UB-Mannheim/tesseract/releases/'
                          'download/v%s/tesseract-ocr-w64-setup-%s.exe'
                          % (version, version)) if version else 'NOASSERTION',
                homepage='https://github.com/tesseract-ocr/tesseract',
                file_name='tesseract-ocr-w64-setup-%s.exe' % version,
                checksums={'SHA256': pin} if pin else None,
                source_info='Pinned in .github/workflows/'
                            'release-windows-selfhosted.yml ($pins); the '
                            'checksum is that of the installer.',
                comment='tesseract.exe, the libraries the installer ships with '
                        'it and its traineddata.')
            if not pin:
                self.unpinned('Tesseract %s' % (version or '?'))
            payload.add_files(pkg, list_files(d))
            self.packages.append(pkg)
            return

        provenance = read_provenance(d)
        binary = os.path.join(d, 'tesseract')
        claimed = set()
        binary_pkg, binary_formula = None, None
        if os.path.isfile(binary):
            origin = (provenance.get('binary') or {}).get('origin', '')
            cellar = homebrew_cellar(origin)
            version = read_first_line(os.path.join(d, '.version')) or \
                (cellar[1] if cellar else '')
            pkg = Package(
                'helper:tesseract', 'tesseract', version or None,
                purpose='APPLICATION', license_raw='Apache-2.0',
                homepage='https://github.com/tesseract-ocr/tesseract',
                source_info='Copied from the build machine (%s) by '
                            'scripts/bundle-tesseract.sh.'
                            % (origin or 'origin not recorded'))
            payload.add_files(pkg, [binary])
            claimed.add(binary)
            self.packages.append(pkg)
            binary_pkg, binary_formula = pkg, cellar[0] if cellar else None
        self.bundled_libs(payload, d, provenance, binary_pkg, binary_formula,
                          claimed, 'tesseract')
        traineddata = os.path.join(d, 'tessdata', 'eng.traineddata')
        if os.path.isfile(traineddata):
            commit = self.pins.tesseract.get('TESSDATA_COMMIT', '')
            pin = self.pins.tesseract.get('EXPECTED_SHA256_TESSDATA_ENG', '')
            self.check_pin('tessdata eng.traineddata', traineddata, pin)
            pkg = Package(
                'helper:tessdata-eng', 'tessdata_fast eng.traineddata',
                commit or None, purpose='OTHER', license_raw='Apache-2.0',
                download=('https://raw.githubusercontent.com/tesseract-ocr/'
                          'tessdata_fast/%s/eng.traineddata' % commit)
                if commit else 'NOASSERTION',
                homepage='https://github.com/tesseract-ocr/tessdata_fast',
                checksums={'SHA256': pin} if pin else None,
                source_info='Pinned in scripts/bundle-tesseract.sh '
                            '(TESSDATA_COMMIT, EXPECTED_SHA256_TESSDATA_ENG).',
                comment='Trained model data for tesseract (English, fast).')
            payload.add_files(pkg, [traineddata])
            claimed.add(traineddata)
            self.packages.append(pkg)
        rest = [p for p in list_files(d) if p not in claimed]
        if rest:
            raise Error('unrecognised files in %s: %s' % (
                d, ', '.join(os.path.relpath(p, d) for p in rest)))

    def bundled_libs(self, payload, d, provenance, owner, owner_formula,
                     claimed, helper):
        """The libraries scripts/macho_bundle_deps.py copied into D/lib:
        one package per Homebrew formula (from the recorded origins), or
        per file when the origin is unknown. A library of the helper's own
        formula (libtesseract) joins OWNER."""
        lib_dir = os.path.join(d, 'lib')
        if not os.path.isdir(lib_dir):
            return
        origins = {}
        for lib in provenance.get('libraries') or []:
            if isinstance(lib, dict) and lib.get('file'):
                origins[os.path.normpath(lib['file'])] = lib.get('origin', '')
        groups = {}
        for path in list_files(lib_dir):
            origin = origins.get(os.path.normpath(os.path.relpath(path, d)), '')
            cellar = homebrew_cellar(origin)
            key = cellar if cellar else (os.path.basename(path), '')
            groups.setdefault(key, []).append((path, origin))
        for (name, version), items in sorted(groups.items()):
            paths = [p for p, _ in items]
            claimed.update(paths)
            if owner is not None and name == owner_formula:
                payload.add_files(owner, paths)
                continue
            brew = brew_info(name) if version else {}
            pkg = Package(
                'helper:%s-lib:%s' % (helper, name), name, version or None,
                purpose='LIBRARY', license_raw=brew.get('license'),
                homepage=brew.get('homepage'),
                source_info='Library copied next to the bundled %s by '
                            'scripts/macho_bundle_deps.py from %s.'
                            % (helper, ', '.join(sorted(
                                {o or 'an unrecorded origin' for _, o in items}))))
            payload.add_files(pkg, paths)
            self.packages.append(pkg)

    # ---- whisper.cpp
    def whisper(self, payload, d):
        exe = 'whisper-cli.exe' if self.platform == 'windows' else 'whisper-cli'
        model = os.path.join(d, 'ggml-tiny.en.bin')
        model_url = self.pins.whisper.get('MODEL_URL', '')
        if self.platform == 'windows':
            version, pin = self.pins.windows_version('whisper-bin-x64-')
            model_pin = self.pins.windows.get('ggml-tiny.en.bin', '')
            model_url = model_url or ('https://huggingface.co/ggerganov/'
                                      'whisper.cpp/resolve/main/ggml-tiny.en.bin')
            pkg = Package(
                'helper:whisper', 'whisper.cpp', normalize_version(version) or None,
                purpose='APPLICATION', license_raw='MIT',
                download=('https://github.com/ggml-org/whisper.cpp/releases/'
                          'download/%s/whisper-bin-x64.zip' % version)
                if version else 'NOASSERTION',
                homepage='https://github.com/ggml-org/whisper.cpp',
                file_name='whisper-bin-x64.zip',
                checksums={'SHA256': pin} if pin else None,
                purl=('pkg:github/ggml-org/whisper.cpp@%s' % version) if version else None,
                source_info='Pinned in .github/workflows/'
                            'release-windows-selfhosted.yml ($pins); the '
                            'checksum is that of the release zip.')
            if not pin:
                self.unpinned('whisper.cpp %s' % (version or '?'))
        else:
            tag = read_first_line(os.path.join(d, '.version')) or \
                self.pins.whisper.get('WHISPER_TAG', '')
            commit = self.pins.whisper_commit(tag) if tag else ''
            model_pin = self.pins.whisper.get('EXPECTED_SHA256_WHISPER_TINY_EN', '')
            if not commit:
                msg = 'whisper.cpp %s has no pinned commit' % (tag or '?')
                if self.release:
                    raise Error(msg)
                self.warn(msg)
            pkg = Package(
                'helper:whisper', 'whisper.cpp', normalize_version(tag) or None,
                purpose='APPLICATION', license_raw='MIT',
                download=('git+https://github.com/ggerganov/whisper.cpp@%s' % commit)
                if commit else 'NOASSERTION',
                homepage='https://github.com/ggml-org/whisper.cpp',
                purl=('pkg:github/ggerganov/whisper.cpp@%s' % tag) if tag else None,
                source_info='Built from source at the commit pinned in '
                            'scripts/bundle-whisper.sh (WHISPER_TAG, '
                            'EXPECTED_COMMIT_WHISPER_*).')
        if not os.path.isfile(os.path.join(d, exe)):
            raise Error('%s has no %s binary' % (d, exe))
        claimed = {model}
        self.packages.append(pkg)
        if self.platform != 'windows':
            # Libraries the build picked up (normally none), by origin.
            self.bundled_libs(payload, d, read_provenance(d), None, None,
                              claimed, 'whisper-cli')
        payload.add_files(pkg, [p for p in list_files(d) if p not in claimed])
        if os.path.isfile(model):
            self.check_pin('whisper model ggml-tiny.en.bin', model, model_pin)
            mpkg = Package(
                'helper:whisper-model', 'whisper ggml-tiny.en model', None,
                purpose='OTHER', license_raw='MIT', download=model_url or 'NOASSERTION',
                homepage='https://huggingface.co/ggerganov/whisper.cpp',
                checksums={'SHA256': model_pin} if model_pin else None,
                source_info='Pinned in scripts/bundle-whisper.sh '
                            '(EXPECTED_SHA256_WHISPER_TINY_EN)'
                            if self.platform != 'windows' else
                            'Pinned in .github/workflows/'
                            'release-windows-selfhosted.yml ($pins).',
                comment='Speech-recognition model weights (OpenAI Whisper '
                        'tiny.en, converted to ggml).')
            payload.add_files(mpkg, [model])
            self.packages.append(mpkg)

    # ---- bundled AI models
    def models(self, payload, d):
        for path in list_files(d):
            fname = os.path.basename(path)
            model_id = fname[:-len('.gguf')] if fname.endswith('.gguf') else None
            entry = self.pins.models.get(model_id) if model_id else None
            if entry is None:
                raise Error('%s: not a model of the registry in '
                            'browser_ai_runtime.cc' % path)
            self.check_pin('bundled model %s' % model_id, path, entry['sha256'])
            rev = entry['revision'] or 'main'
            pkg = Package(
                'helper:model:%s' % model_id, entry['display'],
                entry['revision'] or None, purpose='OTHER',
                license_raw=entry['license'] or None,
                download='https://huggingface.co/%s/resolve/%s/%s'
                         % (entry['repo'], rev, entry['file']),
                homepage='https://huggingface.co/%s' % entry['repo'],
                file_name=entry['file'],
                checksums={'SHA256': entry['sha256']} if entry['sha256'] else None,
                source_info='Model registry entry "%s" in src/chrome/browser/'
                            'molt_ai/runtime/browser_ai_runtime.cc (revision '
                            'and sha256 pins).' % model_id,
                comment='On-device language model weights (GGUF).')
            payload.add_files(pkg, [path])
            self.packages.append(pkg)

    def scan(self, payload):
        root = payload.helpers_root
        for name, fn in (('tor', self.tor), ('ocr', self.ocr),
                         ('whisper', self.whisper), ('molt_models', self.models)):
            d = os.path.join(root, name)
            if os.path.isdir(d):
                fn(payload, d)
        return self.packages


# ------------------------------------------------- MoltNet's bundled fonts

FONTS_DIR = 'molt_fonts'
FONTS_MANIFEST = os.path.join('tools', 'fonts', 'moltnet_fonts.json')


def _font_manifest_module():
    """tools/fonts/manifest.py of this tool's own repository (the rules)."""
    sys.path.insert(0, os.path.join(REPO_ROOT, 'tools', 'fonts'))
    try:
        import manifest as font_manifest  # noqa: E402
    finally:
        sys.path.pop(0)
    return font_manifest


def font_set_packages(repo, payload=None, release=False, warn=None):
    """MoltNet's bundled font set, from tools/fonts/moltnet_fonts.json in
    `repo`: one package per font file and one for the set (its manifest and
    licence texts), each with the manifest's pinned SHA-256. Every platform
    ships the set (scripts/bundle-fonts.sh checks the pins when it installs
    it). With a payload that has the set, every installed file is checked
    against its pin and listed; an unexpected file fails. [] if `repo` has
    no manifest."""
    path = os.path.join(repo, FONTS_MANIFEST)
    if not os.path.isfile(path):
        return []
    mf = _font_manifest_module()
    try:
        data = mf.load(path)
    except mf.ManifestError as e:
        raise Error('%s: %s' % (path, e)) from e
    pending = [e['install_name'] for _, e in mf.all_entries(data)
               if mf.is_pending(e)]
    if pending:
        msg = '%s has unpinned entries: %s' % (FONTS_MANIFEST,
                                               ', '.join(pending))
        if release:
            raise Error(msg + ' (a release must not ship unverified files)')
        if warn:
            warn(msg)
    licenses = {e['install_name']: e for e in data['licenses']}
    installed = None
    if payload is not None:
        d = os.path.join(payload.helpers_root, FONTS_DIR)
        if os.path.isdir(d):
            installed = d
        elif warn:
            warn('%s has no %s folder; the font set is listed from %s only'
                 % (payload.label, FONTS_DIR, FONTS_MANIFEST))
    seen = set()

    def checked(rel, entry):
        """The installed file `rel`, checked against `entry`'s pin."""
        if installed is None:
            return None
        p = os.path.join(installed, *rel.split('/'))
        if not os.path.isfile(p):
            raise Error('%s is missing from %s' % (rel, installed))
        actual = file_digests(p)['SHA256']
        if entry is not None and actual != entry['sha256']:
            raise Error('%s: SHA-256 %s does not match the pin %s'
                        % (p, actual, entry['sha256']))
        seen.add(rel)
        return p

    set_pkg = Package(
        'fonts:set', 'MoltNet font set', data['version'], purpose='OTHER',
        license_raw=' AND '.join(sorted({e['spdx'] for e in licenses.values()})),
        homepage=REPO_URL,
        source_info='Pinned in %s (sources at fixed commits, each file by '
                    'size and SHA-256); installed by scripts/bundle-fonts.sh.'
                    % FONTS_MANIFEST,
        comment='The fonts MoltNet windows draw text with, the same files on '
                'every OS: the manifest and the licence texts of the fonts '
                'listed with it.')
    pkgs = [set_pkg]
    for rel, entry in [(mf.INSTALLED_MANIFEST, None)] + [
            ('%s/%s' % (mf.LICENSE_DIR, e['install_name']), e)
            for e in data['licenses'] if not mf.is_pending(e)]:
        p = checked(rel, entry)
        if p:
            payload.add_files(set_pkg, [p])
    for entry in data['files']:
        if mf.is_pending(entry):
            continue
        source = data['sources'][entry['source']]
        faces = entry['faces']
        families = sorted({f['family'] for f in faces})
        version = faces[0]['version'] if faces else ''
        version = re.sub(r'^Version\s+', '', version).split(';')[0].strip()
        lic = licenses[entry['license']]
        pkg = Package(
            'fonts:' + entry['install_name'],
            '%s (%s)' % (', '.join(families), entry['install_name']),
            version or None, purpose='OTHER', license_raw=lic['spdx'],
            download=mf.source_url(source, entry['path']),
            homepage='https://github.com/%s' % source['repo'],
            file_name=entry['install_name'],
            checksums={'SHA256': entry['sha256']},
            source_info='%s at commit %s, pinned in %s.'
                        % (source['repo'], source['commit'], FONTS_MANIFEST),
            comment='Font file of the MoltNet font set; licence text: %s/%s.'
                    % (mf.LICENSE_DIR, lic['install_name']))
        p = checked(entry['install_name'], entry)
        if p:
            payload.add_files(pkg, [p])
        pkgs.append(pkg)
    if installed is not None:
        extra = sorted(
            os.path.relpath(p, installed).replace(os.sep, '/')
            for p in list_files(installed, skip_hidden_meta=False))
        extra = [r for r in extra if r not in seen]
        if extra:
            raise Error('%s holds files the font manifest doesn\'t pin: %s'
                        % (installed, ', '.join(extra[:5])))
    return pkgs


def read_provenance(d):
    """D/.bundle-provenance.json (scripts/macho_bundle_deps.py --provenance)."""
    path = os.path.join(d, '.bundle-provenance.json')
    if not os.path.isfile(path):
        return {}
    try:
        doc = json.loads(read_text(path))
    except ValueError as e:
        raise Error('%s is not valid JSON' % path) from e
    return doc if isinstance(doc, dict) else {}


def homebrew_cellar(origin):
    """('leptonica', '1.85.0') from a .../Cellar/leptonica/1.85.0/... path."""
    m = re.search(r'/Cellar/([^/]+)/([^/]+)/', origin or '')
    return (m.group(1), m.group(2)) if m else None


_BREW_CACHE = {}
_BREW_DISABLED = [False]


def brew_info(formula):
    """Licence and homepage of a Homebrew formula, if brew is here."""
    if formula in _BREW_CACHE:
        return _BREW_CACHE[formula]
    info = {}
    brew = None if _BREW_DISABLED[0] else shutil.which('brew')
    if brew and re.fullmatch(r'[A-Za-z0-9@+._-]+', formula):
        proc = subprocess.run([brew, 'info', '--json=v2', '--formula', formula],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              universal_newlines=True, check=False)
        try:
            f = json.loads(proc.stdout)['formulae'][0]
            info = {'license': f.get('license'), 'homepage': f.get('homepage')}
        except (ValueError, KeyError, IndexError, TypeError):
            info = {}
    _BREW_CACHE[formula] = info
    return info


def declared_helpers(platform, pins):
    """Repo-only mode: the helpers the bundle scripts pin, without files."""
    pkgs = []
    if platform == 'macos':
        ver = pins.tor.get('PINNED_TOR_VERSION', '')
        for arch in ('aarch64', 'x86_64'):
            pin = pins.tor.get('EXPECTED_SHA256_TOR_' + arch.upper(), '')
            archive = 'tor-expert-bundle-macos-%s-%s.tar.gz' % (arch, ver)
            pkgs.append(Package(
                'helper:tor:' + arch, 'Tor Expert Bundle (%s)' % arch, ver or None,
                purpose='APPLICATION',
                license_raw='BSD-3-Clause AND BSD-2-Clause AND CC-BY-SA-4.0',
                supplier='Organization: The Tor Project, Inc.',
                download='https://archive.torproject.org/tor-package-archive/'
                         'torbrowser/%s/%s' % (ver, archive),
                file_name=archive, checksums={'SHA256': pin} if pin else None,
                source_info='Pinned in scripts/bundle-tor.sh%s.'
                            % ('' if pin else '; SHA-256 pin still empty')))
        tag = pins.whisper.get('WHISPER_TAG', '')
        commit = pins.whisper_commit(tag)
        pkgs.append(Package(
            'helper:whisper', 'whisper.cpp', normalize_version(tag) or None,
            purpose='APPLICATION', license_raw='MIT',
            download=('git+https://github.com/ggerganov/whisper.cpp@%s' % commit)
            if commit else 'NOASSERTION',
            purl='pkg:github/ggerganov/whisper.cpp@%s' % tag if tag else None,
            source_info='Pinned in scripts/bundle-whisper.sh.'))
        mpin = pins.whisper.get('EXPECTED_SHA256_WHISPER_TINY_EN', '')
        pkgs.append(Package(
            'helper:whisper-model', 'whisper ggml-tiny.en model', None,
            purpose='OTHER', license_raw='MIT',
            download=pins.whisper.get('MODEL_URL') or 'NOASSERTION',
            checksums={'SHA256': mpin} if mpin else None,
            source_info='Pinned in scripts/bundle-whisper.sh%s.'
                        % ('' if mpin else '; SHA-256 pin still empty')))
        commit = pins.tesseract.get('TESSDATA_COMMIT', '')
        tpin = pins.tesseract.get('EXPECTED_SHA256_TESSDATA_ENG', '')
        pkgs.append(Package(
            'helper:tessdata-eng', 'tessdata_fast eng.traineddata', commit or None,
            purpose='OTHER', license_raw='Apache-2.0',
            download='https://raw.githubusercontent.com/tesseract-ocr/'
                     'tessdata_fast/%s/eng.traineddata' % commit,
            checksums={'SHA256': tpin} if tpin else None,
            source_info='Pinned in scripts/bundle-tesseract.sh.'))
        pkgs.append(Package(
            'helper:tesseract', 'tesseract', None, purpose='APPLICATION',
            license_raw='Apache-2.0',
            homepage='https://github.com/tesseract-ocr/tesseract',
            source_info='Copied from the build machine\'s Homebrew with the '
                        'libraries it loads (scripts/bundle-tesseract.sh); '
                        'versions are recorded at bundle time.'))
    elif platform == 'windows':
        for prefix, name in (('tor-', 'Tor Expert Bundle'),
                             ('tesseract-', 'Tesseract OCR (UB Mannheim build)'),
                             ('whisper-bin-x64-', 'whisper.cpp')):
            ver, pin = pins.windows_version(prefix)
            pkgs.append(Package(
                'helper:win:' + prefix.strip('-'), name,
                normalize_version(ver) or None, purpose='APPLICATION',
                license_raw={'tor-': 'BSD-3-Clause AND BSD-2-Clause AND CC-BY-SA-4.0',
                             'tesseract-': 'Apache-2.0',
                             'whisper-bin-x64-': 'MIT'}[prefix],
                checksums={'SHA256': pin} if pin else None,
                source_info='Pinned in .github/workflows/'
                            'release-windows-selfhosted.yml ($pins)%s.'
                            % ('' if pin else '; SHA-256 pin still empty')))
        mpin = pins.windows.get('ggml-tiny.en.bin', '')
        pkgs.append(Package(
            'helper:whisper-model', 'whisper ggml-tiny.en model', None,
            purpose='OTHER', license_raw='MIT',
            checksums={'SHA256': mpin} if mpin else None,
            source_info='Pinned in .github/workflows/'
                        'release-windows-selfhosted.yml ($pins)%s.'
                        % ('' if mpin else '; SHA-256 pin still empty')))
    return pkgs


def linux_tor_dependency():
    return Package(
        'dep:tor', 'tor (distribution package)', None, purpose='APPLICATION',
        license_raw='BSD-3-Clause', supplier='Organization: The Tor Project, Inc.',
        homepage='https://www.torproject.org',
        comment='Not bundled. The .deb and .rpm recommend the distribution\'s '
                'tor package; MoltNet runs /usr/bin/tor (or /usr/sbin/tor) '
                'only if root owns it.')


# --------------------------------------------------------------- the document


def spdx_id(prefix, key, used):
    base = re.sub(r'[^A-Za-z0-9.-]+', '-', key).strip('-') or 'x'
    candidate = 'SPDXRef-%s-%s' % (prefix, base)
    n = 2
    while candidate in used:
        candidate = 'SPDXRef-%s-%s-%d' % (prefix, base, n)
        n += 1
    used.add(candidate)
    return candidate


def package_json(pkg, licenses):
    out = {
        'SPDXID': pkg.spdx_id,
        'name': pkg.name,
        'downloadLocation': pkg.download or 'NOASSERTION',
        'filesAnalyzed': bool(pkg.files),
        'licenseConcluded': 'NOASSERTION',
        'licenseDeclared': licenses.expression(pkg.license_raw, pkg.license_origin),
        'copyrightText': 'NOASSERTION',
        'primaryPackagePurpose': pkg.purpose,
    }
    if pkg.version:
        out['versionInfo'] = pkg.version
    out['supplier'] = pkg.supplier or 'NOASSERTION'
    if pkg.file_name:
        out['packageFileName'] = pkg.file_name
    if is_url(pkg.homepage):
        out['homepage'] = pkg.homepage
    if pkg.checksums:
        out['checksums'] = [{'algorithm': a, 'checksumValue': v}
                            for a, v in sorted(pkg.checksums.items())]
    refs = []
    if pkg.purl:
        refs.append({'referenceCategory': 'PACKAGE-MANAGER',
                     'referenceType': 'purl', 'referenceLocator': pkg.purl})
    cref = cpe_ref(pkg.cpe)
    if cref:
        refs.append(cref)
    if refs:
        out['externalRefs'] = refs
    if pkg.source_info:
        out['sourceInfo'] = pkg.source_info
    if pkg.comment:
        out['comment'] = pkg.comment
    if pkg.files:
        code = hashlib.sha1(''.join(sorted(d['SHA1'] for _, d in pkg.files))
                            .encode()).hexdigest()
        out['packageVerificationCode'] = {'packageVerificationCodeValue': code}
        out['licenseInfoFromFiles'] = ['NOASSERTION']
    return out


def build_document(name, root, components, licenses, comment, extra_rels=(),
                   not_contained=()):
    """An SPDX 2.3 document: DOCUMENT DESCRIBES root, root CONTAINS each
    component (but NOT_CONTAINED), each component CONTAINS its files."""
    used = set()
    root.spdx_id = spdx_id('Package', root.name, used)
    for pkg in components:
        pkg.spdx_id = spdx_id('Package', pkg.key, used)
    packages, files, rels = [], [], []
    rels.append({'spdxElementId': 'SPDXRef-DOCUMENT',
                 'relationshipType': 'DESCRIBES',
                 'relatedSpdxElement': root.spdx_id})
    for pkg in [root] + list(components):
        packages.append(package_json(pkg, licenses))
        if pkg is not root and pkg not in not_contained:
            rels.append({'spdxElementId': root.spdx_id,
                         'relationshipType': pkg.relation,
                         'relatedSpdxElement': pkg.spdx_id})
        for display, digests in pkg.files:
            fid = spdx_id('File', display[2:], used)
            files.append({
                'SPDXID': fid, 'fileName': display,
                'checksums': [{'algorithm': a, 'checksumValue': v}
                              for a, v in sorted(digests.items())],
                'licenseConcluded': 'NOASSERTION',
                'licenseInfoInFiles': ['NOASSERTION'],
                'copyrightText': 'NOASSERTION',
            })
            rels.append({'spdxElementId': pkg.spdx_id,
                         'relationshipType': 'CONTAINS',
                         'relatedSpdxElement': fid})
    for element, rtype, related in extra_rels:
        rels.append({'spdxElementId': element.spdx_id, 'relationshipType': rtype,
                     'relatedSpdxElement': related.spdx_id})
    created = created_timestamp()
    seed = json.dumps([name, created, packages[0]], sort_keys=True)
    namespace = '%s/%s-%s' % (NAMESPACE_BASE, name,
                              uuid.uuid5(uuid.NAMESPACE_URL, seed))
    # e.g. .../spdx/MoltBrowser-Linux-x64.deb-<uuid5 of the content>
    doc = {
        'spdxVersion': 'SPDX-2.3',
        'dataLicense': 'CC0-1.0',
        'SPDXID': 'SPDXRef-DOCUMENT',
        'name': name,
        'documentNamespace': namespace,
        'creationInfo': {
            'created': created,
            'creators': ['Tool: molt_sbom.py-%s' % TOOL_VERSION, SUPPLIER],
        },
        'comment': comment,
        'packages': packages,
    }
    if files:
        doc['files'] = files
    doc['relationships'] = rels
    extracted = licenses.extracted()
    if extracted:
        doc['hasExtractedLicensingInfos'] = extracted
    return doc


def write_json(path, doc):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)
        f.write('\n')
    os.replace(tmp, path)


# ------------------------------------------------------------------ generate


def helper_hints(artifact):
    """Member names of a .zip / .tar.gz artifact that look like bundled
    helpers (an SBOM made without --payload would miss them)."""
    lower = artifact.lower()
    try:
        if lower.endswith('.zip'):
            with zipfile.ZipFile(artifact) as z:
                names = z.namelist()
        elif lower.endswith(('.tar.gz', '.tgz', '.tar.xz', '.tar')):
            with tarfile.open(artifact) as t:
                names = t.getnames()
        else:
            return []
    except (OSError, zipfile.BadZipFile, tarfile.TarError):
        return []
    return sorted(n for n in names if HELPER_HINT.search(n.replace('\\', '/')))


def artifact_purpose(name):
    lower = name.lower()
    if lower.endswith(('.tar.gz', '.tgz', '.zip', '.tar.xz')):
        return 'ARCHIVE'
    if lower.endswith('.appimage'):
        return 'APPLICATION'
    return 'INSTALL'


def collect_components(args, warn):
    """(components, coverage comment) for one platform, shared by every
    artifact of a run."""
    repo = os.path.abspath(args.repo)
    pin_version, pin_revision = read_chromium_pin(repo)
    version, revision = pin_version, pin_revision
    licenses_text = None
    chromium_src = os.path.abspath(args.chromium_src) if args.chromium_src else None
    if chromium_src:
        actual = chromium_checkout_version(chromium_src)
        head = git_head(chromium_src)
        if actual and actual != pin_version:
            msg = ('Chromium checkout is %s, not the pinned %s'
                   % (actual, pin_version))
            if args.release:
                raise Error(msg)
            warn(msg)
            version = actual
        if head and head != pin_revision:
            msg = ('Chromium checkout is at %s, not the pinned revision %s'
                   % (head, pin_revision))
            if args.release:
                raise Error(msg)
            warn(msg)
            revision = head
    if args.licenses_list:
        licenses_text = read_text(args.licenses_list)
    elif chromium_src:
        gn_out = args.gn_out_dir or os.path.join(chromium_src, 'out', 'MoltBrowser')
        licenses_text = run_licenses_py(
            chromium_src, gn_out, args.gn_target,
            {'macos': 'mac', 'linux': 'linux', 'windows': 'win'}[args.platform],
            args.python or sys.executable)
    elif args.release:
        raise Error('--release needs the Chromium component list: pass '
                    '--chromium-src (and --gn-out-dir) or --licenses-list')

    components = [chromium_package(version, revision)]
    overlay = overlay_readmes(repo)
    if licenses_text is not None:
        if not chromium_src:
            warn('--licenses-list without --chromium-src: versions come only '
                 'from MoltBrowser\'s own README.chromium files')
        seen = set()
        for meta in parse_licenses_list(licenses_text):
            d = meta['dir'].replace(os.sep, '/').strip('/')
            if not d:
                continue   # "The Chromium Project": the Chromium package above
            readme_dir = os.path.join(chromium_src or os.path.join(repo, 'src'), d)
            readme = readme_metadata(readme_dir, meta.get('Name'))
            if not readme and d in overlay:
                readme = readme_metadata(os.path.join(repo, 'src', d), meta.get('Name'))
            pkg = third_party_package(
                meta, readme, d in overlay,
                '//%s/README.chromium' % d)
            if pkg.key in seen:
                continue
            seen.add(pkg.key)
            components.append(pkg)
        missing = sorted(d for d in overlay
                         if d not in {m.key.split(':')[1] for m in components[1:]})
        coverage = ('Chromium third-party components: every shipped component '
                    'of %s, as listed by tools/licenses/licenses.py in the '
                    'Chromium checkout, with versions from their '
                    'README.chromium files.' % args.gn_target)
        if missing:
            coverage += (' MoltBrowser-vendored directories not linked into '
                         'that target: %s.' % ', '.join(missing))
    else:
        components.extend(repo_only_components(repo))
        coverage = ('Repository-only SBOM: lists Chromium (pinned version), the '
                    'components MoltBrowser vendors under src/third_party with '
                    'the Rust crates they need from the vendored Cargo.lock, '
                    'and the helper pins. Chromium\'s own third-party '
                    'components are NOT enumerated; the release SBOMs list '
                    'them from the build.')

    pins = Pins(repo)
    extra = []
    if args.payload:
        payload = Payload(args.payload)
        helpers = HelperScan(args.platform, pins, args.release, warn).scan(payload)
        components.extend(helpers)
        coverage += (' Bundled helpers: scanned from %s, every file with its '
                     'SHA-1 and SHA-256.' % payload.label)
    elif args.repo_only:
        components.extend(declared_helpers(args.platform, pins))
        coverage += (' Bundled helpers: none (Linux packages bundle none).'
                     if args.platform == 'linux' else
                     ' Bundled helpers: as pinned by the bundle scripts '
                     '(no files).')
    elif args.release and args.platform in BUNDLING_PLATFORMS:
        raise Error('--release on %s needs --payload (the shipped app or '
                    'staging folder) to list the bundled helpers' % args.platform)
    else:
        coverage += ' Bundled helpers: none scanned (no --payload).'
    fonts = font_set_packages(repo, Payload(args.payload) if args.payload
                              else None, args.release, warn)
    if fonts:
        components.extend(fonts)
        coverage += (' MoltNet font set: from %s (pins)%s.' % (
            FONTS_MANIFEST, ', every installed file checked against its pin'
            if args.payload else ''))
    if args.platform == 'linux':
        tor = linux_tor_dependency()
        components.append(tor)
        extra.append(tor)
    return components, coverage, extra


def generate(args):
    warnings = []

    def warn(msg):
        warnings.append(msg)
        print('molt_sbom: warning: %s' % msg, file=sys.stderr)

    if not args.repo_only and not args.artifact:
        raise Error('give --artifact FILE (one SBOM each) or --repo-only')
    if args.repo_only and (args.artifact or args.chromium_src or args.licenses_list):
        raise Error('--repo-only takes no artifact or Chromium checkout')
    if args.repo_only and args.release:
        raise Error('--repo-only SBOMs are not release SBOMs')
    _BREW_DISABLED[0] = args.no_brew
    version = normalize_version(args.version) if args.version else None
    if not version:
        if args.release or args.artifact:
            raise Error('--version is required with --artifact')
        version = 'unreleased'
    tag = 'v' + version if re.match(r'^\d', version) else version
    repo = os.path.abspath(args.repo)
    source_rev = args.source_revision or os.environ.get('GITHUB_SHA') or git_head(repo)

    if not args.payload:
        for artifact in args.artifact:
            hits = helper_hints(artifact)
            if hits:
                msg = ('%s bundles helpers (%s) but no --payload was given to '
                       'list them' % (os.path.basename(artifact),
                                      ', '.join(hits[:4])))
                if args.release:
                    raise Error(msg)
                warn(msg)
    components, coverage, optional = collect_components(args, warn)
    outputs = []
    targets = args.artifact or [None]
    for artifact in targets:
        licenses = Licenses()
        if artifact:
            name = os.path.basename(artifact)
            if not os.path.isfile(artifact):
                raise Error('artifact %s not found' % artifact)
            digests = file_digests(artifact)
            root = Package(
                'artifact', PRODUCT, version, purpose=artifact_purpose(name),
                license_raw=MOLTBROWSER_LICENSE, supplier=SUPPLIER,
                download='%s/releases/download/%s/%s' % (REPO_URL, tag, name),
                homepage=REPO_URL, file_name=name,
                checksums={'SHA256': digests['SHA256'], 'SHA1': digests['SHA1']},
                purl='pkg:github/%s@%s' % (GITHUB_REPO, tag),
                source_info=('Built from %s at commit %s.' % (REPO_URL, source_rev))
                if source_rev else None,
                comment='MoltBrowser %s for %s (%s). MoltBrowser\'s own code is '
                        'GPL-3.0-only; the components it contains keep their '
                        'own licences.' % (version, args.platform, args.arch))
            sbom_name = sbom_name_for(name)
            doc_name = name
            out_dir = args.out_dir or os.path.dirname(os.path.abspath(artifact))
            out_path = os.path.join(out_dir, sbom_name)
        else:
            root = Package(
                'source', PRODUCT, version, purpose='SOURCE',
                license_raw=MOLTBROWSER_LICENSE, supplier=SUPPLIER,
                download=('git+%s.git@%s' % (REPO_URL, source_rev))
                if source_rev else 'git+%s.git' % REPO_URL,
                homepage=REPO_URL,
                comment='The MoltBrowser-owned parts of a %s build.'
                        % args.platform)
            default = '%s-%s-%s-repo.spdx.json' % (PRODUCT, version, args.platform)
            out_path = args.out or os.path.join(args.out_dir or '.', default)
            check_sbom_name(os.path.basename(out_path))
            doc_name = '%s-%s-%s-repo' % (PRODUCT, version, args.platform)
        # Linux: the distribution's tor is an optional runtime dependency,
        # not something the artifact contains.
        rels = [(c, 'OPTIONAL_DEPENDENCY_OF', root) for c in optional]
        doc = build_document(doc_name, root, components, licenses, coverage,
                             rels, not_contained=optional)
        problems = validate_document(doc)
        if problems:
            raise Error('generated SBOM is invalid:\n  ' + '\n  '.join(problems))
        write_json(out_path, doc)
        outputs.append(out_path)
    for path in outputs:
        print(path)
    if args.release and warnings:
        raise Error('%d warning(s) in a release SBOM' % len(warnings))
    return outputs


# ------------------------------------------------------------------ validate

SPDXID_RE = re.compile(r'^SPDXRef-[A-Za-z0-9.-]+$')
CREATED_RE = re.compile(r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$')
HEX_LEN = {'SHA1': 40, 'SHA224': 56, 'SHA256': 64, 'SHA384': 96,
           'SHA512': 128, 'MD5': 32, 'SHA3-256': 64, 'SHA3-384': 96,
           'SHA3-512': 128, 'BLAKE2b-256': 64, 'BLAKE2b-384': 96,
           'BLAKE2b-512': 128, 'BLAKE3': 64, 'MD2': 32, 'MD4': 32, 'MD6': None,
           'ADLER32': 8}
PURPOSES = {'APPLICATION', 'FRAMEWORK', 'LIBRARY', 'CONTAINER',
            'OPERATING-SYSTEM', 'DEVICE', 'FIRMWARE', 'SOURCE', 'ARCHIVE',
            'FILE', 'INSTALL', 'OTHER'}
REF_CATEGORIES = {'SECURITY', 'PACKAGE-MANAGER', 'PERSISTENT-ID', 'OTHER'}
RELATIONSHIP_TYPES = set("""
DESCRIBES DESCRIBED_BY CONTAINS CONTAINED_BY DEPENDS_ON DEPENDENCY_OF
DEPENDENCY_MANIFEST_OF BUILD_DEPENDENCY_OF DEV_DEPENDENCY_OF
OPTIONAL_DEPENDENCY_OF PROVIDED_DEPENDENCY_OF TEST_DEPENDENCY_OF
RUNTIME_DEPENDENCY_OF EXAMPLE_OF GENERATES GENERATED_FROM ANCESTOR_OF
DESCENDANT_OF VARIANT_OF DISTRIBUTION_ARTIFACT PATCH_FOR PATCH_APPLIED
COPY_OF FILE_ADDED FILE_DELETED FILE_MODIFIED EXPANDED_FROM_ARCHIVE
DYNAMIC_LINK STATIC_LINK DATA_FILE_OF TEST_CASE_OF BUILD_TOOL_OF
DEV_TOOL_OF TEST_OF TEST_TOOL_OF DOCUMENTATION_OF OPTIONAL_COMPONENT_OF
METAFILE_OF PACKAGE_OF AMENDS PREREQUISITE_FOR HAS_PREREQUISITE
REQUIREMENT_DESCRIPTION_FOR SPECIFICATION_FOR OTHER
""".split())


def _check_checksums(where, checksums, problems, need_sha1=False):
    if not isinstance(checksums, list) or not checksums:
        problems.append('%s: checksums missing' % where)
        return {}
    seen = {}
    for c in checksums:
        alg = c.get('algorithm') if isinstance(c, dict) else None
        val = c.get('checksumValue') if isinstance(c, dict) else None
        if alg not in HEX_LEN or not isinstance(val, str) \
                or not re.fullmatch(r'[0-9a-f]+', val) \
                or (HEX_LEN[alg] and len(val) != HEX_LEN[alg]):
            problems.append('%s: malformed checksum %r' % (where, c))
            continue
        seen[alg] = val
    if need_sha1 and 'SHA1' not in seen:
        problems.append('%s: a file needs a SHA1 checksum' % where)
    return seen


def _check_spdx_value(where, field, value, problems):
    if not isinstance(value, str) or not value:
        problems.append('%s: %s missing' % (where, field))
        return
    if value in ('NONE', 'NOASSERTION'):
        return
    if field == 'downloadLocation' and not re.match(
            r'^((git|hg|svn|bzr)\+)?(https?|ssh|git|ftp|file)://\S+$', value):
        problems.append('%s: downloadLocation %r is not a URL' % (where, value))
    if field == 'supplier' and not re.match(r'^(Person|Organization): \S', value):
        problems.append('%s: supplier %r must start with Person: / '
                        'Organization:' % (where, value))


def validate_document(doc):
    """Structural SPDX 2.3 problems (empty list if none)."""
    problems = []
    if not isinstance(doc, dict):
        return ['not a JSON object']
    expect = {'spdxVersion': 'SPDX-2.3', 'dataLicense': 'CC0-1.0',
              'SPDXID': 'SPDXRef-DOCUMENT'}
    for k, v in expect.items():
        if doc.get(k) != v:
            problems.append('%s must be %r' % (k, v))
    if not isinstance(doc.get('name'), str) or not doc['name']:
        problems.append('document name missing')
    ns = doc.get('documentNamespace')
    if not isinstance(ns, str) or not re.match(r'^[a-z][a-z0-9+.-]*://\S+$', ns) \
            or '#' in ns:
        problems.append('documentNamespace must be an absolute URI without "#"')
    ci = doc.get('creationInfo') or {}
    if not CREATED_RE.match(str(ci.get('created', ''))):
        problems.append('creationInfo.created must be YYYY-MM-DDThh:mm:ssZ')
    creators = ci.get('creators')
    if not isinstance(creators, list) or not creators or not all(
            isinstance(c, str) and re.match(r'^(Tool|Organization|Person): \S', c)
            for c in creators):
        problems.append('creationInfo.creators must list Tool:/Organization:/'
                        'Person: entries')

    declared_refs = set()
    for e in doc.get('hasExtractedLicensingInfos') or []:
        lid = e.get('licenseId', '') if isinstance(e, dict) else ''
        if not re.fullmatch(r'LicenseRef-[A-Za-z0-9.-]+', lid):
            problems.append('bad extracted licence id %r' % lid)
        elif lid in declared_refs:
            problems.append('licence %s declared twice' % lid)
        if not isinstance(e, dict) or not e.get('extractedText'):
            problems.append('%s has no extractedText' % lid)
        declared_refs.add(lid)

    ids = {'SPDXRef-DOCUMENT': 'document'}
    packages = doc.get('packages')
    if not isinstance(packages, list) or not packages:
        problems.append('no packages')
        packages = []
    for p in packages:
        if not isinstance(p, dict):
            problems.append('package entry is not an object')
            continue
        pid = p.get('SPDXID', '')
        where = 'package %s' % (pid or p.get('name'))
        if not SPDXID_RE.match(str(pid)):
            problems.append('%s: bad SPDXID' % where)
        elif pid in ids:
            problems.append('%s: duplicate SPDXID' % where)
        ids[pid] = 'package'
        if not isinstance(p.get('name'), str) or not p['name']:
            problems.append('%s: name missing' % where)
        _check_spdx_value(where, 'downloadLocation', p.get('downloadLocation'),
                          problems)
        if 'supplier' in p:
            _check_spdx_value(where, 'supplier', p['supplier'], problems)
        if not isinstance(p.get('filesAnalyzed'), bool):
            problems.append('%s: filesAnalyzed must be true or false' % where)
        for field in ('licenseConcluded', 'licenseDeclared'):
            if field in p:
                for prob in check_license_expression(str(p[field]), declared_refs):
                    problems.append('%s: %s: %s' % (where, field, prob))
        if 'checksums' in p:
            _check_checksums(where, p['checksums'], problems)
        if p.get('primaryPackagePurpose', 'OTHER') not in PURPOSES:
            problems.append('%s: bad primaryPackagePurpose' % where)
        for ref in p.get('externalRefs') or []:
            if not isinstance(ref, dict) or ref.get('referenceCategory') not in REF_CATEGORIES \
                    or not ref.get('referenceType') or not ref.get('referenceLocator') \
                    or re.search(r'\s', str(ref.get('referenceLocator'))):
                problems.append('%s: bad externalRef %r' % (where, ref))
            elif ref['referenceType'] == 'purl' and not str(
                    ref['referenceLocator']).startswith('pkg:'):
                problems.append('%s: bad purl %r' % (where, ref['referenceLocator']))
        if p.get('filesAnalyzed') is True and not (
                p.get('packageVerificationCode') or {}).get(
                    'packageVerificationCodeValue'):
            problems.append('%s: filesAnalyzed needs packageVerificationCode' % where)

    file_sha1 = {}
    for f in doc.get('files') or []:
        fid = f.get('SPDXID', '') if isinstance(f, dict) else ''
        where = 'file %s' % fid
        if not SPDXID_RE.match(str(fid)):
            problems.append('%s: bad SPDXID' % where)
        elif fid in ids:
            problems.append('%s: duplicate SPDXID' % where)
        ids[fid] = 'file'
        if not str(f.get('fileName', '')).startswith('./'):
            problems.append('%s: fileName must start with ./' % where)
        sums = _check_checksums(where, f.get('checksums'), problems, need_sha1=True)
        file_sha1[fid] = sums.get('SHA1')
        if 'licenseConcluded' in f:
            for prob in check_license_expression(str(f['licenseConcluded']),
                                                 declared_refs):
                problems.append('%s: %s' % (where, prob))

    contains = {}
    describes = 0
    for r in doc.get('relationships') or []:
        if not isinstance(r, dict):
            problems.append('relationship is not an object')
            continue
        a, t, b = (r.get('spdxElementId'), r.get('relationshipType'),
                   r.get('relatedSpdxElement'))
        if t not in RELATIONSHIP_TYPES:
            problems.append('bad relationship type %r' % t)
        for e in (a, b):
            if e not in ids and e not in ('NONE', 'NOASSERTION'):
                problems.append('relationship refers to unknown %r' % e)
        if a == 'SPDXRef-DOCUMENT' and t == 'DESCRIBES':
            describes += 1
        if t == 'CONTAINS' and ids.get(b) == 'file':
            contains.setdefault(a, []).append(b)
    if describes == 0:
        problems.append('the document DESCRIBES nothing')
    for p in packages:
        if not isinstance(p, dict):
            continue
        pid = p.get('SPDXID')
        mine = contains.get(pid, [])
        if p.get('filesAnalyzed') is False and mine:
            problems.append('package %s: filesAnalyzed is false but it '
                            'CONTAINS files' % pid)
        code = (p.get('packageVerificationCode') or {}).get(
            'packageVerificationCodeValue')
        if p.get('filesAnalyzed') is True and code:
            sha1s = [file_sha1.get(f) for f in mine]
            if None in sha1s or not sha1s:
                problems.append('package %s: cannot recompute its verification '
                                'code' % pid)
            else:
                want = hashlib.sha1(''.join(sorted(sha1s)).encode()).hexdigest()
                if want != code:
                    problems.append('package %s: packageVerificationCode is %s, '
                                    'files give %s' % (pid, code, want))
    used = json.dumps(packages) + json.dumps(doc.get('files') or [])
    for ref in sorted(declared_refs):
        if ref not in used:
            problems.append('%s is declared but never used' % ref)
    return problems


def validate_molt(doc, sbom_path=None, artifact=None, chromium_version=None):
    """MoltBrowser's own rules for a release SBOM."""
    problems = []
    if sbom_path:
        try:
            check_sbom_name(os.path.basename(sbom_path))
        except Error as e:
            problems.append(str(e))
    by_id = {p.get('SPDXID'): p for p in doc.get('packages') or []
             if isinstance(p, dict)}
    described = [r['relatedSpdxElement'] for r in doc.get('relationships') or []
                 if r.get('spdxElementId') == 'SPDXRef-DOCUMENT'
                 and r.get('relationshipType') == 'DESCRIBES']
    if len(described) != 1 or described[0] not in by_id:
        problems.append('the SBOM must describe exactly one package')
        return problems
    root = by_id[described[0]]
    contained = {r['relatedSpdxElement'] for r in doc.get('relationships') or []
                 if r.get('spdxElementId') == root['SPDXID']
                 and r.get('relationshipType') == 'CONTAINS'}
    chromium = [by_id[i] for i in contained
                if i in by_id and by_id[i].get('name') == 'Chromium']
    if len(chromium) != 1:
        problems.append('the artifact must contain exactly one Chromium package')
    elif chromium_version and chromium[0].get('versionInfo') != chromium_version:
        problems.append('Chromium is %s in the SBOM, %s in chromium_version.txt'
                        % (chromium[0].get('versionInfo'), chromium_version))
    if artifact:
        sums = {c['algorithm']: c['checksumValue']
                for c in root.get('checksums') or []}
        if root.get('packageFileName') != os.path.basename(artifact):
            problems.append('the SBOM describes %r, not %r' % (
                root.get('packageFileName'), os.path.basename(artifact)))
        elif sums.get('SHA256') != file_digests(artifact)['SHA256']:
            problems.append('%s does not match the SHA-256 in the SBOM'
                            % artifact)
        if sbom_path and os.path.basename(sbom_path) != sbom_name_for(
                os.path.basename(artifact)):
            problems.append('%s should be named %s' % (
                sbom_path, sbom_name_for(os.path.basename(artifact))))
    return problems


def described_file_name(doc):
    """packageFileName of the package the SBOM DESCRIBES, or None."""
    for r in doc.get('relationships') or []:
        if r.get('spdxElementId') == 'SPDXRef-DOCUMENT' \
                and r.get('relationshipType') == 'DESCRIBES':
            for p in doc.get('packages') or []:
                if p.get('SPDXID') == r.get('relatedSpdxElement'):
                    return p.get('packageFileName')
    return None


def validate(args):
    bad = 0
    chromium_version = None
    try:
        chromium_version = read_chromium_pin(os.path.abspath(args.repo))[0]
    except (OSError, Error):
        pass
    if args.artifact and len(args.artifact) != len(args.sbom):
        raise Error('give one --artifact per SBOM, in the same order')
    for i, path in enumerate(args.sbom):
        try:
            with open(path, encoding='utf-8') as f:
                doc = json.load(f)
        except (OSError, ValueError) as e:
            print('%s: cannot read: %s' % (path, e), file=sys.stderr)
            bad += 1
            continue
        problems = validate_document(doc)
        artifact = args.artifact[i] if args.artifact else None
        if args.artifacts_dir and not problems:
            name = described_file_name(doc)
            artifact = os.path.join(args.artifacts_dir, name or '')
            if not name or not os.path.isfile(artifact):
                problems = ['%s: the installer it describes (%s) is not in %s'
                            % (path, name, args.artifacts_dir)]
        if not problems:
            problems = validate_molt(
                doc, path, artifact,
                None if args.any_chromium else chromium_version)
        if problems:
            bad += 1
            print('%s: INVALID' % path, file=sys.stderr)
            for p in problems:
                print('  ' + p, file=sys.stderr)
        else:
            n = len(doc.get('packages', []))
            print('%s: OK (%d packages, %d files)'
                  % (path, n, len(doc.get('files') or [])))
    return 1 if bad else 0


# ---------------------------------------------------------------------- main


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = p.add_subparsers(dest='cmd', required=True)

    g = sub.add_parser('generate', help='write SBOMs')
    g.add_argument('--repo', default=REPO_ROOT,
                   help='the MoltBrowser repository (default: this one)')
    g.add_argument('--platform', required=True, choices=PLATFORMS)
    g.add_argument('--arch', default='x64', help='arm64, x64 or universal')
    g.add_argument('--version', help='release version or tag (v0.3.0)')
    g.add_argument('--artifact', action='append', default=[],
                   help='a release artifact (repeat; one SBOM each)')
    g.add_argument('--out-dir', help='where SBOMs go (default: next to the '
                                     'artifact)')
    g.add_argument('--out', help='--repo-only: output file')
    g.add_argument('--chromium-src', help='the Chromium checkout that was built')
    g.add_argument('--gn-out-dir', help='its build directory '
                                        '(default: CHROMIUM_SRC/out/MoltBrowser)')
    g.add_argument('--gn-target', default=DEFAULT_GN_TARGET)
    g.add_argument('--licenses-list',
                   help='saved output of `licenses.py list --verbose '
                        '--shipped-only` (instead of running it)')
    g.add_argument('--python', help='interpreter for licenses.py '
                                    '(default: this one)')
    g.add_argument('--payload', help='the shipped tree: MoltBrowser.app, or '
                                     'the Windows / Linux staging folder')
    g.add_argument('--repo-only', action='store_true',
                   help='MoltBrowser-owned parts only, from this repository')
    g.add_argument('--source-revision', help='MoltBrowser commit (default: '
                                             '$GITHUB_SHA or git HEAD)')
    g.add_argument('--no-brew', action='store_true',
                   help='do not ask Homebrew for the licences of bundled '
                        'libraries')
    g.add_argument('--release', action='store_true',
                   help='refuse an incomplete SBOM (release builds)')
    g.set_defaults(fn=generate)

    v = sub.add_parser('validate', help='check SBOMs')
    v.add_argument('sbom', nargs='+')
    v.add_argument('--artifact', action='append',
                   help='the artifact each SBOM describes (in order)')
    v.add_argument('--artifacts-dir',
                   help='or: find each SBOM\'s artifact here by its name')
    v.add_argument('--repo', default=REPO_ROOT)
    v.add_argument('--any-chromium', action='store_true',
                   help='do not compare Chromium with chromium_version.txt')
    v.set_defaults(fn=validate)

    args = p.parse_args(argv)
    try:
        result = args.fn(args)
    except Error as e:
        print('molt_sbom: error: %s' % e, file=sys.stderr)
        return 1
    return result if isinstance(result, int) else 0


if __name__ == '__main__':
    sys.exit(main())
