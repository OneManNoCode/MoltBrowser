#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for molt_sbom.py.

Builds a fake release around the fixtures in testdata/ (a MoltBrowser
repository, a Chromium checkout whose tools/licenses/licenses.py is a
stand-in, a MoltBrowser.app payload and an artifact), generates SBOMs and
checks them: against golden files, with the built-in validator, and, when
$MOLT_SPDX_TOOLS names the official SPDX validator (`pyspdxtools` from the
spdx-tools package), with that too. Also runs --repo-only against this
repository, so a renamed pin in a bundle script fails here, not in a
release.

  python3 tools/sbom/molt_sbom_test.py
  MOLT_SBOM_UPDATE_GOLDEN=1 python3 tools/sbom/molt_sbom_test.py   # rewrite
"""

import contextlib
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import molt_sbom as ms  # noqa: E402

TESTDATA = os.path.join(HERE, 'testdata')
GOLDEN = os.path.join(TESTDATA, 'golden')
REPO_ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
EPOCH = '1790000000'  # 2026-09-21T14:13:20Z
SOURCE_REV = 'a' * 40
SPDX_TOOLS = os.environ.get('MOLT_SPDX_TOOLS', '')
if os.environ.get('MOLT_REQUIRE_SPDX_TOOLS') == '1' and not (
        SPDX_TOOLS and os.access(SPDX_TOOLS, os.X_OK)):
    sys.exit('MOLT_REQUIRE_SPDX_TOOLS=1 but $MOLT_SPDX_TOOLS is not an '
             'executable pyspdxtools')

# A Mach-O header for arm64 (MH_MAGIC_64, CPU_TYPE_ARM64).
ARM64_MACHO = b'\xcf\xfa\xed\xfe\x0c\x00\x00\x01'

MAC_PAYLOAD = {
    'Contents/MacOS/MoltBrowser': b'launcher',
    'Contents/Resources/en.lproj/InfoPlist.strings': b'not a helper',
    'Contents/Resources/tor/tor': ARM64_MACHO + b' fake tor',
    'Contents/Resources/tor/libevent-2.1.7.dylib': b'fake libevent',
    'Contents/Resources/tor/geoip': b'fake geoip\n',
    'Contents/Resources/tor/geoip6': b'fake geoip6\n',
    'Contents/Resources/tor/.version': b'15.0.15\n',
    'Contents/Resources/tor/pluggable_transports/lyrebird': b'fake lyrebird',
    'Contents/Resources/tor/pluggable_transports/snowflake-client': b'fake sf',
    'Contents/Resources/tor/pluggable_transports/pt_config.json': b'{}\n',
    'Contents/Resources/ocr/tesseract': ARM64_MACHO + b' fake tesseract',
    'Contents/Resources/ocr/.version': b'5.5.0\n',
    'Contents/Resources/ocr/lib/libtesseract.5.dylib': b'fake libtesseract',
    'Contents/Resources/ocr/lib/libleptonica.6.dylib': b'fake leptonica',
    'Contents/Resources/ocr/lib/libpng16.16.dylib': b'fake libpng',
    'Contents/Resources/ocr/tessdata/eng.traineddata': b'fake eng.traineddata\n',
    'Contents/Resources/whisper/whisper-cli': ARM64_MACHO + b' fake whisper',
    'Contents/Resources/whisper/ggml-tiny.en.bin': b'fake ggml-tiny.en.bin\n',
    'Contents/Resources/whisper/.version': b'v1.7.5\n',
    'Contents/Resources/whisper/lib/libomp.dylib': b'fake libomp',
    'Contents/Resources/molt_models/tinyllama-1.1b.gguf': b'GGUF fake tinyllama\n',
}
OCR_PROVENANCE = {
    'format': 1,
    'binary': {'file': 'tesseract',
               'origin': '/opt/homebrew/Cellar/tesseract/5.5.0/bin/tesseract'},
    'libraries': [
        {'file': 'lib/libtesseract.5.dylib',
         'origin': '/opt/homebrew/Cellar/tesseract/5.5.0/lib/libtesseract.5.dylib'},
        {'file': 'lib/libleptonica.6.dylib',
         'origin': '/opt/homebrew/Cellar/leptonica/1.85.0/lib/libleptonica.6.dylib'},
        {'file': 'lib/libpng16.16.dylib',
         'origin': '/opt/homebrew/Cellar/libpng/1.6.47/lib/libpng16.16.dylib'},
    ],
}
WHISPER_PROVENANCE = {
    'format': 1,
    'binary': {'file': 'whisper-cli',
               'origin': '/build/whisper.cpp-v1.7.5/build/bin/whisper-cli'},
    'libraries': [
        {'file': 'lib/libomp.dylib',
         'origin': '/opt/homebrew/Cellar/libomp/20.1.8/lib/libomp.dylib'},
    ],
}
WIN_PAYLOAD = {
    'MoltBrowser.exe': b'fake exe',
    'tor/tor.exe': b'fake tor.exe',
    'tor/geoip': b'fake geoip\n',
    'ocr/tesseract.exe': b'fake tesseract.exe',
    'ocr/libtesseract-5.dll': b'fake dll',
    'ocr/tessdata/eng.traineddata': b'installer traineddata',
    'whisper/whisper-cli.exe': b'fake whisper-cli.exe',
    'whisper/whisper.dll': b'fake whisper.dll',
    'whisper/ggml-tiny.en.bin': b'fake ggml-tiny.en.bin\n',
}


def write_tree(root, files):
    for rel, data in files.items():
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)


@contextlib.contextmanager
def env(**values):
    saved = {k: os.environ.get(k) for k in values}
    for k, v in values.items():
        if v is None:
            os.environ.pop(k, None)
        else:
            os.environ[k] = v
    try:
        yield
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


def run_main(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = ms.main(argv)
    return code, out.getvalue(), err.getvalue()


class Fixture:
    def __init__(self, tmp):
        self.tmp = tmp
        self.repo = os.path.join(tmp, 'repo')
        self.chromium = os.path.join(tmp, 'chromium')
        shutil.copytree(os.path.join(TESTDATA, 'repo'), self.repo)
        shutil.copytree(os.path.join(TESTDATA, 'chromium'), self.chromium)
        self.out_dir = os.path.join(self.chromium, 'out', 'MoltBrowser')
        os.makedirs(self.out_dir)
        with open(os.path.join(self.out_dir, 'args.gn'), 'w') as f:
            f.write('is_official_build = true\n')
        self.dist = os.path.join(tmp, 'dist')
        os.makedirs(self.dist)

    def mac_app(self):
        app = os.path.join(self.tmp, 'MoltBrowser.app')
        write_tree(app, MAC_PAYLOAD)
        with open(os.path.join(app, 'Contents/Resources/ocr/'
                               '.bundle-provenance.json'), 'w') as f:
            json.dump(OCR_PROVENANCE, f)
        with open(os.path.join(app, 'Contents/Resources/whisper/'
                               '.bundle-provenance.json'), 'w') as f:
            json.dump(WHISPER_PROVENANCE, f)
        return app

    def win_stage(self):
        stage = os.path.join(self.tmp, 'MoltBrowser-Windows-x64')
        write_tree(stage, WIN_PAYLOAD)
        return stage

    def artifact(self, name, data=b'fake installer bytes\n'):
        path = os.path.join(self.dist, name)
        with open(path, 'wb') as f:
            f.write(data)
        return path

    def generate(self, platform, artifacts, *extra, payload=None, release=True,
                 arch='arm64'):
        argv = ['generate', '--repo', self.repo, '--platform', platform,
                '--arch', arch, '--version', 'v0.3.0', '--source-revision',
                SOURCE_REV, '--chromium-src', self.chromium, '--gn-out-dir',
                self.out_dir, '--no-brew']
        for a in artifacts:
            argv += ['--artifact', a]
        if payload:
            argv += ['--payload', payload]
        if release:
            argv.append('--release')
        argv += list(extra)
        with env(SOURCE_DATE_EPOCH=EPOCH, GITHUB_SHA=None):
            return run_main(argv)


def load(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def read(path, mode='r'):
    with open(path, mode) as f:
        return f.read()


def by_name(doc):
    return {p['name']: p for p in doc['packages']}


class SbomTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='molt-sbom-test-')
        self.addCleanup(shutil.rmtree, self.tmp)
        self.fx = Fixture(self.tmp)

    def assertValid(self, path, artifact=None):
        doc = load(path)
        self.assertEqual(ms.validate_document(doc), [])
        self.assertEqual(ms.validate_molt(doc, path, artifact,
                                          '152.0.7977.169'), [])
        if SPDX_TOOLS:
            proc = subprocess.run([SPDX_TOOLS, '-i', path],
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT,
                                  universal_newlines=True)
            self.assertEqual(proc.returncode, 0, proc.stdout)
        return doc

    def assertGolden(self, path):
        golden = os.path.join(GOLDEN, os.path.basename(path))
        if os.environ.get('MOLT_SBOM_UPDATE_GOLDEN') == '1':
            shutil.copyfile(path, golden)
        self.assertEqual(load(path), load(golden),
                         'differs from %s (MOLT_SBOM_UPDATE_GOLDEN=1 rewrites '
                         'it; review the diff)' % golden)


class NameTest(unittest.TestCase):
    def test_names_avoid_updater_matchers(self):
        cases = {
            'MoltBrowser-macOS-arm64.dmg': 'MoltBrowser-macOS-arm64-dmg.spdx.json',
            'MoltBrowser-Linux-x64.deb': 'MoltBrowser-Linux-x64-deb.spdx.json',
            'MoltBrowser-Linux-x64.rpm': 'MoltBrowser-Linux-x64-rpm.spdx.json',
            'MoltBrowser-Linux-x64.tar.gz':
                'MoltBrowser-Linux-x64-tar-gz.spdx.json',
            'MoltBrowser-Windows-x64-Setup.exe':
                'MoltBrowser-Windows-x64-Setup-exe.spdx.json',
            'MoltBrowser-0.3.0-linux-x64.AppImage':
                'MoltBrowser-0-3-0-linux-x64-AppImage.spdx.json',
        }
        for artifact, sbom in cases.items():
            self.assertEqual(ms.sbom_name_for(artifact), sbom)
            for m in ms.UPDATER_ASSET_MATCHERS:
                self.assertNotIn(m, sbom)

    def test_rejects_names_the_updater_would_pick(self):
        for bad in ('x.deb.spdx.json', 'MoltBrowser-Setup.exe.json',
                    'a.tar.gz.spdx.json', 'with space.spdx.json'):
            with self.assertRaises(ms.Error):
                ms.check_sbom_name(bad)


class ParserTest(unittest.TestCase):
    def test_readme_with_two_dependencies(self):
        deps = ms.parse_readme_chromium(read(os.path.join(
            TESTDATA, 'chromium/third_party/libxml/README.chromium')))
        self.assertEqual([d['Name'] for d in deps], ['libxml', 'libxml test data'])
        self.assertEqual(deps[0]['CPEPrefix'],
                         'cpe:2.3:a:xmlsoft:libxml2:2.14.0:*:*:*:*:*:*:*')
        self.assertEqual(deps[1]['Version'], 'N/A')
        self.assertIsNone(ms.meaningful(deps[1]['Version']))

    def test_readme_ignores_unknown_fields(self):
        dep = ms.parse_readme_chromium(read(os.path.join(
            TESTDATA, 'repo/src/third_party/llama_cpp/README.chromium')))[0]
        self.assertEqual(dep['Version'], 'b11641')
        self.assertNotIn('Exceptions', dep)

    def test_cargo_files(self):
        cio = os.path.join(TESTDATA, 'repo/src/third_party/rust/chromium_crates_io')
        lock = ms.parse_cargo_lock(read(os.path.join(cio, 'Cargo.lock')))
        self.assertEqual(len(lock), 7)
        anydoc = [p for p in lock if p['name'] == 'anydoc'][0]
        self.assertEqual(anydoc['dependencies'], ['memchr', 'zip 8.6.0'])
        zip8 = [p for p in lock if p['name'] == 'zip' and p['version'] == '8.6.0'][0]
        self.assertEqual(zip8['dependencies'], ['memchr'])
        self.assertEqual(ms.molt_cargo_deps(read(os.path.join(cio, 'Cargo.toml'))),
                         {'adblock': '=0.13.3'})

    def test_crate_epoch(self):
        self.assertEqual(ms.crate_epoch('0.13.3'), 'v0_13')
        self.assertEqual(ms.crate_epoch('1.2.0'), 'v1')
        self.assertEqual(ms.crate_epoch('0.0.7'), 'v0_0_7')

    def test_pins(self):
        pins = ms.Pins(os.path.join(TESTDATA, 'repo'))
        self.assertEqual(pins.tor['PINNED_TOR_VERSION'], '15.0.15')
        self.assertNotIn('ARCHIVE_PATH', pins.tor)   # not a literal
        self.assertEqual(pins.whisper_commit('v1.7.5'),
                         '51c6961c7b64b406833f4b6a4a20e67142f69225')
        self.assertEqual(pins.windows_version('tor-'), (
            '15.0.15',
            'd59385129782a8a44f074a91de4aa3c794d69d0ecfca6c4b4975f775c55dbebe'))
        self.assertNotIn('not-a-pin', pins.windows)
        self.assertEqual(pins.windows['ggml-tiny.en.bin'][:8], 'c139ead9')
        self.assertEqual(sorted(pins.models), ['qwen3.5-4b', 'tinyllama-1.1b'])
        tiny = pins.models['tinyllama-1.1b']
        self.assertEqual((tiny['repo'], tiny['license'], tiny['size']),
                         ('TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF', 'apache-2.0', 20))
        qwen = pins.models['qwen3.5-4b']
        self.assertEqual((qwen['file'], qwen['sha256'], qwen['license']),
                         ('Qwen3.5-4B-Instruct-Q4_K_M.gguf', '', ''))

    def test_macho_arch(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        for name, data, want in (
                ('arm', ARM64_MACHO, 'aarch64'),
                ('x86', b'\xcf\xfa\xed\xfe\x07\x00\x00\x01', 'x86_64'),
                ('fat', b'\xca\xfe\xba\xbe\x00\x00\x00\x02', 'universal'),
                ('elf', b'\x7fELF\x02\x01\x01\x00', None)):
            path = os.path.join(tmp, name)
            with open(path, 'wb') as f:
                f.write(data)
            self.assertEqual(ms.macho_arch(path), want)


class LicenseTest(unittest.TestCase):
    def test_expressions(self):
        lic = ms.Licenses()
        cases = {
            'MIT': 'MIT',
            'Apache 2.0': 'Apache-2.0',
            'apache-2.0': 'Apache-2.0',
            'MIT OR Apache-2.0': 'MIT OR Apache-2.0',
            'MIT/Apache-2.0': 'MIT OR Apache-2.0',
            'Apache-2.0 WITH LLVM-exception': 'Apache-2.0 WITH LLVM-exception',
            'Apache 2.0, MIT': 'Apache-2.0 AND MIT',
            '(MIT OR Apache-2.0) AND Unicode-3.0':
                '(MIT OR Apache-2.0) AND Unicode-3.0',
            '': 'NOASSERTION',
            None: 'NOASSERTION',
            'BSD': 'LicenseRef-BSD',
            'Custom license': 'LicenseRef-Custom-license',
            'Apache 2.0, BSD': 'Apache-2.0 AND LicenseRef-BSD',
            'MIT OR Weird': 'LicenseRef-MIT-OR-Weird',
        }
        for raw, want in cases.items():
            self.assertEqual(lic.expression(raw), want, raw)
        refs = {e['licenseId'] for e in lic.extracted()}
        self.assertEqual(refs, {'LicenseRef-BSD', 'LicenseRef-Custom-license',
                                'LicenseRef-MIT-OR-Weird'})
        for e in lic.extracted():
            self.assertTrue(e['extractedText'])

    def test_colliding_refs_stay_distinct(self):
        lic = ms.Licenses()
        a = lic.expression('Foo bar')
        b = lic.expression('Foo/bar!')
        self.assertNotEqual(a, b)

    def test_checker(self):
        ok = ms.check_license_expression
        self.assertEqual(ok('MIT AND (Apache-2.0 OR BSD-3-Clause)', set()), [])
        self.assertEqual(ok('GPL-2.0-or-later WITH Classpath-exception-2.0',
                            set()), [])
        self.assertTrue(ok('MIT AND', set()))
        self.assertTrue(ok('(MIT', set()))
        self.assertTrue(ok('MIT Apache-2.0', set()))
        self.assertTrue(ok('NotALicense', set()))
        self.assertTrue(ok('LicenseRef-X', set()))
        self.assertEqual(ok('LicenseRef-X', {'LicenseRef-X'}), [])
        self.assertTrue(ok('MIT WITH NotAnException', set()))


class GenerateTest(SbomTestCase):
    def test_macos_release_sbom(self):
        dmg = self.fx.artifact('MoltBrowser-macOS-arm64.dmg')
        argv_log = os.path.join(self.tmp, 'argv.txt')
        with env(LICENSES_STUB_ARGV=argv_log):
            code, out, err = self.fx.generate('macos', [dmg],
                                              payload=self.fx.mac_app())
        self.assertEqual(code, 0, err)
        sbom = os.path.join(self.fx.dist, 'MoltBrowser-macOS-arm64-dmg.spdx.json')
        self.assertEqual(out.strip(), sbom)
        self.assertEqual(read(argv_log).split('\n')[:7], [
            'list', '--verbose', '--shipped-only', '--gn-out-dir',
            self.fx.out_dir, '--gn-target', '//chrome:chrome'])
        doc = self.assertValid(sbom, dmg)
        self.assertGolden(sbom)

        pkgs = by_name(doc)
        root = pkgs['MoltBrowser']
        self.assertEqual(root['packageFileName'], 'MoltBrowser-macOS-arm64.dmg')
        self.assertEqual({c['algorithm']: c['checksumValue']
                          for c in root['checksums']}['SHA256'],
                         hashlib.sha256(b'fake installer bytes\n').hexdigest())
        self.assertEqual(pkgs['Chromium']['versionInfo'], '152.0.7977.169')
        self.assertNotIn('The Chromium Project', pkgs)
        # Versions and CPEs come from README.chromium, licences from licenses.py.
        self.assertEqual(pkgs['zlib']['versionInfo'], '1.3.1')
        self.assertEqual(pkgs['zlib']['externalRefs'][0]['referenceType'],
                         'cpe22Type')
        self.assertEqual(pkgs['libxml test data']['versionInfo'],
                         '0123456789abcdef0123456789abcdef01234567')
        self.assertEqual(pkgs['libxml test data']['licenseDeclared'],
                         'Apache-2.0 AND LicenseRef-BSD')
        self.assertEqual(pkgs['BoringSSL']['licenseDeclared'], 'LicenseRef-BSDish')
        self.assertNotIn('versionInfo', pkgs['BoringSSL'])
        self.assertEqual(pkgs['adblock']['externalRefs'][0]['referenceLocator'],
                         'pkg:cargo/adblock@0.13.3')
        self.assertIn('vendored by MoltBrowser', pkgs['llama.cpp']['sourceInfo'])
        self.assertNotIn('vendored by MoltBrowser', pkgs['zlib']['sourceInfo'])
        # Helpers: pins, versions and every shipped file.
        tor = pkgs['Tor Expert Bundle']
        self.assertEqual(tor['packageFileName'],
                         'tor-expert-bundle-macos-aarch64-15.0.15.tar.gz')
        self.assertEqual(tor['checksums'][0]['checksumValue'][:8], 'd5938512')
        self.assertEqual(pkgs['tesseract']['versionInfo'], '5.5.0')
        self.assertEqual(pkgs['leptonica']['versionInfo'], '1.85.0')
        self.assertEqual(sum(p['name'] == 'tesseract' for p in doc['packages']), 1)
        self.assertEqual(pkgs['libomp']['versionInfo'], '20.1.8')
        self.assertEqual(pkgs['whisper.cpp']['downloadLocation'],
                         'git+https://github.com/ggerganov/whisper.cpp@'
                         '51c6961c7b64b406833f4b6a4a20e67142f69225')
        self.assertEqual(pkgs['TinyLlama 1.1B Chat']['licenseDeclared'], 'Apache-2.0')
        files = {f['fileName'] for f in doc['files']}
        for rel in MAC_PAYLOAD:
            if '/Resources/' in rel and '/en.lproj/' not in rel \
                    and not rel.endswith('.version'):
                self.assertIn('./MoltBrowser.app/' + rel, files)
        self.assertNotIn('./MoltBrowser.app/Contents/MacOS/MoltBrowser', files)
        self.assertNotIn(self.tmp, json.dumps(doc))

    def test_one_sbom_per_artifact(self):
        arts = [self.fx.artifact('MoltBrowser-Linux-x64.deb', b'deb'),
                self.fx.artifact('MoltBrowser-Linux-x64.rpm', b'rpm'),
                self.fx.artifact('MoltBrowser-Linux-x64.tar.gz', b'tgz')]
        code, out, err = self.fx.generate('linux', arts, arch='x64')
        self.assertEqual(code, 0, err)
        paths = out.split()
        self.assertEqual([os.path.basename(p) for p in paths], [
            'MoltBrowser-Linux-x64-deb.spdx.json',
            'MoltBrowser-Linux-x64-rpm.spdx.json',
            'MoltBrowser-Linux-x64-tar-gz.spdx.json'])
        for art, path in zip(arts, paths):
            doc = self.assertValid(path, art)
            tor = by_name(doc)['tor (distribution package)']
            rels = [(r['spdxElementId'], r['relationshipType'], r['relatedSpdxElement'])
                    for r in doc['relationships']]
            root = by_name(doc)['MoltBrowser']['SPDXID']
            self.assertIn((tor['SPDXID'], 'OPTIONAL_DEPENDENCY_OF', root), rels)
            self.assertNotIn((root, 'CONTAINS', tor['SPDXID']), rels)
            self.assertNotIn('files', doc)
        self.assertGolden(paths[0])
        self.assertEqual(by_name(load(paths[2]))['MoltBrowser']['primaryPackagePurpose'],
                         'ARCHIVE')

    def test_windows_payload(self):
        exe = self.fx.artifact('MoltBrowser-Windows-x64-Setup.exe')
        code, out, err = self.fx.generate('windows', [exe], arch='x64',
                                          payload=self.fx.win_stage())
        self.assertEqual(code, 0, err)
        doc = self.assertValid(out.strip(), exe)
        pkgs = by_name(doc)
        self.assertEqual(pkgs['Tor Expert Bundle']['packageFileName'],
                         'tor-expert-bundle-windows-x86_64-15.0.15.tar.gz')
        self.assertEqual(pkgs['Tesseract OCR (UB Mannheim build)']['versionInfo'],
                         '5.4.0.20240606')
        self.assertEqual(pkgs['whisper.cpp']['versionInfo'], '1.9.1')
        self.assertIn('whisper ggml-tiny.en model', pkgs)
        files = {f['fileName'] for f in doc['files']}
        self.assertIn('./MoltBrowser-Windows-x64/ocr/libtesseract-5.dll', files)
        self.assertNotIn('./MoltBrowser-Windows-x64/MoltBrowser.exe', files)

    def test_pin_mismatch_fails(self):
        app = self.fx.mac_app()
        with open(os.path.join(app, 'Contents/Resources/ocr/tessdata/'
                               'eng.traineddata'), 'ab') as f:
            f.write(b'tampered')
        dmg = self.fx.artifact('MoltBrowser-macOS-arm64.dmg')
        code, _, err = self.fx.generate('macos', [dmg], payload=app,
                                        release=False)
        self.assertEqual(code, 1)
        self.assertIn('does not match the pin', err)

    def test_unpinned_file_fails_a_release_but_not_a_dev_build(self):
        app = self.fx.mac_app()
        path = os.path.join(self.fx.repo, 'scripts', 'bundle-whisper.sh')
        text = read(path).replace(
            'c139ead9d2bfac381db38e15adeaf4fd0cf6e56d1cd948c822ca3e11bfc2768d', '')
        with open(path, 'w') as f:
            f.write(text)
        dmg = self.fx.artifact('MoltBrowser-macOS-arm64.dmg')
        code, _, err = self.fx.generate('macos', [dmg], payload=app)
        self.assertEqual(code, 1)
        self.assertIn('no pinned SHA-256', err)
        code, out, err = self.fx.generate('macos', [dmg], payload=app,
                                          release=False)
        self.assertEqual(code, 0, err)
        self.assertIn('warning: whisper model', err)
        self.assertValid(out.strip(), dmg)

    def test_unknown_helper_file_fails(self):
        app = self.fx.mac_app()
        write_tree(app, {'Contents/Resources/ocr/tessdata/osd.traineddata': b'x'})
        dmg = self.fx.artifact('MoltBrowser-macOS-arm64.dmg')
        code, _, err = self.fx.generate('macos', [dmg], payload=app)
        self.assertEqual(code, 1)
        self.assertIn('unrecognised files', err)

    def test_release_needs_a_complete_picture(self):
        dmg = self.fx.artifact('MoltBrowser-macOS-arm64.dmg')
        code, _, err = self.fx.generate('macos', [dmg])
        self.assertEqual(code, 1)
        self.assertIn('needs --payload', err)
        with env(SOURCE_DATE_EPOCH=EPOCH):
            code, _, err = run_main(['generate', '--repo', self.fx.repo,
                                     '--platform', 'linux', '--version', '1.0.0',
                                     '--artifact', dmg, '--release'])
        self.assertEqual(code, 1)
        self.assertIn('needs the Chromium component list', err)
        with open(os.path.join(self.fx.chromium, 'chrome', 'VERSION'), 'w') as f:
            f.write('MAJOR=153\nMINOR=0\nBUILD=1\nPATCH=0\n')
        code, _, err = self.fx.generate('linux', [dmg], arch='x64')
        self.assertEqual(code, 1)
        self.assertIn('not the pinned 152.0.7977.169', err)

    def test_zip_with_helpers_needs_a_payload(self):
        import zipfile
        path = os.path.join(self.fx.dist, 'MoltBrowser-Windows-x64.zip')
        with zipfile.ZipFile(path, 'w') as z:
            z.writestr('MoltBrowser-0.3.0-win-x64/MoltBrowser.exe', b'x')
            z.writestr('MoltBrowser-0.3.0-win-x64/tor/tor.exe', b'x')
        code, _, err = self.fx.generate('windows', [path], arch='x64')
        self.assertEqual(code, 1)
        self.assertIn('bundles helpers (MoltBrowser-0.3.0-win-x64/tor/tor.exe)', err)
        with zipfile.ZipFile(path, 'w') as z:
            z.writestr('MoltBrowser-0.3.0-win-x64/MoltBrowser.exe', b'x')
        code, _, err = self.fx.generate('windows', [path], arch='x64')
        self.assertEqual(code, 0, err)

    def test_licenses_py_failure_is_reported(self):
        os.remove(os.path.join(self.fx.out_dir, 'args.gn'))
        dmg = self.fx.artifact('MoltBrowser-Linux-x64.deb')
        code, _, err = self.fx.generate('linux', [dmg], arch='x64')
        self.assertEqual(code, 1)
        self.assertIn('has no args.gn', err)

    def test_reproducible(self):
        deb = self.fx.artifact('MoltBrowser-Linux-x64.deb')
        self.fx.generate('linux', [deb], arch='x64')
        sbom = os.path.join(self.fx.dist, 'MoltBrowser-Linux-x64-deb.spdx.json')
        first = read(sbom, 'rb')
        self.fx.generate('linux', [deb], arch='x64')
        self.assertEqual(read(sbom, 'rb'), first)


class WrapperTest(SbomTestCase):
    @unittest.skipUnless(shutil.which('bash'), 'needs bash')
    def test_release_sbom_sh(self):
        """scripts/release-sbom.sh: one validated SBOM per artifact, paths
        on stdout (this repository's pins, the fixture checkout)."""
        arts = [self.fx.artifact('MoltBrowser-Linux-x64.deb', b'deb'),
                self.fx.artifact('MoltBrowser-Linux-x64.tar.gz', b'tgz')]
        proc = subprocess.run(
            ['bash', os.path.join(REPO_ROOT, 'scripts', 'release-sbom.sh'),
             '--platform', 'linux', '--arch', 'x64', '--version', 'v0.3.0']
            + arts, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True,
            env=dict(os.environ, CHROMIUM_SRC=self.fx.chromium,
                     SOURCE_DATE_EPOCH=EPOCH, MOLT_RELEASE_BUILD='1'))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        sboms = proc.stdout.split()
        self.assertEqual([os.path.basename(p) for p in sboms], [
            'MoltBrowser-Linux-x64-deb.spdx.json',
            'MoltBrowser-Linux-x64-tar-gz.spdx.json'])
        for art, sbom in zip(arts, sboms):
            self.assertValid(sbom, art)
        self.assertIn(': OK', proc.stderr)


class ValidateTest(SbomTestCase):
    def setUp(self):
        super().setUp()
        self.deb = self.fx.artifact('MoltBrowser-Linux-x64.deb', b'a deb')
        code, out, err = self.fx.generate('linux', [self.deb], arch='x64')
        self.assertEqual(code, 0, err)
        self.sbom = out.strip()
        self.doc = load(self.sbom)

    def validate(self, *extra):
        return run_main(['validate', '--repo', self.fx.repo, self.sbom] + list(extra))

    def test_cli(self):
        code, out, _ = self.validate('--artifact', self.deb)
        self.assertEqual(code, 0)
        self.assertIn('OK', out)

    def test_artifacts_dir(self):
        code, out, err = self.validate('--artifacts-dir', self.fx.dist)
        self.assertEqual(code, 0, err)
        os.remove(self.deb)
        code, _, err = self.validate('--artifacts-dir', self.fx.dist)
        self.assertEqual(code, 1)
        self.assertIn('is not in', err)

    def test_changed_artifact(self):
        with open(self.deb, 'ab') as f:
            f.write(b'!')
        code, _, err = self.validate('--artifact', self.deb)
        self.assertEqual(code, 1)
        self.assertIn('does not match the SHA-256', err)

    def test_wrong_chromium(self):
        with open(os.path.join(self.fx.repo, 'chromium_version.txt'), 'w') as f:
            f.write('CHROMIUM_VERSION=153.0.1.0\nCHROMIUM_REVISION=%s\n' % ('b' * 40))
        code, _, err = self.validate()
        self.assertEqual(code, 1)
        self.assertIn('153.0.1.0 in chromium_version.txt', err)

    def test_structural_problems(self):
        def broken(mutate):
            doc = json.loads(json.dumps(self.doc))
            mutate(doc)
            return ms.validate_document(doc)

        self.assertTrue(broken(lambda d: d.update(spdxVersion='SPDX-2.2')))
        self.assertTrue(broken(lambda d: d['packages'][1].update(SPDXID='bad id')))
        self.assertTrue(broken(lambda d: d['packages'][1].update(
            SPDXID=d['packages'][2]['SPDXID'])))
        self.assertTrue(broken(lambda d: d['packages'][1].update(
            licenseDeclared='MIT AND')))
        self.assertTrue(broken(lambda d: d['packages'][1].update(
            licenseDeclared='LicenseRef-Undeclared')))
        self.assertTrue(broken(lambda d: d['packages'][0]['checksums'][0].update(
            checksumValue='abc')))
        self.assertTrue(broken(lambda d: d['relationships'].append(
            {'spdxElementId': 'SPDXRef-DOCUMENT', 'relationshipType': 'CONTAINS',
             'relatedSpdxElement': 'SPDXRef-Nowhere'})))
        self.assertTrue(broken(lambda d: d['relationships'].pop(0)))
        self.assertTrue(broken(lambda d: d['creationInfo'].update(
            created='yesterday')))
        self.assertTrue(broken(lambda d: d['packages'][1].update(
            downloadLocation='not a url')))

    def test_verification_code_is_checked(self):
        dmg = self.fx.artifact('MoltBrowser-macOS-arm64.dmg')
        code, out, err = self.fx.generate('macos', [dmg], payload=self.fx.mac_app())
        self.assertEqual(code, 0, err)
        doc = load(out.strip())
        doc['files'][0]['checksums'][0]['checksumValue'] = '0' * 40
        problems = ms.validate_document(doc)
        self.assertTrue(any('packageVerificationCode' in p for p in problems),
                        problems)

    def test_badly_named_sbom(self):
        bad = os.path.join(self.fx.dist, 'MoltBrowser-Linux-x64.deb.spdx.json')
        shutil.copyfile(self.sbom, bad)
        code, _, err = run_main(['validate', '--repo', self.fx.repo, bad])
        self.assertEqual(code, 1)
        self.assertIn('in-app updater', err)


class RepoOnlyTest(SbomTestCase):
    def test_fixture_repo(self):
        out = os.path.join(self.tmp, 'MoltBrowser-0.3.0-macos-repo.spdx.json')
        with env(SOURCE_DATE_EPOCH=EPOCH, GITHUB_SHA=None):
            code, _, err = run_main(['generate', '--repo', self.fx.repo,
                                     '--repo-only', '--platform', 'macos',
                                     '--version', '0.3.0', '--source-revision',
                                     SOURCE_REV, '--out', out])
        self.assertEqual(code, 0, err)
        doc = self.assertValid(out)
        pkgs = {(p['name'], p.get('versionInfo')): p for p in doc['packages']}
        rel = {r['relatedSpdxElement']: r['relationshipType']
               for r in doc['relationships']}
        # Vendored in src/: contained. From Cargo.lock / Cargo.toml: depends on.
        self.assertEqual(rel[pkgs[('llama.cpp', 'b11641')]['SPDXID']], 'CONTAINS')
        self.assertEqual(rel[pkgs[('anydoc', '0.1.6')]['SPDXID']], 'CONTAINS')
        for crate in (('memchr', '2.8.2'), ('zip', '8.6.0'), ('seahash', '4.1.0'),
                      ('adblock', '0.13.3')):
            self.assertEqual(rel[pkgs[crate]['SPDXID']], 'DEPENDS_ON', crate)
        self.assertNotIn(('zip', '2.4.2'), pkgs)
        self.assertNotIn(('serde', '1.0.228'), pkgs)
        self.assertIn(('Tor Expert Bundle (aarch64)', '15.0.15'), pkgs)
        self.assertGolden(out)

    def test_this_repository(self):
        """The real pins and vendored components still parse."""
        pins = ms.Pins(REPO_ROOT)
        for var in ('PINNED_TOR_VERSION', 'TOR_VERSION',
                    'EXPECTED_SHA256_TOR_AARCH64', 'EXPECTED_SHA256_TOR_X86_64'):
            self.assertIn(var, pins.tor)
        self.assertIn('WHISPER_TAG', pins.whisper)
        self.assertTrue(pins.whisper_commit(pins.whisper['WHISPER_TAG']))
        self.assertIn('EXPECTED_SHA256_WHISPER_TINY_EN', pins.whisper)
        self.assertIn('MODEL_URL', pins.whisper)
        self.assertTrue(pins.tesseract.get('TESSDATA_COMMIT'))
        self.assertTrue(pins.tesseract.get('EXPECTED_SHA256_TESSDATA_ENG'))
        self.assertIn('tinyllama-1.1b', pins.models)
        self.assertTrue(pins.windows_version('tor-')[0])
        self.assertTrue(pins.windows_version('tesseract-')[0])
        self.assertTrue(pins.windows_version('whisper-bin-x64-')[0])
        llama = ms.parse_readme_chromium(read(os.path.join(
            REPO_ROOT, 'src/third_party/llama_cpp/README.chromium')))[0]
        for platform in ms.PLATFORMS:
            out = os.path.join(self.tmp, platform + '.spdx.json')
            with env(SOURCE_DATE_EPOCH=EPOCH):
                code, _, err = run_main(['generate', '--repo-only', '--platform',
                                         platform, '--out', out])
            self.assertEqual(code, 0, err)
            doc = self.assertValid(out)
            names = {p['name']: p for p in doc['packages']}
            self.assertEqual(names['llama.cpp']['versionInfo'], llama['Version'])
            self.assertIn('adblock', names)
            self.assertIn('anydoc', names)


if __name__ == '__main__':
    unittest.main()
