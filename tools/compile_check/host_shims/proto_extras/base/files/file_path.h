// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// compile_check STAND-IN for //base in a host tool (see base/check.h):
// the POSIX base::FilePath members the plugin calls, with their behaviour
// (base/files/file_path.cc: ReplaceExtension() drops the final component's
// extension and appends the new one, adding the '.' if it has none).

#ifndef COMPILE_CHECK_SHIM_BASE_FILES_FILE_PATH_H_
#define COMPILE_CHECK_SHIM_BASE_FILES_FILE_PATH_H_

#include <string>
#include <string_view>

#define FILE_PATH_LITERAL(x) x

namespace base {

class FilePath {
 public:
  using StringType = std::string;

  FilePath() = default;
  explicit FilePath(std::string_view path) : path_(path) {}

  static FilePath FromASCII(std::string_view path) { return FilePath(path); }

  const StringType& value() const { return path_; }
  std::string AsUTF8Unsafe() const { return path_; }

  // The extension of the final component, '.' included ("" if none; ""
  // for "." and ".."). //base also knows a few double extensions
  // (".tar.gz", ".user.js"); a .proto file name has none of them.
  StringType FinalExtension() const {
    const size_t slash = path_.find_last_of('/');
    const size_t start = slash == StringType::npos ? 0 : slash + 1;
    const std::string_view base = std::string_view(path_).substr(start);
    if (base == "." || base == "..") {
      return StringType();
    }
    const size_t dot = base.find_last_of('.');
    return dot == std::string_view::npos ? StringType()
                                         : StringType(base.substr(dot));
  }

  FilePath RemoveFinalExtension() const {
    const StringType ext = FinalExtension();
    return FilePath(std::string_view(path_).substr(0, path_.size() -
                                                         ext.size()));
  }

  FilePath ReplaceExtension(std::string_view extension) const {
    const size_t slash = path_.find_last_of('/');
    const std::string_view base =
        std::string_view(path_).substr(slash == StringType::npos ? 0
                                                                 : slash + 1);
    if (base.empty() || base == "." || base == "..") {
      return FilePath();
    }
    FilePath no_ext = RemoveFinalExtension();
    if (extension.empty() || extension == ".") {
      return no_ext;
    }
    StringType str = no_ext.path_;
    if (extension[0] != '.' && str.back() != '.') {
      str.push_back('.');
    }
    str.append(extension);
    return FilePath(str);
  }

 private:
  StringType path_;
};

}  // namespace base

#endif  // COMPILE_CHECK_SHIM_BASE_FILES_FILE_PATH_H_
