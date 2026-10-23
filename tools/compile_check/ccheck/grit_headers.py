#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Chromium's grit, writing only a .grd's resource headers.

  grit_headers.py GRIT_DIR <the arguments of the GN grit action>

GRIT_DIR is Chromium's tools/grit. The arguments are exactly those the
build's grit() action passes to tools/grit/grit.py (`-i X.grd build -o ...
-D ... -E ... -t linux -f default_resource_ids ...`), and grit itself reads
the .grd, its <part> files and the first-ids file, evaluates the <if>
conditions and assigns the IDs. Only the header outputs (rc_header,
resource_map_header) are written: the .pak files and the resource maps'
.cc files need the resources themselves (images, translations), which a
syntax check doesn't. For the same reason grit's gatherers (which read
those files) are not run; header contents don't depend on them. The asserted
output list and the depfile are not checked or written.
"""
import os
import sys

HEADER_TYPES = ("rc_header", "resource_map_header")


def main(argv):
    sys.path.insert(0, os.path.abspath(argv[0]))
    from grit import grit_runner
    from grit.node import misc
    from grit.tool import build

    def process(self):
        if self.res.GetIdMap() is None:
            self.res.InitializeIds()
        outputs = self.res.GetOutputFiles()
        for output in outputs:
            output.output_filename = os.path.abspath(os.path.join(
                self.output_directory, output.GetOutputFilename()))
        if self.allowlist_names:
            self.AddAllowlistTags(self.res, self.allowlist_names)
        for output in outputs:
            if output.GetType() in HEADER_TYPES:
                self._ProcessOutput(output, [])

    build.RcBuilder.Process = process
    build.RcBuilder.CheckAssertedOutputFiles = lambda self, files: True
    build.RcBuilder.GenerateDepfile = lambda self, *a, **k: None
    misc.GritNode.RunGatherers = lambda self, debug=False: None
    os.environ["GRIT_DISABLE_MULTIPROCESSING"] = "1"
    return grit_runner.Main(argv[1:])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]) or 0)
