// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt_os_crypt — where MoltBrowser's encrypted stores get their encryption:
// the API-key store (keys.enc), the password vault (vault.enc), the
// form-filler profile (profile.enc), saved AI-chat conversations and the
// Personal Vector Memory database.
//
// Chromium 152 deleted the synchronous OSCrypt. What replaces it is
// os_crypt_async::OSCryptAsync, owned by BrowserProcessImpl
// (g_browser_process->os_crypt_async()). Once its key providers have
// answered, it hands out an immutable, thread-safe os_crypt_async::Encryptor.
// That happens asynchronously, after startup has begun. Init() fetches that
// Encryptor once. The stores then encrypt and decrypt through it
// synchronously, from whatever worker sequence they run on.
//
// Until the Encryptor has arrived, every call fails closed. Encryption fails,
// so callers refuse to write and nothing is ever written in plaintext.
// Decryption fails with DecryptFlags::temporarily_unavailable set.
//
// Data written by the old OSCrypt decrypts unchanged: the 152 key providers
// derive the same keys and use the same version tags. On Linux that is "v11"
// for the Secret Service / KWallet key and "v10" for the hard-coded fallback
// key; macOS uses "v10" (Keychain), Windows "v10" (DPAPI). The details are in
// molt_os_crypt.cc.
//
// Threading: call Init() and RunWhenReady() on the UI thread (the sequence
// OSCryptAsync lives on). Everything else may be called on any thread. The
// Encryptor is set once and never changes afterwards, apart from the test
// hook below.

#ifndef CHROME_BROWSER_MOLT_AI_KEYS_MOLT_OS_CRYPT_H_
#define CHROME_BROWSER_MOLT_AI_KEYS_MOLT_OS_CRYPT_H_

#include <string>

#include "base/functional/callback_forward.h"
#include "base/memory/scoped_refptr.h"
#include "components/os_crypt/async/common/encryptor.h"

namespace os_crypt_async {
class OSCryptAsync;
}  // namespace os_crypt_async

namespace molt_ai {
namespace molt_os_crypt {

// Starts fetching the Encryptor from |os_crypt_async| (normally
// g_browser_process->os_crypt_async()). Call on the UI thread. Only the first
// call with a non-null |os_crypt_async| has any effect; later calls are
// no-ops.
void Init(os_crypt_async::OSCryptAsync* os_crypt_async);

// True once the Encryptor has arrived.
bool IsReady();

// Runs |callback| on the calling (UI) sequence once the Encryptor has
// arrived. If it is already there, the callback runs before this returns.
void RunWhenReady(base::OnceClosure callback);

// True when new data can be encrypted. Any key counts here, including
// Linux's hard-coded "v10" key. The secret stores apply the stricter
// molt_ai::IsSecureStorageAvailable() (keys/molt_keys_store.h).
bool IsEncryptionAvailable();

// Encrypts |plaintext| with the current encryption key. The result starts
// with that key's tag. An empty |plaintext| encrypts to an empty string, as
// OSCrypt did. Returns false, leaving |ciphertext| empty, if the Encryptor
// isn't ready or has no key to encrypt with.
bool EncryptString(const std::string& plaintext, std::string* ciphertext);

// Decrypts what EncryptString() (or the old OSCrypt) wrote. Returns false if
// the data is corrupt or no available key matches its tag. When |flags| is
// non-null, flags->temporarily_unavailable is set if the key may come back
// (the Encryptor isn't ready yet, or the key provider reported a temporary
// failure such as a locked keychain). flags->should_reencrypt is set when the
// data was decrypted with a key other than the current encryption key.
bool DecryptString(const std::string& ciphertext,
                   std::string* plaintext,
                   os_crypt_async::Encryptor::DecryptFlags* flags = nullptr);

// Test hook. Makes |encryptor| the process-wide Encryptor while this object
// is alive and restores the previous state when it goes away. A null
// |encryptor| means "not ready yet", and it also lets Init() run again. A
// non-null one runs pending RunWhenReady() callbacks. Not for production code.
class ScopedEncryptorForTesting {
 public:
  explicit ScopedEncryptorForTesting(
      scoped_refptr<os_crypt_async::Encryptor> encryptor);
  ScopedEncryptorForTesting(const ScopedEncryptorForTesting&) = delete;
  ScopedEncryptorForTesting& operator=(const ScopedEncryptorForTesting&) =
      delete;
  ~ScopedEncryptorForTesting();

  // Swaps in another Encryptor mid-test (e.g. the keyring went away).
  void Set(scoped_refptr<os_crypt_async::Encryptor> encryptor);

 private:
  scoped_refptr<os_crypt_async::Encryptor> previous_;
  bool previous_init_started_ = false;
};

}  // namespace molt_os_crypt
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_KEYS_MOLT_OS_CRYPT_H_
