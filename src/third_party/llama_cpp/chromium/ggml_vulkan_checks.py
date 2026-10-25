#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Build-time checks for ggml-vulkan's shaders (GN switch molt_llama_vulkan).

1. Every shader extension BUILD.gn enables (GGML_VULKAN_<X>_GLSLC_SUPPORT)
   still compiles with ggml_vk_glslc, i.e. with the glslang Chromium pins.
   Upstream's CMake probes this at configure time and quietly drops a shader
   family the compiler can't build; here the list is fixed in BUILD.gn, so a
   glslang roll that loses an extension fails the build instead.
2. BUILD.gn's list of .comp shaders is exactly the directory's (GN has no
   globbing), so a llama.cpp update can't silently leave a shader out (its
   pipelines would then fail to link) or keep a removed one.
3. Every weight type BUILD.gn drops (_ggml_vk_dropped_types) is one the
   generator names its variants after (its type_names list), and not one
   every model needs (f32, f16): a typo would otherwise drop nothing, and a
   renamed type would silently come back.

Writes |--stamp| on success. Prints only on failure.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

# GGML_VULKAN_<feature>_GLSLC_SUPPORT -> its test in vulkan-shaders/feature-tests
# (ggml/src/ggml-vulkan/CMakeLists.txt, test_shader_extension_support).
FEATURE_TESTS = {
    "GGML_VULKAN_COOPMAT_GLSLC_SUPPORT": "coopmat.comp",
    "GGML_VULKAN_COOPMAT2_GLSLC_SUPPORT": "coopmat2.comp",
    "GGML_VULKAN_COOPMAT2_DECODE_VECTOR_GLSLC_SUPPORT":
        "coopmat2_decode_vector.comp",
    "GGML_VULKAN_INTEGER_DOT_GLSLC_SUPPORT": "integer_dot.comp",
    "GGML_VULKAN_BFLOAT16_GLSLC_SUPPORT": "bfloat16.comp",
    "GGML_VULKAN_FLOAT_E2M1_GLSLC_SUPPORT": "float_e2m1.comp",
    "GGML_VULKAN_FLOAT_E4M3_GLSLC_SUPPORT": "float_e4m3.comp",
}

# Activations and the KV cache: never dropped.
ALWAYS_BUILT = ("f32", "f16")


def generator_type_names(shader_dir):
    """The type_names list of vulkan-shaders-gen.cpp, or None."""
    path = os.path.join(shader_dir, "vulkan-shaders-gen.cpp")
    with open(path) as f:
        text = f.read()
    m = re.search(r"type_names\s*=\s*\{([^}]*)\}", text)
    if not m:
        return None
    return re.findall(r'"([a-z0-9_]+)"', m.group(1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--glslc", required=True)
    ap.add_argument("--shader-dir", required=True)
    ap.add_argument("--stamp", required=True)
    ap.add_argument("--feature", action="append", default=[])
    ap.add_argument("--listed", action="append", default=[],
                    help="a .comp file BUILD.gn lists (basename)")
    ap.add_argument("--dropped", action="append", default=[],
                    help="a weight type BUILD.gn drops")
    args = ap.parse_args()
    # Relative to the build directory, and maybe a bare name (a native
    # build's host tools are in the root of it): never looked up in PATH.
    glslc = os.path.abspath(args.glslc)

    errors = []
    for feature in args.feature:
        test = FEATURE_TESTS.get(feature)
        if not test:
            errors.append("unknown feature define %s" % feature)
            continue
        source = os.path.join(args.shader_dir, "feature-tests", test)
        with tempfile.TemporaryDirectory() as tmp:
            proc = subprocess.run(
                [glslc, "-fshader-stage=compute",
                 "--target-env=vulkan1.3", source, "-o",
                 os.path.join(tmp, "test.spv")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if proc.returncode != 0 or proc.stdout:
            errors.append("%s: %s no longer compiles with Chromium's glslang "
                          "(drop the define from BUILD.gn or fix glslang):\n%s"
                          % (feature, test, proc.stdout))

    on_disk = sorted(f for f in os.listdir(args.shader_dir)
                     if f.endswith(".comp"))
    listed = sorted(args.listed)
    if on_disk != listed:
        missing = sorted(set(on_disk) - set(listed))
        extra = sorted(set(listed) - set(on_disk))
        errors.append("BUILD.gn's ggml-vulkan shader list doesn't match %s:\n"
                      "  not listed: %s\n  listed but absent: %s"
                      % (args.shader_dir, " ".join(missing) or "-",
                         " ".join(extra) or "-"))

    if args.dropped:
        names = generator_type_names(args.shader_dir)
        if not names:
            errors.append("can't find vulkan-shaders-gen.cpp's type_names "
                          "(update ggml_vulkan_checks.py)")
        else:
            for t in args.dropped:
                if t not in names:
                    errors.append("BUILD.gn drops %s, which isn't a type the "
                                  "shader generator knows" % t)
                if t in ALWAYS_BUILT:
                    errors.append("BUILD.gn drops %s, which every model "
                                  "needs" % t)
            if len(set(args.dropped)) != len(args.dropped):
                errors.append("BUILD.gn drops a type twice")

    if errors:
        sys.stderr.write("\n".join(errors) + "\n")
        return 1
    with open(args.stamp, "w") as f:
        f.write("ok\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
