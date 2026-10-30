// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A stand-in for tesseract and whisper-cli in the Linux and macOS helper
// sandbox tests (helper_sandbox_unittest.cc). It does one thing per command
// line and prints what happened, so the test can tell what the sandbox and
// the launcher's limits let it do:
//
//   echo TEXT            TEXT on stdout and on stderr; exit 3
//   write PATH MIB       writes MIB MiB to PATH: "WROTE" or "FAILED <errno>"
//   spread DIR N MIB     writes MIB MiB to each of N files in DIR
//   files DIR N          makes N empty files in DIR: "MADE" or "FAILED"
//   delete PATH          "DELETED" or "DENIED <errno>"
//   rename FROM TO       "RENAMED" or "DENIED <errno>"
//   link FROM TO         a hard link: "LINKED" or "DENIED <errno>"
//   symlink TARGET PATH  "LINKED" or "DENIED <errno>"
//   mkdir PATH           "MADE" or "DENIED <errno>"
//   fork                 "FORKED" or "DENIED <errno>"
//   preallocate PATH MIB reserves MIB MiB for PATH without writing
//                        (fallocate, F_PREALLOCATE), then waits
//   xattr PATH           sets an extended attribute: "SET" or "DENIED"
//   tmpfile DIR          (Linux) an O_TMPFILE file: "OPENED" or "DENIED"
//   memfd                (Linux) "OPENED" or "DENIED <errno>"
//   shm MIB              SysV shared memory: "MADE" or "DENIED <errno>"
//   shared MIB           shared anonymous memory: "MAPPED" or "DENIED"
//   alloc MIB            private memory, touched 64 MiB at a time:
//                        "ALLOCATED" or "FAILED <errno>"
//   reserve MIB          private memory in one mapping, not touched:
//                        "RESERVED" or "FAILED <errno>"
//   rsrc PATH MIB        (macOS) writes MIB MiB to PATH's resource fork,
//                        then waits
//   lock DIR             writes DIR/out.txt, takes away all access to DIR
//                        (chmod 000), then waits
//   limits               its file size, core, data and open file limits
//   spin                 loops until killed
// and for the resident mode's tests (helper_resident_unittest.cc):
//   read PATH            reads PATH: "READ <bytes>" or "DENIED <errno>"
//   connect PORT         TCP to 127.0.0.1:PORT: "CONNECTED" or
//                        "DENIED <errno>"
//   dgram PATH           a datagram to the UNIX socket at PATH, without
//                        connect(): "SENT" or "DENIED <errno>"
//   signals              signals itself in each way (its ID, its process
//                        group, raise(), sigqueue()): "SELF <sent>
//                        <received>"; then its parent, with signal 0 (no
//                        effect but the permission check), in each way:
//                        one line each, "<way> SENT" or "<way> DENIED
//                        <errno>"
//   abort                abort(), with a SIGABRT handler that prints
//                        "ABORTED" and exits 0
//   relay                copies stdin to stdout until end of file, then
//                        prints "EOF" and exits 0
//   then ACTION...       ACTION, then relay (the helper stays resident)
//
// Plain POSIX, no //base.

#ifdef UNSAFE_BUFFERS_BUILD
// A C entry point's argv is a raw array, and this test-only program has no
// //base for spans.
#pragma allow_unsafe_buffers
#endif

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace {

constexpr size_t kMiB = 1024 * 1024;

void Print(const char* text) {
  fputs(text, stdout);
  fflush(stdout);
}

void PrintErrno(const char* what) {
  printf("%s %d\n", what, errno);
  fflush(stdout);
}

// Writes |mib| MiB to |fd|.
bool WriteMiB(int fd, unsigned long mib) {
  static char block[kMiB];
  memset(block, 'x', sizeof(block));
  for (unsigned long i = 0; i < mib; ++i) {
    size_t done = 0;
    while (done < sizeof(block)) {
      const ssize_t n = write(fd, block + done, sizeof(block) - done);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      done += static_cast<size_t>(n);
    }
  }
  return true;
}

int Write(const char* path, unsigned long mib) {
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0 || !WriteMiB(fd, mib)) {
    PrintErrno("FAILED");
    return 0;
  }
  close(fd);
  Print("WROTE\n");
  return 0;
}

int Spread(const char* dir, unsigned long count, unsigned long mib) {
  char path[4096];
  for (unsigned long i = 0; i < count; ++i) {
    snprintf(path, sizeof(path), "%s/spread%lu", dir, i);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 || !WriteMiB(fd, mib)) {
      PrintErrno("FAILED");
      return 0;
    }
    close(fd);
  }
  Print("WROTE\n");
  return 0;
}

int Files(const char* dir, unsigned long count) {
  char path[4096];
  for (unsigned long i = 0; i < count; ++i) {
    snprintf(path, sizeof(path), "%s/file%lu", dir, i);
    const int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
      PrintErrno("FAILED");
      return 0;
    }
    close(fd);
  }
  Print("MADE\n");
  // Give the launcher time to look.
  sleep(5);
  return 0;
}

int Result(int rv, const char* done) {
  if (rv != 0) {
    PrintErrno("DENIED");
  } else {
    Print(done);
  }
  return 0;
}

int Fork() {
  const pid_t pid = fork();
  if (pid < 0) {
    PrintErrno("DENIED");
    return 0;
  }
  if (pid == 0) {
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  Print("FORKED\n");
  return 0;
}

int Preallocate(const char* path, unsigned long mib) {
  const int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    PrintErrno("DENIED");
    return 0;
  }
  const off_t bytes = static_cast<off_t>(mib * kMiB);
#if defined(__linux__)
  // FALLOC_FL_KEEP_SIZE: the space is reserved, the size stays 0.
  const int rv = static_cast<int>(syscall(SYS_fallocate, fd, 1, 0, bytes));
#elif defined(__APPLE__)
  fstore_t store = {};
  store.fst_flags = F_ALLOCATEALL;
  store.fst_posmode = F_PEOFPOSMODE;
  store.fst_length = bytes;
  const int rv = fcntl(fd, F_PREALLOCATE, &store);
#else
  const int rv = -1;
#endif
  Result(rv, "PREALLOCATED\n");
  if (rv == 0) {
    // Give the launcher time to look.
    sleep(5);
  }
  return 0;
}

int Xattr(const char* path) {
  static const char value[1024] = {};
#if defined(__APPLE__)
  const int rv = setxattr(path, "org.moltbrowser.test", value, sizeof(value), 0,
                          XATTR_NOFOLLOW);
#else
  const int rv =
      setxattr(path, "user.moltbrowser.test", value, sizeof(value), 0);
#endif
  return Result(rv, "SET\n");
}

int Shm(unsigned long mib) {
  const int id = shmget(IPC_PRIVATE, mib * kMiB, IPC_CREAT | 0600);
  if (id < 0) {
    PrintErrno("DENIED");
    return 0;
  }
  shmctl(id, IPC_RMID, nullptr);
  Print("MADE\n");
  return 0;
}

int Shared(unsigned long mib) {
  void* memory = mmap(nullptr, mib * kMiB, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) {
    PrintErrno("DENIED");
    return 0;
  }
  memset(memory, 1, mib * kMiB);
  Print("MAPPED\n");
  return 0;
}

int Alloc(unsigned long mib) {
  // 64 MiB at a time, each touched: what a runaway allocation looks like.
  constexpr unsigned long kStep = 64;
  for (unsigned long done = 0; done < mib; done += kStep) {
    void* memory = mmap(nullptr, kStep * kMiB, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
      PrintErrno("FAILED");
      return 0;
    }
    memset(memory, 1, kStep * kMiB);
  }
  Print("ALLOCATED\n");
  return 0;
}

int Reserve(unsigned long mib) {
  void* memory = mmap(nullptr, mib * kMiB, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) {
    PrintErrno("FAILED");
    return 0;
  }
  Print("RESERVED\n");
  return 0;
}

void PrintLimit(const char* name, int resource) {
  struct rlimit limit = {};
  getrlimit(resource, &limit);
  printf("%s %llu %llu\n", name,
         static_cast<unsigned long long>(limit.rlim_cur),
         static_cast<unsigned long long>(limit.rlim_max));
}

int Limits() {
  PrintLimit("FSIZE", RLIMIT_FSIZE);
  PrintLimit("CORE", RLIMIT_CORE);
  PrintLimit("DATA", RLIMIT_DATA);
  PrintLimit("NOFILE", RLIMIT_NOFILE);
  fflush(stdout);
  return 0;
}

int Read(const char* path) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    PrintErrno("DENIED");
    return 0;
  }
  char buffer[4096];
  const ssize_t bytes = read(fd, buffer, sizeof(buffer));
  close(fd);
  if (bytes < 0) {
    PrintErrno("DENIED");
    return 0;
  }
  printf("READ %zd\n", bytes);
  fflush(stdout);
  return 0;
}

int Connect(unsigned long port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    PrintErrno("DENIED");
    return 0;
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const int result =
      connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  close(fd);
  return Result(result, "CONNECTED\n");
}

// A datagram to the named UNIX socket at |path|, as a logger sends one to
// the system log: socket() and sendto(), no connect().
int Datagram(const char* path) {
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  const size_t length = strlen(path);
  if (length >= sizeof(address.sun_path)) {
    Print("FAILED path too long\n");
    return 0;
  }
  memcpy(address.sun_path, path, length + 1);
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    PrintErrno("DENIED");
    return 0;
  }
  const char message[] = "molt";
  const ssize_t sent =
      sendto(fd, message, sizeof(message), 0,
             reinterpret_cast<sockaddr*>(&address), sizeof(address));
  close(fd);
  return Result(sent < 0 ? -1 : 0, "SENT\n");
}

volatile sig_atomic_t g_received = 0;

void CountSignal(int) {
  g_received = g_received + 1;
}

void Aborted(int) {
  static const char kText[] = "ABORTED\n";
  [[maybe_unused]] const ssize_t written =
      write(STDOUT_FILENO, kText, sizeof(kText) - 1);
  _exit(0);
}

// One way of signalling another process: |result| 0 is a success.
void Way(const char* way, long result) {
  if (result == 0) {
    printf("%s SENT\n", way);
  } else {
    printf("%s DENIED %d\n", way, errno);
  }
}

int Signals() {
  struct sigaction action = {};
  action.sa_handler = CountSignal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGUSR1, &action, nullptr);
  const pid_t self = getpid();
  // Itself: a signal sent to the calling process is delivered before the
  // call returns.
  int sent = 0;
  sent += kill(self, SIGUSR1) == 0;
  sent += kill(0, SIGUSR1) == 0;      // its process group
  sent += kill(-self, SIGUSR1) == 0;  // the same, by its ID
  sent += raise(SIGUSR1) == 0;
#if defined(__linux__)
  union sigval value = {};
  sent += sigqueue(self, SIGUSR1, value) == 0;
#endif
  printf("SELF %d %d\n", sent, static_cast<int>(g_received));

  // Its parent (the browser), with signal 0.
  const pid_t parent = getppid();
  Way("kill", kill(parent, 0));
  Way("everyone", kill(-1, 0));
#if defined(__linux__)
  Way("tgkill", syscall(SYS_tgkill, parent, parent, 0));
  Way("tkill", syscall(SYS_tkill, parent, 0));
  siginfo_t info = {};
  info.si_signo = 0;
  info.si_code = SI_QUEUE;
  info.si_pid = self;
  info.si_uid = getuid();
  Way("sigqueue", syscall(SYS_rt_sigqueueinfo, parent, 0, &info));
  Way("tgsigqueue",
      syscall(SYS_rt_tgsigqueueinfo, parent, parent, 0, &info));
  // pidfd_open() and pidfd_send_signal() (Linux 5.3 and 5.1).
  const long pidfd = syscall(434, parent, 0);
  Way("pidfd", pidfd < 0 ? pidfd : syscall(424, pidfd, 0, nullptr, 0));
  // Whom the kernel signals when a file is ready.
  int fds[2];
  if (pipe(fds) == 0) {
    Way("owner", fcntl(fds[0], F_SETOWN, parent));
    close(fds[0]);
    close(fds[1]);
  }
  // Its resource limits (RLIMIT_CPU would have the kernel kill it).
  struct rlimit limit = {};
  Way("limits", syscall(SYS_prlimit64, parent, RLIMIT_NOFILE, nullptr, &limit));
#endif
  fflush(stdout);
  return 0;
}

// Copies stdin to stdout until end of file.
int Relay() {
  char buffer[4096];
  for (;;) {
    const ssize_t bytes = read(STDIN_FILENO, buffer, sizeof(buffer));
    if (bytes < 0 && errno == EINTR) {
      continue;
    }
    if (bytes <= 0) {
      break;
    }
    ssize_t done = 0;
    while (done < bytes) {
      const ssize_t n = write(STDOUT_FILENO, buffer + done,
                              static_cast<size_t>(bytes - done));
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        return 1;
      }
      done += n;
    }
  }
  Print("EOF\n");
  return 0;
}

int Act(int argc, char* argv[]);

}  // namespace

int main(int argc, char* argv[]) {
  if (argc >= 3 && strcmp(argv[1], "then") == 0) {
    Act(argc - 1, argv + 1);
    return Relay();
  }
  return Act(argc, argv);
}

namespace {

int Act(int argc, char* argv[]) {
  if (argc == 3 && strcmp(argv[1], "echo") == 0) {
    printf("%s\n", argv[2]);
    fflush(stdout);
    fprintf(stderr, "%s\n", argv[2]);
    fflush(stderr);
    return 3;
  }
  if (argc == 4 && strcmp(argv[1], "write") == 0) {
    return Write(argv[2], strtoul(argv[3], nullptr, 10));
  }
  if (argc == 5 && strcmp(argv[1], "spread") == 0) {
    return Spread(argv[2], strtoul(argv[3], nullptr, 10),
                  strtoul(argv[4], nullptr, 10));
  }
  if (argc == 4 && strcmp(argv[1], "files") == 0) {
    return Files(argv[2], strtoul(argv[3], nullptr, 10));
  }
  if (argc == 3 && strcmp(argv[1], "delete") == 0) {
    return Result(unlink(argv[2]), "DELETED\n");
  }
  if (argc == 4 && strcmp(argv[1], "rename") == 0) {
    return Result(rename(argv[2], argv[3]), "RENAMED\n");
  }
  if (argc == 4 && strcmp(argv[1], "link") == 0) {
    return Result(link(argv[2], argv[3]), "LINKED\n");
  }
  if (argc == 4 && strcmp(argv[1], "symlink") == 0) {
    return Result(symlink(argv[2], argv[3]), "LINKED\n");
  }
  if (argc == 3 && strcmp(argv[1], "mkdir") == 0) {
    return Result(mkdir(argv[2], 0700), "MADE\n");
  }
  if (argc == 2 && strcmp(argv[1], "fork") == 0) {
    return Fork();
  }
  if (argc == 4 && strcmp(argv[1], "preallocate") == 0) {
    return Preallocate(argv[2], strtoul(argv[3], nullptr, 10));
  }
  if (argc == 3 && strcmp(argv[1], "xattr") == 0) {
    return Xattr(argv[2]);
  }
#if defined(__linux__)
  if (argc == 3 && strcmp(argv[1], "tmpfile") == 0) {
    const int fd = open(argv[2], O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    return Result(fd < 0 ? -1 : 0, "OPENED\n");
  }
  if (argc == 2 && strcmp(argv[1], "memfd") == 0) {
    const long fd = syscall(SYS_memfd_create, "molt", 0U);
    return Result(fd < 0 ? -1 : 0, "OPENED\n");
  }
#endif
  if (argc == 3 && strcmp(argv[1], "shm") == 0) {
    return Shm(strtoul(argv[2], nullptr, 10));
  }
  if (argc == 3 && strcmp(argv[1], "shared") == 0) {
    return Shared(strtoul(argv[2], nullptr, 10));
  }
  if (argc == 3 && strcmp(argv[1], "alloc") == 0) {
    return Alloc(strtoul(argv[2], nullptr, 10));
  }
  if (argc == 3 && strcmp(argv[1], "reserve") == 0) {
    return Reserve(strtoul(argv[2], nullptr, 10));
  }
#if defined(__APPLE__)
  if (argc == 4 && strcmp(argv[1], "rsrc") == 0) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/..namedfork/rsrc", argv[2]);
    Write(path, strtoul(argv[3], nullptr, 10));
    sleep(5);
    return 0;
  }
#endif
  if (argc == 3 && strcmp(argv[1], "lock") == 0) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/out.txt", argv[2]);
    Write(path, 1);
    Result(chmod(argv[2], 0), "LOCKED\n");
    sleep(2);
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "limits") == 0) {
    return Limits();
  }
  if (argc == 2 && strcmp(argv[1], "spin") == 0) {
    for (volatile unsigned counter = 0;; counter = counter + 1) {
    }
  }
  if (argc == 3 && strcmp(argv[1], "read") == 0) {
    return Read(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "connect") == 0) {
    return Connect(strtoul(argv[2], nullptr, 10));
  }
  if (argc == 3 && strcmp(argv[1], "dgram") == 0) {
    return Datagram(argv[2]);
  }
  if (argc == 2 && strcmp(argv[1], "signals") == 0) {
    return Signals();
  }
  if (argc == 2 && strcmp(argv[1], "abort") == 0) {
    struct sigaction action = {};
    action.sa_handler = Aborted;
    sigemptyset(&action.sa_mask);
    sigaction(SIGABRT, &action, nullptr);
    abort();
  }
  if (argc == 2 && strcmp(argv[1], "relay") == 0) {
    return Relay();
  }
  Print(
      "usage: echo|write|spread|files|delete|rename|link|symlink|mkdir|"
      "fork|preallocate|xattr|tmpfile|memfd|shm|shared|alloc|reserve|rsrc|"
      "lock|limits|spin|read|connect|dgram|signals|abort|relay|then ...\n");
  return 2;
}

}  // namespace
