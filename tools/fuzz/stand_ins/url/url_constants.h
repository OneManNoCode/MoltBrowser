// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's url/url_constants.h (subset),
// for tools/fuzz/molt_fuzz.py only.

#ifndef MOLT_FUZZ_STAND_INS_URL_URL_CONSTANTS_H_
#define MOLT_FUZZ_STAND_INS_URL_URL_CONSTANTS_H_

namespace url {

inline constexpr char kHttpScheme[] = "http";
inline constexpr char kHttpsScheme[] = "https";
inline constexpr char kAboutScheme[] = "about";
inline constexpr char kDataScheme[] = "data";
inline constexpr char kFileScheme[] = "file";
inline constexpr char kJavaScriptScheme[] = "javascript";
inline constexpr char kStandardSchemeSeparator[] = "://";
inline constexpr int PORT_UNSPECIFIED = -1;

}  // namespace url

#endif  // MOLT_FUZZ_STAND_INS_URL_URL_CONSTANTS_H_
