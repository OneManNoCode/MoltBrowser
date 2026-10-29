// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// How much a helper program (tesseract, whisper-cli) may write and use
// while it runs, and how its scratch directory is measured.
// RunSandboxedHelper() (helper_sandbox.h) applies these limits on every OS.
//
// What it writes. Its scratch directory, the one place it may write, may
// grow by less than kMaxHelperWriteBytes beyond what the browser put there
// before it started (the input), and gain at most kMaxScratchNewEntries
// entries. While the helper runs, the launcher measures the directory every
// kScratchCheckInterval (MeasureScratch()) and kills the helper once it is
// past either limit (JudgeScratch()). It measures once more after the
// helper exits. Past a limit, the run fails and says why
// (DescribeScratchVerdict()). Each entry counts with its size or the space
// it takes, whichever is larger, so a sparse or preallocated file counts
// in full; on macOS that includes its resource fork, and on Windows every
// stream. Nothing is followed out of the directory: a link or reparse point
// is just an entry. Directories nested more than kMaxScratchDepth deep, or
// an entry that can't be measured, fail the run too.
//
// The OS confinement keeps everything the helper writes in that directory,
// where it can be measured. The helper may create and write files there,
// but may not delete or rename them, so a file it still holds open can't
// leave the directory, nor make directories or symbolic links. On Linux,
// RLIMIT_FSIZE also caps each file at kMaxHelperWriteBytes, and fallocate()
// is refused. There a file grows only as fast as the helper writes to it,
// so the launcher's next check sees it. On macOS (F_PREALLOCATE) and
// Windows (SetEndOfFile()) a helper can reserve space for a file at once;
// the next check sees that too.
//
// Memory: at most kMaxHelperMemoryBytes on every OS. Linux sets
// RLIMIT_DATA; Windows sets its job's memory limit; on macOS the launcher
// checks the helper's memory footprint every kScratchCheckInterval. On
// every OS the helper is one process (it can start no other), so that is
// all its memory.
//
// Pure C++ (POSIX or Win32 calls, no //base on POSIX), so a standalone
// harness runs the measuring code that ships against real file systems.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_HELPER_LIMITS_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_HELPER_LIMITS_H_

#include <stdint.h>

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace molt_ai {
namespace helper_limits {

// The most a helper's scratch directory may grow (it must stay below this)
// and, on Linux, the largest file a helper may write. tesseract and
// whisper-cli write one text file of at most a few hundred KiB.
inline constexpr uint64_t kMaxHelperWriteBytes = 64ULL << 20;
// The most entries a helper may add to its scratch directory. tesseract and
// whisper-cli add one.
inline constexpr uint64_t kMaxScratchNewEntries = 64;
// The deepest a directory beneath the scratch directory may be (a
// directory in it is at depth 1). Helpers can't make directories; this
// bounds the walk.
inline constexpr int kMaxScratchDepth = 8;
// How often the launcher measures the scratch directory (and, on macOS,
// the helper's memory) while the helper runs.
inline constexpr int kScratchCheckIntervalMs = 10;
// The most memory a helper may use. Measured with typical and the largest
// inputs: tesseract 5.3 needs about 470 MiB of RLIMIT_DATA for the largest
// image the browser writes (33.5 MP, RGB), whisper-cli 1.7.5 about 590 MiB
// with the tiny.en model.
inline constexpr uint64_t kMaxHelperMemoryBytes = 4ULL << 30;

#if defined(_WIN32)
using PathString = std::wstring;
#else
using PathString = std::string;
#endif

// What a scratch directory holds (MeasureScratch()).
struct ScratchUsage {
  // Why a measurement is incomplete, if it is.
  enum class Gap { kNone, kTooManyEntries, kTooDeep, kUnreadable };

  // The entries beneath the directory, each counted with the larger of its
  // size and the space it takes. Saturates at UINT64_MAX.
  uint64_t bytes = 0;
  // Entries beneath it: files, directories, links, anything.
  uint64_t entries = 0;
  Gap gap = Gap::kNone;
};

// Walks the directory |dir| (an absolute path) without following links or
// reparse points. Stops at the first entry past |max_entries| (gap
// kTooManyEntries), at a directory deeper than kMaxScratchDepth (kTooDeep)
// or at an entry it can't measure (kUnreadable); an entry that disappears
// while it walks is counted with no bytes. Blocking.
ScratchUsage MeasureScratch(const PathString& dir, uint64_t max_entries);

// The |max_entries| to measure with while a helper runs, when its scratch
// directory held |before| when it started.
uint64_t MaxScratchEntries(const ScratchUsage& before);

enum class ScratchVerdict {
  kWithinLimits,
  kTooMuchData,
  kTooManyEntries,
  kTooDeep,
  kUnmeasurable,
};

// Whether a scratch directory that holds |now| is within the limits, when
// it held |before| when the helper started.
ScratchVerdict JudgeScratch(const ScratchUsage& before,
                            const ScratchUsage& now);

// Why a run was stopped, e.g. "it wrote 64 MiB or more to its scratch
// directory". Empty for kWithinLimits.
std::string DescribeScratchVerdict(ScratchVerdict verdict);

// "it used more than 4 GiB of memory".
std::string DescribeMemoryLimit();

// ---------------------------------------------------------------------------
// Resident helpers (helper_resident.h): a helper that runs as long as a
// feature is on (the speech helper while voice mode listens), with no
// deadline and nothing to write. Its memory and CPU are bounded instead.
// ---------------------------------------------------------------------------

// The most memory a resident helper may use, by default (Linux
// RLIMIT_DATA, Windows its job's limit, macOS its footprint, checked). The
// speech helper (three small ONNX models) peaks under 300 MiB.
inline constexpr uint64_t kMaxResidentMemoryBytes = 1ULL << 30;
// The most CPU it may use: on average no more than this many cores over
// the last kResidentCpuWindowMs. Listening costs a few percent of one core
// (VOICE_BENCH.md §3.6); a helper stuck in a loop is stopped.
inline constexpr double kMaxResidentCpuCores = 0.75;
inline constexpr int64_t kResidentCpuWindowMs = 30000;
// Windows also caps its job at this many cores' worth of CPU, so even a
// burst can't take the machine.
inline constexpr double kResidentCpuHardCapCores = 2.0;
// How often the launcher checks a resident helper's memory and CPU.
inline constexpr int64_t kResidentCheckIntervalMs = 1000;

// Judges a process's CPU use from samples of its cumulative CPU time: past
// the limit once it has used more than |max_cores| x |window_ms| of CPU
// within the last |window_ms| (so a burst on many cores is caught before
// the window has passed, and a steady load just above the average after
// it).
class CpuUsageWindow {
 public:
  CpuUsageWindow(double max_cores, int64_t window_ms);
  ~CpuUsageWindow();
  CpuUsageWindow(const CpuUsageWindow&) = delete;
  CpuUsageWindow& operator=(const CpuUsageWindow&) = delete;

  // The process had used |cpu_ms| of CPU in all at |now_ms| (both
  // monotonic). False once it is past the limit; it stays past it.
  bool Add(int64_t now_ms, int64_t cpu_ms);

  // "it used more than 0.75 of a CPU core for 30 s".
  std::string Describe() const;

 private:
  const double max_cores_;
  const int64_t window_ms_;
  std::deque<std::pair<int64_t, int64_t>> samples_;
  bool past_ = false;
};

// Linux: a process's user and system CPU time, in clock ticks, from the
// contents of /proc/<pid>/stat (its 14th and 15th fields, after the command
// name in parentheses, which may hold anything); nullopt if it doesn't
// read as one.
std::optional<uint64_t> ParseProcStatCpuTicks(std::string_view stat);

}  // namespace helper_limits
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_HELPER_LIMITS_H_
