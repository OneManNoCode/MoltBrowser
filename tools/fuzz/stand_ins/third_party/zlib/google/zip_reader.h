// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's
// third_party/zlib/google/zip_reader.h, for tools/fuzz/molt_fuzz.py only.
// The real reader (minizip) walks the central directory; this one walks
// local file headers in order, stored and deflated entries only, which is
// enough to drive MoltBrowser's callers (entry iteration, path checks, the
// read cap) with the same contract:
// ExtractCurrentEntryToString() returns false, with |max_read_bytes| bytes
// in |*output|, when the entry is bigger, and false with what was read so
// far on an error.

#ifndef MOLT_FUZZ_STAND_INS_THIRD_PARTY_ZLIB_GOOGLE_ZIP_READER_H_
#define MOLT_FUZZ_STAND_INS_THIRD_PARTY_ZLIB_GOOGLE_ZIP_READER_H_

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include "base/files/file_path.h"

namespace zip {

class ZipReader {
 public:
  struct Entry {
    base::FilePath path;
    int64_t original_size = 0;
    bool is_directory = false;
    bool is_unsafe = false;
    bool is_encrypted = false;
    bool is_symbolic_link = false;
  };

  ZipReader() = default;
  ZipReader(const ZipReader&) = delete;
  ZipReader& operator=(const ZipReader&) = delete;

  bool OpenFromString(const std::string& data) {
    data_ = data;
    pos_ = 0;
    // A zip starts with a local file header or (empty) the end record.
    return data_.size() >= 4 && (Le32(0) == 0x04034b50 || Le32(0) == 0x06054b50);
  }

  const Entry* Next() {
    if (pos_ + 30 > data_.size() || Le32(pos_) != 0x04034b50) return nullptr;
    const uint16_t flags = Le16(pos_ + 6);
    method_ = Le16(pos_ + 8);
    const uint32_t compressed = Le32(pos_ + 18);
    const uint32_t uncompressed = Le32(pos_ + 22);
    const uint16_t name_len = Le16(pos_ + 26);
    const uint16_t extra_len = Le16(pos_ + 28);
    const size_t name_at = pos_ + 30;
    const size_t data_at = name_at + name_len + extra_len;
    // Sizes in a data descriptor (flag bit 3) aren't supported here.
    if ((flags & 8) || data_at > data_.size() ||
        compressed > data_.size() - data_at) {
      return nullptr;
    }
    const std::string name = data_.substr(name_at, name_len);
    entry_ = Entry();
    entry_.path = base::FilePath(name);
    entry_.original_size = uncompressed;
    entry_.is_directory = !name.empty() && name.back() == '/';
    entry_.is_encrypted = flags & 1;
    entry_.is_unsafe = name.find("..") != std::string::npos ||
                       (!name.empty() && name[0] == '/');
    body_ = std::string_view(data_).substr(data_at, compressed);
    pos_ = data_at + compressed;
    return &entry_;
  }

  bool ExtractCurrentEntryToString(uint64_t max_read_bytes,
                                   std::string* output) const {
    output->clear();
    if (entry_.is_directory) return true;
    if (method_ == 0) {
      output->assign(body_.substr(
          0, static_cast<size_t>(std::min<uint64_t>(max_read_bytes,
                                                    body_.size()))));
      return body_.size() <= max_read_bytes;
    }
    if (method_ != 8) return false;
    z_stream zs{};
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(body_.data()));
    zs.avail_in = static_cast<uInt>(body_.size());
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    char buf[16384];
    bool ok = false;
    while (true) {
      zs.next_out = reinterpret_cast<Bytef*>(buf);
      zs.avail_out = sizeof(buf);
      const int r = inflate(&zs, Z_NO_FLUSH);
      const size_t got = sizeof(buf) - zs.avail_out;
      const uint64_t room = max_read_bytes - output->size();
      output->append(buf, static_cast<size_t>(std::min<uint64_t>(got, room)));
      if (got > room) break;  // bigger than the cap
      if (r == Z_STREAM_END) {
        ok = true;
        break;
      }
      if (r != Z_OK || (got == 0 && zs.avail_in == 0)) break;
    }
    inflateEnd(&zs);
    return ok;
  }

 private:
  uint16_t Le16(size_t at) const {
    return static_cast<uint16_t>(static_cast<uint8_t>(data_[at]) |
                                 static_cast<uint8_t>(data_[at + 1]) << 8);
  }
  uint32_t Le32(size_t at) const {
    return static_cast<uint32_t>(Le16(at)) |
           static_cast<uint32_t>(Le16(at + 2)) << 16;
  }

  std::string data_;
  size_t pos_ = 0;
  uint16_t method_ = 0;
  std::string_view body_;
  Entry entry_;
};

}  // namespace zip

#endif  // MOLT_FUZZ_STAND_INS_THIRD_PARTY_ZLIB_GOOGLE_ZIP_READER_H_
