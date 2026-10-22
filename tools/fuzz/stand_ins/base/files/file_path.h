// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/files/file_path.h (POSIX,
// the subset MoltBrowser's parsers use), for tools/fuzz/molt_fuzz.py only.

#ifndef MOLT_FUZZ_STAND_INS_BASE_FILES_FILE_PATH_H_
#define MOLT_FUZZ_STAND_INS_BASE_FILES_FILE_PATH_H_

#include <string>
#include <string_view>
#include <utility>

namespace base {

class FilePath {
 public:
  using StringType = std::string;
  using CharType = char;

  FilePath() = default;
  explicit FilePath(std::string_view path) : path_(path) {}

  const StringType& value() const { return path_; }
  bool empty() const { return path_.empty(); }
  std::string AsUTF8Unsafe() const { return path_; }
  static FilePath FromUTF8Unsafe(std::string_view utf8) {
    return FilePath(utf8);
  }
  FilePath BaseName() const {
    const size_t slash = path_.rfind('/');
    return slash == std::string::npos ? *this
                                      : FilePath(path_.substr(slash + 1));
  }
  FilePath Append(std::string_view component) const {
    if (path_.empty()) return FilePath(component);
    std::string out = path_;
    if (out.back() != '/') out += '/';
    out += component;
    return FilePath(out);
  }
  bool operator==(const FilePath& other) const { return path_ == other.path_; }

 private:
  std::string path_;
};

}  // namespace base

#endif  // MOLT_FUZZ_STAND_INS_BASE_FILES_FILE_PATH_H_
