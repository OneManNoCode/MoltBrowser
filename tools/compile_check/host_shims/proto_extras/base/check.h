// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// compile_check STAND-IN for //base, for building one host tool here:
// components/proto_extras/proto_extras_plugin.cc (the protoc plugin that
// writes the .equal.h, .to_value.h and .ostream.h headers). The plugin uses
// only CHECK, NOTREACHED, flat_set, FilePath::FromASCII/ReplaceExtension/
// AsUTF8Unsafe, StrCat, ToUpperASCII and ReplaceChars from //base; these
// headers implement exactly those, with //base's behaviour. Linking the
// real //base into a host tool would need most of it (logging, threading,
// allocator shims). See tools/compile_check/README.md.

#ifndef COMPILE_CHECK_SHIM_BASE_CHECK_H_
#define COMPILE_CHECK_SHIM_BASE_CHECK_H_

#include <cstdlib>
#include <iostream>
#include <sstream>

namespace compile_check_shim {

class FatalMessage {
 public:
  FatalMessage(const char* file, int line, const char* what) {
    stream_ << file << ":" << line << ": " << what << " ";
  }
  [[noreturn]] ~FatalMessage() {
    std::cerr << stream_.str() << std::endl;
    std::abort();
  }
  std::ostream& stream() { return stream_; }

 private:
  std::ostringstream stream_;
};

// Swallows the stream of a CHECK that passed.
struct Voidify {
  void operator&(std::ostream&) {}
};

}  // namespace compile_check_shim

#define CHECK(condition)                                           \
  (condition) ? (void)0                                            \
              : ::compile_check_shim::Voidify() &                  \
                    ::compile_check_shim::FatalMessage(            \
                        __FILE__, __LINE__, "Check failed: " #condition) \
                        .stream()

#define DCHECK(condition) CHECK(condition)

#endif  // COMPILE_CHECK_SHIM_BASE_CHECK_H_
