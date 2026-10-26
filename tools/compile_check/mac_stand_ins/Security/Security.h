// compile_check STAND-IN for the macOS SDK's Security framework (see
// ../README.md): the declarations Chromium's Mac code and the checked
// files use, with the SDK's names, types and signatures.

#ifndef COMPILE_CHECK_STAND_IN_SECURITY_H_
#define COMPILE_CHECK_STAND_IN_SECURITY_H_

#include <CoreFoundation/CoreFoundation.h>

CF_EXTERN_C_BEGIN

// -- SecBase.h ---------------------------------------------------------------

typedef struct CF_BRIDGED_TYPE(id) __SecCertificate* SecCertificateRef;
typedef struct CF_BRIDGED_TYPE(id) __SecIdentity* SecIdentityRef;
typedef struct CF_BRIDGED_TYPE(id) __SecKey* SecKeyRef;
typedef struct CF_BRIDGED_TYPE(id) __SecPolicy* SecPolicyRef;
typedef struct CF_BRIDGED_TYPE(id) __SecAccessControl* SecAccessControlRef;
typedef struct CF_BRIDGED_TYPE(id) __SecKeychain* SecKeychainRef;
typedef struct CF_BRIDGED_TYPE(id) __SecKeychainItem* SecKeychainItemRef;
typedef struct CF_BRIDGED_TYPE(id) __SecTrust* SecTrustRef;
typedef struct CF_BRIDGED_TYPE(id) __SecAccess* SecAccessRef;
typedef struct CF_BRIDGED_TYPE(id) __SecRequirement* SecRequirementRef;
typedef struct CF_BRIDGED_TYPE(id) __SecCode* SecCodeRef;
typedef struct CF_BRIDGED_TYPE(id) __SecCode* SecStaticCodeRef;

enum {
  errSecSuccess = 0,
  errSecUnimplemented = -4,
  errSecParam = -50,
  errSecAllocate = -108,
  errSecNotAvailable = -25291,
  errSecDuplicateItem = -25299,
  errSecItemNotFound = -25300,
  errSecInteractionNotAllowed = -25308,
  errSecAuthFailed = -25293,
};

CF_EXPORT CFStringRef SecCopyErrorMessageString(OSStatus status,
                                                void* reserved);

CF_EXPORT CFTypeID SecCertificateGetTypeID(void);
CF_EXPORT CFTypeID SecKeyGetTypeID(void);
CF_EXPORT CFTypeID SecPolicyGetTypeID(void);
CF_EXPORT CFTypeID SecAccessControlGetTypeID(void);
CF_EXPORT CFTypeID SecTrustGetTypeID(void);
CF_EXPORT CFTypeID SecIdentityGetTypeID(void);

CF_EXPORT SecCertificateRef SecCertificateCreateWithData(
    CFAllocatorRef allocator,
    CFDataRef data);
CF_EXPORT CFDataRef SecCertificateCopyData(SecCertificateRef certificate);

// -- SecItem.h ---------------------------------------------------------------

CF_EXPORT const CFStringRef kSecClass;
CF_EXPORT const CFStringRef kSecClassGenericPassword;
CF_EXPORT const CFStringRef kSecClassInternetPassword;
CF_EXPORT const CFStringRef kSecClassCertificate;
CF_EXPORT const CFStringRef kSecClassKey;
CF_EXPORT const CFStringRef kSecClassIdentity;
CF_EXPORT const CFStringRef kSecAttrService;
CF_EXPORT const CFStringRef kSecAttrAccount;
CF_EXPORT const CFStringRef kSecAttrLabel;
CF_EXPORT const CFStringRef kSecAttrAccessible;
CF_EXPORT const CFStringRef kSecReturnData;
CF_EXPORT const CFStringRef kSecReturnAttributes;
CF_EXPORT const CFStringRef kSecReturnRef;
CF_EXPORT const CFStringRef kSecMatchLimit;
CF_EXPORT const CFStringRef kSecMatchLimitOne;
CF_EXPORT const CFStringRef kSecMatchLimitAll;
CF_EXPORT const CFStringRef kSecValueData;
CF_EXPORT const CFStringRef kSecUseDataProtectionKeychain;

CF_EXPORT OSStatus SecItemCopyMatching(CFDictionaryRef query,
                                       CFTypeRef* result);
CF_EXPORT OSStatus SecItemAdd(CFDictionaryRef attributes, CFTypeRef* result);
CF_EXPORT OSStatus SecItemUpdate(CFDictionaryRef query,
                                 CFDictionaryRef attributesToUpdate);
CF_EXPORT OSStatus SecItemDelete(CFDictionaryRef query);

CF_EXTERN_C_END

#endif  // COMPILE_CHECK_STAND_IN_SECURITY_H_
