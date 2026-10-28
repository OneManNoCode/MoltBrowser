// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// compile_check STAND-IN for //base, for building one host tool here:
// net/tools/transport_security_state_generator (with
// net/tools/huffman_trie), which writes net/http/transport_security_state_
// static.h and _pins.h from the HSTS preload list and the pins. The tool's
// own sources and BoringSSL are the real ones; //base would need most of
// itself to link (logging, threading, the allocator shim) and its JSON
// reader is Rust (serde_json_lenient through cxx). These headers implement
// exactly what the tool calls, with //base's behaviour on the inputs the
// build gives it (a JSON parser with //base's options, of which the tool
// uses JSON_PARSE_CHROMIUM_EXTENSIONS: comments, \x escapes, newlines in
// strings). Every base/... header of the tool includes this one;
// shim_test.cc checks it. See tools/compile_check/README.md.

#ifndef COMPILE_CHECK_SHIM_HSTS_GENERATOR_SHIM_H_
#define COMPILE_CHECK_SHIM_HSTS_GENERATOR_SHIM_H_

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// -- base/compiler_specific.h -------------------------------------------------

#define UNSAFE_BUFFERS(...) __VA_ARGS__
#define UNSAFE_TODO(...) __VA_ARGS__

// -- base/check.h, base/logging.h ---------------------------------------------

namespace compile_check_shim {

class LogMessage {
 public:
  LogMessage(const char* file, int line, const char* what, bool fatal)
      : fatal_(fatal) {
    stream_ << "[" << file << ":" << line << "] " << what;
  }
  ~LogMessage() {
    std::cerr << stream_.str() << std::endl;
    if (fatal_) {
      std::abort();
    }
  }
  std::ostream& stream() { return stream_; }

 private:
  bool fatal_;
  std::ostringstream stream_;
};

// Swallows the stream of a check that passed, or of a disabled VLOG.
struct Voidify {
  void operator&(std::ostream&) {}
};

inline int& Verbosity() {
  static int v = 0;
  return v;
}

}  // namespace compile_check_shim

#define CHECK(cond)                                                     \
  (cond) ? (void)0                                                      \
         : compile_check_shim::Voidify() &                              \
               compile_check_shim::LogMessage(__FILE__, __LINE__,       \
                                              "Check failed: " #cond " ", \
                                              true)                     \
                   .stream()
#define DCHECK(cond) CHECK(cond)
#define NOTREACHED()                                                       \
  compile_check_shim::LogMessage(__FILE__, __LINE__, "NOTREACHED ", true) \
      .stream()
#define LOG(severity)                                                      \
  compile_check_shim::LogMessage(__FILE__, __LINE__, #severity ": ", false) \
      .stream()
#define VLOG(level)                                                      \
  (compile_check_shim::Verbosity() < (level))                            \
      ? (void)0                                                          \
      : compile_check_shim::Voidify() &                                  \
            compile_check_shim::LogMessage(__FILE__, __LINE__, "VLOG ",  \
                                           false)                        \
                .stream()

namespace logging {
enum LoggingDestination : uint32_t {
  LOG_NONE = 0,
  LOG_TO_FILE = 1 << 0,
  LOG_TO_SYSTEM_DEBUG_LOG = 1 << 1,
  LOG_TO_STDERR = 1 << 2,
};
struct LoggingSettings {
  uint32_t logging_dest = LOG_TO_STDERR;
};
inline bool InitLogging(const LoggingSettings&) {
  return true;
}
}  // namespace logging

namespace base {

// -- base/at_exit.h -------------------------------------------------------------

class AtExitManager {
 public:
  AtExitManager() = default;
  AtExitManager(const AtExitManager&) = delete;
  AtExitManager& operator=(const AtExitManager&) = delete;
};

// -- base/numerics ---------------------------------------------------------------

template <typename Dst, typename Src>
constexpr bool IsValueInRangeForNumericType(Src value) {
  return std::in_range<Dst>(value);
}

template <typename Dst, typename Src>
constexpr Dst checked_cast(Src value) {
  CHECK(std::in_range<Dst>(value));
  return static_cast<Dst>(value);
}

// -- base/containers/span.h -------------------------------------------------------

template <typename T>
class span : public std::span<T> {
 public:
  using std::span<T>::span;
  constexpr span(std::span<T> s) : std::span<T>(s) {}  // NOLINT

  template <typename R>
  void copy_from(const R& other) {
    CHECK(std::size(other) == this->size());
    std::copy(std::begin(other), std::end(other), this->begin());
  }
};

template <typename T, size_t N>
span(std::array<T, N>&) -> span<T>;
template <typename T, size_t N>
span(const std::array<T, N>&) -> span<const T>;
template <typename T>
span(T*, size_t) -> span<T>;

inline span<const uint8_t> as_byte_span(std::string_view s) {
  return span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()),
                             s.size());
}

inline std::string_view as_string_view(span<const uint8_t> s) {
  return std::string_view(reinterpret_cast<const char*>(s.data()), s.size());
}

inline std::string_view as_string_view(span<uint8_t> s) {
  return std::string_view(reinterpret_cast<const char*>(s.data()), s.size());
}

// -- base/containers/fixed_flat_set.h ---------------------------------------------

template <typename Key, size_t N>
class fixed_flat_set {
 public:
  constexpr explicit fixed_flat_set(std::array<Key, N> a) : keys_(a) {
    std::sort(keys_.begin(), keys_.end());
  }
  constexpr bool contains(const Key& k) const {
    return std::binary_search(keys_.begin(), keys_.end(), k);
  }
  template <typename K>
  bool contains(const K& k) const {
    return contains(Key(k));
  }
  constexpr size_t size() const { return N; }
  constexpr auto begin() const { return keys_.begin(); }
  constexpr auto end() const { return keys_.end(); }

 private:
  std::array<Key, N> keys_;
};

template <typename Key, size_t N>
constexpr fixed_flat_set<Key, N> MakeFixedFlatSet(
    std::type_identity_t<Key> (&&data)[N]) {
  return fixed_flat_set<Key, N>(std::to_array(data));
}

// -- base/strings ---------------------------------------------------------------

enum class CompareCase { SENSITIVE, INSENSITIVE_ASCII };
enum WhitespaceHandling { KEEP_WHITESPACE, TRIM_WHITESPACE };
enum SplitResult { SPLIT_WANT_ALL, SPLIT_WANT_NONEMPTY };
enum TrimPositions {
  TRIM_NONE = 0,
  TRIM_LEADING = 1 << 0,
  TRIM_TRAILING = 1 << 1,
  TRIM_ALL = TRIM_LEADING | TRIM_TRAILING,
};

inline constexpr char kWhitespaceASCII[] = {0x09, 0x0A, 0x0B, 0x0C,
                                            0x0D, 0x20, 0};

constexpr char ToLowerASCII(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}
constexpr char ToUpperASCII(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c + ('A' - 'a')) : c;
}
inline std::string ToLowerASCII(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = ToLowerASCII(c);
  }
  return out;
}
inline std::string ToUpperASCII(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = ToUpperASCII(c);
  }
  return out;
}

inline bool EqualsCaseInsensitiveASCII(std::string_view a,
                                       std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (ToLowerASCII(a[i]) != ToLowerASCII(b[i])) {
      return false;
    }
  }
  return true;
}

inline bool StartsWith(std::string_view str,
                       std::string_view search_for,
                       CompareCase case_sensitivity = CompareCase::SENSITIVE) {
  if (search_for.size() > str.size()) {
    return false;
  }
  std::string_view source = str.substr(0, search_for.size());
  return case_sensitivity == CompareCase::SENSITIVE
             ? source == search_for
             : EqualsCaseInsensitiveASCII(source, search_for);
}

inline std::optional<std::string_view> RemovePrefix(
    std::string_view string,
    std::string_view prefix,
    CompareCase case_sensitivity = CompareCase::SENSITIVE) {
  if (!StartsWith(string, prefix, case_sensitivity)) {
    return std::nullopt;
  }
  string.remove_prefix(prefix.size());
  return string;
}

inline std::string_view TrimStringView(std::string_view input,
                                       std::string_view trim_chars,
                                       TrimPositions positions) {
  size_t begin = (positions & TRIM_LEADING)
                     ? input.find_first_not_of(trim_chars)
                     : 0;
  if (begin == std::string_view::npos) {
    return std::string_view();
  }
  size_t end = (positions & TRIM_TRAILING)
                   ? input.find_last_not_of(trim_chars) + 1
                   : input.size();
  return input.substr(begin, end - begin);
}

inline bool TrimString(std::string_view input,
                       std::string_view trim_chars,
                       std::string* output) {
  std::string_view t = TrimStringView(input, trim_chars, TRIM_ALL);
  bool changed = t.size() != input.size();
  *output = std::string(t);
  return changed;
}

// //base's CollapseWhitespaceASCII: leading and trailing whitespace goes, a
// run of whitespace becomes one space, or nothing when it holds a line
// break and trim_sequences_with_line_breaks is set.
inline std::string CollapseWhitespaceASCII(
    std::string_view text,
    bool trim_sequences_with_line_breaks) {
  std::string result;
  result.resize(text.size());
  int chars_written = 0;
  bool in_whitespace = true;
  bool already_trimmed = true;
  for (char c : text) {
    bool is_ws = std::strchr(kWhitespaceASCII, c) != nullptr && c != 0;
    if (is_ws) {
      if (!in_whitespace) {
        in_whitespace = true;
        result[chars_written++] = ' ';
      }
      if (trim_sequences_with_line_breaks && !already_trimmed &&
          (c == '\n' || c == '\r')) {
        already_trimmed = true;
        --chars_written;
      }
    } else {
      in_whitespace = false;
      already_trimmed = false;
      result[chars_written++] = c;
    }
  }
  if (in_whitespace && !already_trimmed) {
    --chars_written;
  }
  result.resize(chars_written);
  return result;
}

inline std::vector<std::string_view> SplitStringPiece(
    std::string_view input,
    std::string_view separators,
    WhitespaceHandling whitespace,
    SplitResult result_type) {
  std::vector<std::string_view> out;
  if (input.empty()) {
    return out;
  }
  size_t start = 0;
  while (start != std::string_view::npos) {
    size_t end = input.find_first_of(separators, start);
    std::string_view piece;
    if (end == std::string_view::npos) {
      piece = input.substr(start);
      start = std::string_view::npos;
    } else {
      piece = input.substr(start, end - start);
      start = end + 1;
    }
    if (whitespace == TRIM_WHITESPACE) {
      piece = TrimStringView(piece, kWhitespaceASCII, TRIM_ALL);
    }
    if (result_type == SPLIT_WANT_ALL || !piece.empty()) {
      out.push_back(piece);
    }
  }
  return out;
}

inline std::string StrCat(std::initializer_list<std::string_view> pieces) {
  std::string out;
  for (std::string_view p : pieces) {
    out.append(p);
  }
  return out;
}

inline void StrAppend(std::string* dest,
                      std::initializer_list<std::string_view> pieces) {
  for (std::string_view p : pieces) {
    dest->append(p);
  }
}

template <typename T>
  requires std::is_arithmetic_v<T>
std::string NumberToString(T value) {
  if constexpr (std::is_floating_point_v<T>) {
    std::ostringstream s;
    s << value;
    return s.str();
  } else {
    return std::to_string(value);
  }
}

// //base's integer parsing: digits only (a leading '+' or '-' allowed for
// signed types), no surrounding whitespace, in range.
template <typename T>
bool StringToIntImpl(std::string_view input, T* output) {
  *output = 0;
  if (input.empty()) {
    return false;
  }
  size_t i = 0;
  bool negative = false;
  if (input[0] == '-' || input[0] == '+') {
    negative = input[0] == '-';
    if (negative && !std::is_signed_v<T>) {
      return false;
    }
    i = 1;
    if (input.size() == 1) {
      return false;
    }
  }
  unsigned long long v = 0;
  for (; i < input.size(); ++i) {
    char c = input[i];
    if (c < '0' || c > '9') {
      return false;
    }
    if (v > (ULLONG_MAX - (c - '0')) / 10) {
      return false;
    }
    v = v * 10 + (c - '0');
  }
  if (negative) {
    long long sv = -static_cast<long long>(v);
    if (!std::in_range<T>(sv)) {
      return false;
    }
    *output = static_cast<T>(sv);
  } else {
    if (!std::in_range<T>(v)) {
      return false;
    }
    *output = static_cast<T>(v);
  }
  return true;
}

inline bool StringToUint(std::string_view input, unsigned* output) {
  return StringToIntImpl(input, output);
}
inline bool StringToUint64(std::string_view input, uint64_t* output) {
  return StringToIntImpl(input, output);
}
inline bool StringToInt(std::string_view input, int* output) {
  return StringToIntImpl(input, output);
}

inline void StringAppendV(std::string* dst, const char* format, va_list ap) {
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  if (n <= 0) {
    return;
  }
  std::string buf(static_cast<size_t>(n) + 1, '\0');
  vsnprintf(buf.data(), buf.size(), format, ap);
  buf.resize(static_cast<size_t>(n));
  dst->append(buf);
}

[[gnu::format(printf, 2, 3)]] inline void StringAppendF(std::string* dst,
                                                        const char* format,
                                                        ...) {
  va_list ap;
  va_start(ap, format);
  StringAppendV(dst, format, ap);
  va_end(ap);
}

[[gnu::format(printf, 1, 2)]] inline std::string StringPrintf(
    const char* format,
    ...) {
  std::string out;
  va_list ap;
  va_start(ap, format);
  StringAppendV(&out, format, ap);
  va_end(ap);
  return out;
}

// -- base/base64.h ----------------------------------------------------------------

// //base's strict decoding (modp_b64): the standard alphabet, padded to a
// multiple of four, nothing else (no whitespace).
inline std::optional<std::vector<uint8_t>> Base64Decode(
    std::string_view input) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  if (input.size() % 4 != 0) {
    return std::nullopt;
  }
  std::vector<uint8_t> out;
  out.reserve(input.size() / 4 * 3);
  for (size_t i = 0; i < input.size(); i += 4) {
    int v[4];
    int pad = 0;
    for (int j = 0; j < 4; ++j) {
      char c = input[i + j];
      if (c == '=') {
        // Padding only at the very end, in the last two places.
        if (i + 4 != input.size() || j < 2) {
          return std::nullopt;
        }
        v[j] = 0;
        ++pad;
      } else {
        if (pad) {
          return std::nullopt;
        }
        v[j] = value(c);
        if (v[j] < 0) {
          return std::nullopt;
        }
      }
    }
    uint32_t n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
    out.push_back(static_cast<uint8_t>(n >> 16));
    if (pad < 2) {
      out.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
    }
    if (pad < 1) {
      out.push_back(static_cast<uint8_t>(n & 0xff));
    }
  }
  return out;
}

inline bool Base64Decode(std::string_view input, std::string* output) {
  std::optional<std::vector<uint8_t>> d = Base64Decode(input);
  if (!d) {
    return false;
  }
  output->assign(d->begin(), d->end());
  return true;
}

// -- base/time/time.h ---------------------------------------------------------------

class Time {
 public:
  constexpr Time() = default;
  static Time FromTimeT(time_t t) {
    Time r;
    r.t_ = t;
    return r;
  }
  time_t ToTimeT() const { return t_; }
  bool is_null() const { return t_ == 0; }

 private:
  time_t t_ = 0;
};

// -- base/files/file_path.h, base/files/file_util.h -------------------------------------

class FilePath {
 public:
  using StringType = std::string;
  FilePath() = default;
  explicit FilePath(std::string_view path) : path_(path) {}
  const std::string& value() const { return path_; }
  bool empty() const { return path_.empty(); }
  std::string AsUTF8Unsafe() const { return path_; }

 private:
  std::string path_;
};

inline bool PathExists(const FilePath& path) {
  std::ifstream f(path.value());
  return f.good();
}

inline FilePath MakeAbsoluteFilePath(const FilePath& input) {
  char buf[PATH_MAX];
  if (!realpath(input.value().c_str(), buf)) {
    return FilePath();
  }
  return FilePath(buf);
}

inline bool ReadFileToString(const FilePath& path, std::string* contents) {
  std::ifstream f(path.value(), std::ios::binary);
  if (!f) {
    return false;
  }
  std::ostringstream s;
  s << f.rdbuf();
  *contents = s.str();
  return true;
}

inline bool WriteFile(const FilePath& path, std::string_view data) {
  std::ofstream f(path.value(), std::ios::binary | std::ios::trunc);
  if (!f) {
    return false;
  }
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(f);
}

// -- base/command_line.h ------------------------------------------------------------

// POSIX parsing as //base does it: "--name" or "--name=value" (or a single
// "-") are switches, "--" ends them, everything else is an argument.
class CommandLine {
 public:
  using StringType = std::string;
  using StringVector = std::vector<std::string>;

  static bool Init(int argc, const char* const* argv) {
    Current() = std::make_unique<CommandLine>();
    bool switches_done = false;
    for (int i = 1; i < argc; ++i) {
      std::string_view a(argv[i]);
      if (!switches_done && a == "--") {
        switches_done = true;
        continue;
      }
      std::string_view name;
      if (!switches_done && a.starts_with("--") && a.size() > 2) {
        name = a.substr(2);
      } else if (!switches_done && a.starts_with("-") && a.size() > 1) {
        name = a.substr(1);
      }
      if (!name.empty()) {
        size_t eq = name.find('=');
        Current()->switches_[std::string(name.substr(0, eq))] =
            eq == std::string_view::npos ? std::string()
                                         : std::string(name.substr(eq + 1));
      } else {
        Current()->args_.emplace_back(a);
      }
    }
    return true;
  }
  static CommandLine* ForCurrentProcess() { return Current().get(); }
  bool HasSwitch(std::string_view name) const {
    return switches_.count(std::string(name)) != 0;
  }
  std::string GetSwitchValueASCII(std::string_view name) const {
    auto it = switches_.find(std::string(name));
    return it == switches_.end() ? std::string() : it->second;
  }
  const StringVector& GetArgs() const { return args_; }

 private:
  static std::unique_ptr<CommandLine>& Current() {
    static std::unique_ptr<CommandLine> c;
    return c;
  }
  std::map<std::string, std::string> switches_;
  StringVector args_;
};

// -- base/values.h ---------------------------------------------------------------------

class Value;

class DictValue {
 public:
  using Storage = std::map<std::string, std::unique_ptr<Value>, std::less<>>;

  class const_iterator {
   public:
    explicit const_iterator(Storage::const_iterator it) : it_(it) {}
    std::pair<const std::string&, const Value&> operator*() const {
      return {it_->first, *it_->second};
    }
    const_iterator& operator++() {
      ++it_;
      return *this;
    }
    bool operator==(const const_iterator& o) const { return it_ == o.it_; }

   private:
    Storage::const_iterator it_;
  };

  DictValue() = default;
  DictValue(DictValue&&) = default;
  DictValue& operator=(DictValue&&) = default;

  const_iterator begin() const { return const_iterator(storage_.begin()); }
  const_iterator end() const { return const_iterator(storage_.end()); }
  size_t size() const { return storage_.size(); }
  bool empty() const { return storage_.empty(); }

  void Set(std::string_view key, Value&& value);
  const Value* Find(std::string_view key) const;
  Value* Find(std::string_view key);
  std::optional<bool> FindBool(std::string_view key) const;
  std::optional<int> FindInt(std::string_view key) const;
  const std::string* FindString(std::string_view key) const;
  const DictValue* FindDict(std::string_view key) const;
  const class ListValue* FindList(std::string_view key) const;
  class ListValue* FindList(std::string_view key);

 private:
  Storage storage_;
};

class ListValue {
 public:
  ListValue() = default;
  ListValue(ListValue&&) = default;
  ListValue& operator=(ListValue&&) = default;

  size_t size() const { return storage_.size(); }
  bool empty() const { return storage_.empty(); }
  const Value& operator[](size_t i) const { return *storage_[i]; }
  Value& operator[](size_t i) { return *storage_[i]; }
  void Append(Value&& value);

  class const_iterator {
   public:
    explicit const_iterator(
        std::vector<std::unique_ptr<Value>>::const_iterator it)
        : it_(it) {}
    const Value& operator*() const { return **it_; }
    const_iterator& operator++() {
      ++it_;
      return *this;
    }
    bool operator==(const const_iterator& o) const { return it_ == o.it_; }

   private:
    std::vector<std::unique_ptr<Value>>::const_iterator it_;
  };
  const_iterator begin() const { return const_iterator(storage_.begin()); }
  const_iterator end() const { return const_iterator(storage_.end()); }

 private:
  std::vector<std::unique_ptr<Value>> storage_;
};

class Value {
 public:
  enum class Type { NONE, BOOLEAN, INTEGER, DOUBLE, STRING, DICT, LIST };

  Value() = default;
  explicit Value(bool b) : data_(b) {}
  explicit Value(int i) : data_(i) {}
  explicit Value(double d) : data_(d) {}
  explicit Value(std::string s) : data_(std::move(s)) {}
  explicit Value(DictValue d) : data_(std::move(d)) {}
  explicit Value(ListValue l) : data_(std::move(l)) {}
  Value(Value&&) = default;
  Value& operator=(Value&&) = default;

  Type type() const { return static_cast<Type>(data_.index()); }
  bool is_none() const { return type() == Type::NONE; }
  bool is_bool() const { return type() == Type::BOOLEAN; }
  bool is_int() const { return type() == Type::INTEGER; }
  bool is_double() const { return type() == Type::DOUBLE; }
  bool is_string() const { return type() == Type::STRING; }
  bool is_dict() const { return type() == Type::DICT; }
  bool is_list() const { return type() == Type::LIST; }

  std::optional<bool> GetIfBool() const {
    return is_bool() ? std::optional<bool>(std::get<bool>(data_))
                     : std::nullopt;
  }
  std::optional<int> GetIfInt() const {
    return is_int() ? std::optional<int>(std::get<int>(data_)) : std::nullopt;
  }
  const std::string* GetIfString() const {
    return std::get_if<std::string>(&data_);
  }
  const DictValue* GetIfDict() const { return std::get_if<DictValue>(&data_); }
  DictValue* GetIfDict() { return std::get_if<DictValue>(&data_); }
  const ListValue* GetIfList() const { return std::get_if<ListValue>(&data_); }
  ListValue* GetIfList() { return std::get_if<ListValue>(&data_); }
  const std::string& GetString() const {
    CHECK(is_string());
    return std::get<std::string>(data_);
  }
  bool GetBool() const {
    CHECK(is_bool());
    return std::get<bool>(data_);
  }
  std::string DebugString() const;

 private:
  std::variant<std::monostate, bool, int, double, std::string, DictValue,
               ListValue>
      data_;
};

inline void DictValue::Set(std::string_view key, Value&& value) {
  storage_[std::string(key)] = std::make_unique<Value>(std::move(value));
}
inline const Value* DictValue::Find(std::string_view key) const {
  auto it = storage_.find(key);
  return it == storage_.end() ? nullptr : it->second.get();
}
inline Value* DictValue::Find(std::string_view key) {
  auto it = storage_.find(key);
  return it == storage_.end() ? nullptr : it->second.get();
}
inline std::optional<bool> DictValue::FindBool(std::string_view key) const {
  const Value* v = Find(key);
  return v ? v->GetIfBool() : std::nullopt;
}
inline std::optional<int> DictValue::FindInt(std::string_view key) const {
  const Value* v = Find(key);
  return v ? v->GetIfInt() : std::nullopt;
}
inline const std::string* DictValue::FindString(std::string_view key) const {
  const Value* v = Find(key);
  return v ? v->GetIfString() : nullptr;
}
inline const DictValue* DictValue::FindDict(std::string_view key) const {
  const Value* v = Find(key);
  return v ? v->GetIfDict() : nullptr;
}
inline const ListValue* DictValue::FindList(std::string_view key) const {
  const Value* v = Find(key);
  return v ? v->GetIfList() : nullptr;
}
inline ListValue* DictValue::FindList(std::string_view key) {
  Value* v = Find(key);
  return v ? v->GetIfList() : nullptr;
}
inline void ListValue::Append(Value&& value) {
  storage_.push_back(std::make_unique<Value>(std::move(value)));
}

inline std::string Value::DebugString() const {
  std::ostringstream s;
  switch (type()) {
    case Type::NONE:
      s << "null";
      break;
    case Type::BOOLEAN:
      s << (std::get<bool>(data_) ? "true" : "false");
      break;
    case Type::INTEGER:
      s << std::get<int>(data_);
      break;
    case Type::DOUBLE:
      s << std::get<double>(data_);
      break;
    case Type::STRING:
      s << '"' << std::get<std::string>(data_) << '"';
      break;
    case Type::DICT:
      s << "{...}";
      break;
    case Type::LIST:
      s << "[...]";
      break;
  }
  return s.str();
}

// -- base/json/json_reader.h ------------------------------------------------------------

// As base/json/json_reader.h declares them.
enum JSONParserOptions {
  JSON_PARSE_RFC = 0,
  JSON_ALLOW_TRAILING_COMMAS = 1 << 0,
  JSON_REPLACE_INVALID_CHARACTERS = 1 << 1,
  JSON_ALLOW_COMMENTS = 1 << 2,
  JSON_ALLOW_VERT_TAB = 1 << 3,
  JSON_ALLOW_X_ESCAPES = 1 << 4,
  JSON_ALLOW_NEWLINES_IN_STRINGS = 1 << 5,
  JSON_PARSE_CHROMIUM_EXTENSIONS = JSON_ALLOW_COMMENTS |
                                   JSON_ALLOW_NEWLINES_IN_STRINGS |
                                   JSON_ALLOW_X_ESCAPES,
};

namespace internal {

// A recursive-descent JSON parser with //base's options: comments (// and
// /* */), \x escapes, \v escapes, raw \r and \n in strings and trailing
// commas, each only with its option. Integers that fit an int are ints, the
// rest doubles, as //base's reader makes them.
class JsonParser {
 public:
  JsonParser(std::string_view text, int options)
      : s_(text), options_(options) {}

  std::optional<Value> Parse() {
    std::optional<Value> v = ParseValue(0);
    SkipSpace();
    if (!v || ok_ == false || i_ != s_.size()) {
      return std::nullopt;
    }
    return v;
  }

 private:
  void SkipSpace() {
    while (i_ < s_.size()) {
      char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++i_;
      } else if (c == '/' && (options_ & JSON_ALLOW_COMMENTS) &&
                 i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
        while (i_ < s_.size() && s_[i_] != '\n') {
          ++i_;
        }
      } else if (c == '/' && (options_ & JSON_ALLOW_COMMENTS) &&
                 i_ + 1 < s_.size() && s_[i_ + 1] == '*') {
        size_t end = s_.find("*/", i_ + 2);
        if (end == std::string_view::npos) {
          ok_ = false;
          i_ = s_.size();
          return;
        }
        i_ = end + 2;
      } else {
        break;
      }
    }
  }

  bool Consume(std::string_view word) {
    if (s_.substr(i_, word.size()) == word) {
      i_ += word.size();
      return true;
    }
    return false;
  }

  static void AppendUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool Hex(size_t n, uint32_t* out) {
    if (i_ + n > s_.size()) {
      return false;
    }
    uint32_t v = 0;
    for (size_t k = 0; k < n; ++k) {
      char c = s_[i_ + k];
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= c - '0';
      } else if (c >= 'a' && c <= 'f') {
        v |= c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        v |= c - 'A' + 10;
      } else {
        return false;
      }
    }
    i_ += n;
    *out = v;
    return true;
  }

  std::optional<std::string> ParseString() {
    if (i_ >= s_.size() || s_[i_] != '"') {
      return std::nullopt;
    }
    ++i_;
    std::string out;
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') {
        return out;
      }
      if (static_cast<unsigned char>(c) < 0x20 &&
          !((c == '\n' || c == '\r') &&
            (options_ & JSON_ALLOW_NEWLINES_IN_STRINGS))) {
        return std::nullopt;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (i_ >= s_.size()) {
        return std::nullopt;
      }
      char e = s_[i_++];
      uint32_t cp;
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out.push_back(e);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'v':
          if (!(options_ & JSON_ALLOW_VERT_TAB)) {
            return std::nullopt;
          }
          out.push_back('\v');
          break;
        case 'x':
          if (!(options_ & JSON_ALLOW_X_ESCAPES) || !Hex(2, &cp)) {
            return std::nullopt;
          }
          AppendUtf8(&out, cp);
          break;
        case 'u':
          if (!Hex(4, &cp)) {
            return std::nullopt;
          }
          if (cp >= 0xD800 && cp <= 0xDBFF && Consume("\\u")) {
            uint32_t lo;
            if (!Hex(4, &lo) || lo < 0xDC00 || lo > 0xDFFF) {
              return std::nullopt;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          AppendUtf8(&out, cp);
          break;
        default:
          return std::nullopt;
      }
    }
    return std::nullopt;
  }

  std::optional<Value> ParseNumber() {
    size_t start = i_;
    if (i_ < s_.size() && s_[i_] == '-') {
      ++i_;
    }
    bool is_int = true;
    while (i_ < s_.size() &&
           (std::isdigit(static_cast<unsigned char>(s_[i_])) ||
            s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E' ||
            s_[i_] == '+' || s_[i_] == '-')) {
      if (!std::isdigit(static_cast<unsigned char>(s_[i_]))) {
        is_int = false;
      }
      ++i_;
    }
    std::string num(s_.substr(start, i_ - start));
    if (num.empty() || num == "-") {
      return std::nullopt;
    }
    if (is_int) {
      int v;
      if (StringToInt(num, &v)) {
        return Value(v);
      }
    }
    char* end = nullptr;
    double d = std::strtod(num.c_str(), &end);
    if (!end || *end != '\0') {
      return std::nullopt;
    }
    return Value(d);
  }

  std::optional<Value> ParseValue(int depth) {
    if (depth > 200) {
      return std::nullopt;
    }
    SkipSpace();
    if (i_ >= s_.size()) {
      return std::nullopt;
    }
    char c = s_[i_];
    if (c == '{') {
      ++i_;
      DictValue dict;
      SkipSpace();
      if (i_ < s_.size() && s_[i_] == '}') {
        ++i_;
        return Value(std::move(dict));
      }
      while (true) {
        SkipSpace();
        if ((options_ & JSON_ALLOW_TRAILING_COMMAS) && i_ < s_.size() &&
            s_[i_] == '}') {
          ++i_;
          return Value(std::move(dict));
        }
        std::optional<std::string> key = ParseString();
        if (!key) {
          return std::nullopt;
        }
        SkipSpace();
        if (!Consume(":")) {
          return std::nullopt;
        }
        std::optional<Value> v = ParseValue(depth + 1);
        if (!v) {
          return std::nullopt;
        }
        dict.Set(*key, std::move(*v));
        SkipSpace();
        if (Consume(",")) {
          continue;
        }
        if (Consume("}")) {
          return Value(std::move(dict));
        }
        return std::nullopt;
      }
    }
    if (c == '[') {
      ++i_;
      ListValue list;
      SkipSpace();
      if (i_ < s_.size() && s_[i_] == ']') {
        ++i_;
        return Value(std::move(list));
      }
      while (true) {
        SkipSpace();
        if ((options_ & JSON_ALLOW_TRAILING_COMMAS) && i_ < s_.size() &&
            s_[i_] == ']') {
          ++i_;
          return Value(std::move(list));
        }
        std::optional<Value> v = ParseValue(depth + 1);
        if (!v) {
          return std::nullopt;
        }
        list.Append(std::move(*v));
        SkipSpace();
        if (Consume(",")) {
          continue;
        }
        if (Consume("]")) {
          return Value(std::move(list));
        }
        return std::nullopt;
      }
    }
    if (c == '"') {
      std::optional<std::string> str = ParseString();
      if (!str) {
        return std::nullopt;
      }
      return Value(std::move(*str));
    }
    if (Consume("true")) {
      return Value(true);
    }
    if (Consume("false")) {
      return Value(false);
    }
    if (Consume("null")) {
      return Value();
    }
    return ParseNumber();
  }

  std::string_view s_;
  int options_;
  size_t i_ = 0;
  bool ok_ = true;
};

}  // namespace internal

class JSONReader {
 public:
  static std::optional<Value> Read(std::string_view json,
                                   int options = JSON_PARSE_CHROMIUM_EXTENSIONS) {
    return internal::JsonParser(json, options).Parse();
  }
  static std::optional<DictValue> ReadDict(
      std::string_view json,
      int options = JSON_PARSE_CHROMIUM_EXTENSIONS) {
    std::optional<Value> v = Read(json, options);
    if (!v || !v->is_dict()) {
      return std::nullopt;
    }
    return std::move(*v->GetIfDict());
  }
};

}  // namespace base

#endif  // COMPILE_CHECK_SHIM_HSTS_GENERATOR_SHIM_H_
