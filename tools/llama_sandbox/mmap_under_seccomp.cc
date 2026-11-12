// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Maps a model file with the vendored llama.cpp's llama_mmap under a
// seccomp filter that answers, as Chrome's seccomp-bpf policy for the
// inference process does, two calls llama.cpp makes there with SIGSYS:
//   - fadvise64, in none of the policy's allowed sets (sandbox/linux/
//     seccomp-bpf-helpers/syscall_sets.cc), which posix_fadvise() makes;
//   - mmap with MAP_POPULATE (RestrictMmapFlags in
//     syscall_parameters_restrictions.cc).
// Everything else is allowed: this checks these two calls only, which
// third_party/llama_cpp's LLAMA_MOLTBROWSER_SANDBOXED patches leave out
// (README.chromium). check.sh builds it with and without that define.
//
// Usage: mmap_under_seccomp <file>. Exit 0: mapped; 3: SIGSYS; 2: setup.

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>

#include "llama-mmap.h"

namespace {

void OnSigsys(int, siginfo_t* info, void*) {
  char msg[] = "SIGSYS: syscall 000 trapped\n";
  int nr = info->si_syscall;
  for (int i = 18; i >= 16; --i) {
    msg[i] = static_cast<char>('0' + nr % 10);
    nr /= 10;
  }
  [[maybe_unused]] const ssize_t written = write(2, msg, sizeof(msg) - 1);
  _exit(3);
}

bool InstallFilter() {
#if !defined(__x86_64__)
  std::fprintf(stderr, "x86_64 only\n");
  return false;
#else
  struct sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fadvise64, 4, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mmap, 0, 4),
      // mmap: its flags (the fourth argument, low word).
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               offsetof(struct seccomp_data, args) + 3 * sizeof(__u64)),
      BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, MAP_POPULATE, 0, 2),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog prog = {
      static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
  struct sigaction sa = {};
  sa.sa_sigaction = OnSigsys;
  sa.sa_flags = SA_SIGINFO;
  if (sigaction(SIGSYS, &sa, nullptr) != 0 ||
      prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
      prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
    std::perror("seccomp");
    return false;
  }
  return true;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <file>\n", argv[0]);
    return 2;
  }
  // The file is opened before the filter, as the browser opens it for the
  // sandboxed process; llama_mmap then works on the descriptor.
  llama_file file(argv[1], "rb");
  if (!InstallFilter()) {
    return 2;
  }
  llama_mmap map(&file);
  std::printf("mapped %zu bytes, first byte %d\n", map.size(),
              static_cast<const unsigned char*>(map.addr())[0]);
  return 0;
}
