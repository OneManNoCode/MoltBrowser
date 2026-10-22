#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Stand-in for Chromium's tools/licenses/licenses.py (fixture for
tools/sbom/molt_sbom_test.py).

It accepts the arguments molt_sbom.py passes and prints what the real
`licenses.py list --verbose` prints: one Python dict per component, after
the Chromium entry (dir ''). Like the real one, it reads only Name, URL,
License, License File and Shipped. With $LICENSES_STUB_ARGV set, it writes
its arguments there, one per line.
"""

import argparse
import os
import sys

p = argparse.ArgumentParser()
p.add_argument('command')
p.add_argument('--verbose', action='store_true')
p.add_argument('--shipped-only', action='store_true')
p.add_argument('--gn-out-dir', required=True)
p.add_argument('--gn-target', required=True)
p.add_argument('--target-os')
args = p.parse_args()

if os.environ.get('LICENSES_STUB_ARGV'):
    with open(os.environ['LICENSES_STUB_ARGV'], 'w') as f:
        f.write('\n'.join(sys.argv[1:]) + '\n')
if args.command != 'list' or not args.verbose or not args.shipped_only:
    sys.exit('stub: expected `list --verbose --shipped-only`')
if not os.path.isfile(os.path.join(args.gn_out_dir, 'args.gn')):
    sys.exit('stub: no args.gn in ' + args.gn_out_dir)

ENTRIES = [
    {'Name': 'The Chromium Project', 'URL': 'https://www.chromium.org',
     'License': 'BSD-3-Clause', 'Shipped': 'yes', 'License File': ['LICENSE'],
     'dir': ''},
    {'Name': 'adblock', 'URL': 'https://crates.io/crates/adblock',
     'License': 'MPL-2.0', 'Shipped': 'yes',
     'License File': ['third_party/rust/chromium_crates_io/vendor/adblock-v0_13/LICENSE'],
     'dir': os.path.join('third_party', 'rust', 'adblock', 'v0_13')},
    {'Name': 'anydoc', 'URL': 'https://crates.io/crates/anydoc',
     'License': 'MIT', 'Shipped': 'yes',
     'License File': ['third_party/rust/chromium_crates_io/vendor/anydoc-v0_1/LICENSE'],
     'dir': os.path.join('third_party', 'rust', 'anydoc', 'v0_1')},
    {'Name': 'BoringSSL', 'URL': 'https://boringssl.googlesource.com/boringssl/',
     'License': 'BSDish', 'Shipped': 'yes',
     'License File': ['third_party/boringssl/src/LICENSE'],
     'dir': os.path.join('third_party', 'boringssl')},
    {'Name': 'libxml', 'URL': 'http://xmlsoft.org', 'License': 'MIT',
     'Shipped': 'yes', 'License File': ['third_party/libxml/src/Copyright'],
     'dir': os.path.join('third_party', 'libxml')},
    {'Name': 'libxml test data', 'URL': 'http://xmlsoft.org',
     'License': 'Apache 2.0, BSD', 'Shipped': 'yes',
     'License File': ['third_party/libxml/LICENSE.test'],
     'dir': os.path.join('third_party', 'libxml')},
    {'Name': 'llama.cpp', 'URL': 'https://github.com/ggml-org/llama.cpp',
     'License': 'MIT', 'Shipped': 'yes',
     'License File': ['third_party/llama_cpp/LICENSE'],
     'dir': os.path.join('third_party', 'llama_cpp')},
    {'Name': 'zlib', 'URL': 'http://zlib.net/', 'License': 'Zlib',
     'Shipped': 'yes', 'License File': ['third_party/zlib/LICENSE'],
     'dir': os.path.join('third_party', 'zlib')},
]
print('Some warning line the parser must skip')
for entry in ENTRIES:
    print(entry)
