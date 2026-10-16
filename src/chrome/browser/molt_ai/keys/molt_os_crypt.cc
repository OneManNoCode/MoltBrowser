// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Compatibility with data the old (sync) OSCrypt wrote. Checked against the
// Chromium 152 sources of components/os_crypt/async and the 148 sources of
// components/os_crypt/sync:
//
//   Linux  "v10": PosixKeyProvider (always registered, precedence 5) holds
//          the same hard-coded key, PBKDF2-HMAC-SHA1("peanuts", "saltysalt",
//          1 iteration), AES-128-CBC with an IV of 16 spaces.
//          "v11": FreedesktopSecretKeyProvider (precedence 10) reads the same
//          secret OSCrypt's libsecret / KWallet backends stored: the
//          libsecret item with application="chromium" (schema
//          chrome_libsecret_os_crypt_password_v2), or KWallet folder
//          "Chromium Keys", entry "Chromium Safe Storage". It derives the key
//          the same way (PBKDF2-HMAC-SHA1, "saltysalt", 1 iteration,
//          AES-128-CBC). Both still try the empty-password key when the real
//          one fails (crbug.com/40055416).
//          "v12": SecretPortalKeyProvider (Flatpak/Snap secret portal,
//          AES-256-GCM, precedence 15) is new. It encrypts only when
//          kSecretPortalKeyProviderUseForEncryption is on (off by default).
//   macOS  "v10": KeychainKeyProvider (kUseKeychainKeyProvider, on by
//          default) uses the same KeychainPassword item ("Chromium Safe
//          Storage") and derivation (PBKDF2-HMAC-SHA1, "saltysalt", 1003
//          iterations, AES-128-CBC).
//   Win    "v10": DPAPIKeyProvider unwraps the same Local State key
//          (os_crypt.encrypted_key, "DPAPI" + CryptProtectData blob), which
//          is used for AES-256-GCM with the 12-byte nonce after the tag.
//          "v20" (app-bound) is new and used only when upstream turns it on.
//
// As before, an empty plaintext encrypts to an empty string (148's Windows
// OSCrypt wrote a tagged blob for it, which still decrypts), and data without
// a known tag fails to decrypt. 148's Windows OSCrypt still tried raw DPAPI on
// untagged data, a pre-2020 Chrome format MoltBrowser never wrote.

#include "chrome/browser/molt_ai/keys/molt_os_crypt.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"

namespace molt_ai {
namespace molt_os_crypt {

namespace {

using os_crypt_async::Encryptor;

struct State {
  base::Lock lock;
  // Set once (on the UI thread) when OSCryptAsync delivers it; immutable and
  // thread-safe after that. Every user takes a reference under |lock| and
  // works on its own copy of the pointer.
  scoped_refptr<Encryptor> encryptor GUARDED_BY(lock);
  bool init_started GUARDED_BY(lock) = false;
  std::vector<base::OnceClosure> pending GUARDED_BY(lock);
};

State& GetState() {
  static base::NoDestructor<State> state;
  return *state;
}

scoped_refptr<Encryptor> CurrentEncryptor() {
  State& state = GetState();
  base::AutoLock lock(state.lock);
  return state.encryptor;
}

// Installs |encryptor| (may be null) and, if it is non-null, runs the
// RunWhenReady() callbacks queued so far, outside the lock.
void SetEncryptor(scoped_refptr<Encryptor> encryptor) {
  std::vector<base::OnceClosure> to_run;
  {
    State& state = GetState();
    base::AutoLock lock(state.lock);
    state.encryptor = std::move(encryptor);
    if (state.encryptor) {
      to_run.swap(state.pending);
    }
  }
  for (base::OnceClosure& callback : to_run) {
    std::move(callback).Run();
  }
}

void OnEncryptorReady(scoped_refptr<Encryptor> encryptor) {
  if (!encryptor) {
    return;  // OSCryptAsync always supplies one; stay "not ready" otherwise.
  }
  {
    State& state = GetState();
    base::AutoLock lock(state.lock);
    if (state.encryptor) {
      return;  // Set once. (A test hook may have got there first.)
    }
  }
  SetEncryptor(std::move(encryptor));
}

}  // namespace

void Init(os_crypt_async::OSCryptAsync* os_crypt_async) {
  if (!os_crypt_async) {
    return;
  }
  {
    State& state = GetState();
    base::AutoLock lock(state.lock);
    if (state.init_started || state.encryptor) {
      return;
    }
    state.init_started = true;
  }
  // Not under the lock: the callback may run before GetInstance() returns.
  os_crypt_async->GetInstance(base::BindOnce(&OnEncryptorReady));
}

bool IsReady() {
  return !!CurrentEncryptor();
}

void RunWhenReady(base::OnceClosure callback) {
  {
    State& state = GetState();
    base::AutoLock lock(state.lock);
    if (!state.encryptor) {
      state.pending.push_back(std::move(callback));
      return;
    }
  }
  std::move(callback).Run();
}

bool IsEncryptionAvailable() {
  scoped_refptr<Encryptor> encryptor = CurrentEncryptor();
  return encryptor && encryptor->IsEncryptionAvailable();
}

bool EncryptString(const std::string& plaintext, std::string* ciphertext) {
  ciphertext->clear();
  scoped_refptr<Encryptor> encryptor = CurrentEncryptor();
  if (!encryptor) {
    return false;
  }
  std::string out;
  if (!encryptor->EncryptString(plaintext, &out)) {
    return false;
  }
  // Belt and braces for "never write plaintext": non-empty input always
  // yields a tag plus ciphertext.
  if (!plaintext.empty() && out.empty()) {
    return false;
  }
  *ciphertext = std::move(out);
  return true;
}

bool DecryptString(const std::string& ciphertext,
                   std::string* plaintext,
                   Encryptor::DecryptFlags* flags) {
  plaintext->clear();
  if (flags) {
    *flags = Encryptor::DecryptFlags();
  }
  scoped_refptr<Encryptor> encryptor = CurrentEncryptor();
  if (!encryptor) {
    if (flags) {
      flags->temporarily_unavailable = true;
    }
    return false;
  }
  return encryptor->DecryptString(ciphertext, plaintext, flags);
}

ScopedEncryptorForTesting::ScopedEncryptorForTesting(
    scoped_refptr<Encryptor> encryptor) {
  {
    State& state = GetState();
    base::AutoLock lock(state.lock);
    previous_ = state.encryptor;
    previous_init_started_ = state.init_started;
    state.init_started = false;
  }
  Set(std::move(encryptor));
}

ScopedEncryptorForTesting::~ScopedEncryptorForTesting() {
  std::vector<base::OnceClosure> dropped;
  {
    State& state = GetState();
    base::AutoLock lock(state.lock);
    state.encryptor = std::move(previous_);
    state.init_started = previous_init_started_;
    // Callbacks queued during the test must not run in a later one.
    dropped.swap(state.pending);
  }
}

void ScopedEncryptorForTesting::Set(scoped_refptr<Encryptor> encryptor) {
  SetEncryptor(std::move(encryptor));
}

}  // namespace molt_os_crypt
}  // namespace molt_ai
