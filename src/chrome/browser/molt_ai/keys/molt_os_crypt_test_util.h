// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Encryptors for MoltBrowser's store tests. A real
// os_crypt_async::OSCryptAsync builds them from key providers that answer
// synchronously, so tags, provider precedence and the decrypt rules are the
// Chromium code's own. Install one with
// molt_ai::molt_os_crypt::ScopedEncryptorForTesting.

#ifndef CHROME_BROWSER_MOLT_AI_KEYS_MOLT_OS_CRYPT_TEST_UTIL_H_
#define CHROME_BROWSER_MOLT_AI_KEYS_MOLT_OS_CRYPT_TEST_UTIL_H_

#include <cstdint>

#include "base/memory/scoped_refptr.h"
#include "build/build_config.h"
#include "components/os_crypt/async/common/encryptor.h"

namespace molt_ai {
namespace test {

// The tag an OS-backed key writes in these tests: "v11" (Secret Service /
// KWallet) on Linux, "v10" (Keychain / DPAPI) elsewhere.
extern const char kOsKeyTag[];

// An OS-backed key. A different |key_seed| is another machine's (or another
// keyring's) key. On Linux the hard-coded "v10" key is registered as well,
// as in the browser, so old files still decrypt.
scoped_refptr<os_crypt_async::Encryptor> MakeOsKeyEncryptor(
    uint8_t key_seed = 1);

// The OS key exists but is locked (its provider reports it temporarily
// unavailable), so data under it can't be decrypted for now. On Linux new
// data falls back to the hard-coded "v10" key. Elsewhere nothing can be
// encrypted.
scoped_refptr<os_crypt_async::Encryptor> MakeLockedOsKeyEncryptor();

// An Encryptor without any key: every operation fails.
scoped_refptr<os_crypt_async::Encryptor> MakeEncryptorWithoutKeys();

#if BUILDFLAG(IS_LINUX)
// Linux without a system keyring (or with --password-store=basic): only the
// real hard-coded-key PosixKeyProvider ("v10").
scoped_refptr<os_crypt_async::Encryptor> MakeHardCodedKeyEncryptor();

// The XDG secret portal key ("v12", AES-256-GCM) encrypting, as with
// kSecretPortalKeyProviderUseForEncryption. The Secret Service and the
// hard-coded key are registered too.
scoped_refptr<os_crypt_async::Encryptor> MakeSecretPortalEncryptor();
#endif  // BUILDFLAG(IS_LINUX)

}  // namespace test
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_KEYS_MOLT_OS_CRYPT_TEST_UTIL_H_
