// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Prints the sandbox-exec command line MoltBrowser runs a helper with, built
// by the browser's own seatbelt_profile.cc, one NUL-terminated argument at a
// time, for ocr_smoke_mac.sh. Paths are canonicalized with realpath() first,
// as WrapInSandboxExec() (helper_sandbox_posix.cc) does, and a read-only path
// that doesn't exist is dropped, as there.
//
//   sandbox_exec_argv HELPER SCRATCH [READ_ONLY...] -- [HELPER_ARGS...]
//
// std and POSIX only, so it also builds on Linux (lint.yml checks that).

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "chrome/browser/molt_ai/common/seatbelt_profile.h"

namespace {

// Empty if |path| doesn't exist.
std::string CanonicalPath(const char* path) {
  char resolved[PATH_MAX];
  return realpath(path, resolved) ? std::string(resolved) : std::string();
}

int Usage() {
  fprintf(stderr,
          "usage: sandbox_exec_argv HELPER SCRATCH [READ_ONLY...] -- "
          "[HELPER_ARGS...]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    return Usage();
  }
  const std::string helper = CanonicalPath(argv[1]);
  const std::string scratch = CanonicalPath(argv[2]);
  if (helper.empty() || scratch.empty()) {
    fprintf(stderr, "sandbox_exec_argv: the helper or the scratch directory "
                    "is missing\n");
    return 2;
  }
  std::vector<std::string> read_only;
  int i = 3;
  for (; i < argc && strcmp(argv[i], "--") != 0; ++i) {
    std::string canonical = CanonicalPath(argv[i]);
    if (!canonical.empty()) {
      read_only.push_back(std::move(canonical));
    }
  }
  if (i == argc) {
    return Usage();
  }
  std::vector<std::string> helper_args(argv + i + 1, argv + argc);

  for (const std::string& arg : molt_ai::seatbelt::BuildSandboxExecArgv(
           helper, helper_args, read_only, scratch)) {
    fwrite(arg.c_str(), 1, arg.size() + 1, stdout);
  }
  return fflush(stdout) == 0 ? 0 : 1;
}
