// compile_check STAND-IN for the macOS SDK's CoreFoundation framework
// (see ../README.md): the declarations Chromium's Mac code and the checked
// files use, with the SDK's names, types and signatures.

#ifndef COMPILE_CHECK_STAND_IN_COREFOUNDATION_H_
#define COMPILE_CHECK_STAND_IN_COREFOUNDATION_H_

#include <Availability.h>
#include <MacTypes.h>
#include <TargetConditionals.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// -- CFBase.h ---------------------------------------------------------------

#if defined(__cplusplus)
#define CF_EXTERN_C_BEGIN extern "C" {
#define CF_EXTERN_C_END }
#else
#define CF_EXTERN_C_BEGIN
#define CF_EXTERN_C_END
#endif
#define CF_EXPORT extern __attribute__((visibility("default")))
#define CF_INLINE static __inline__ __attribute__((always_inline))
#define CF_RETURNS_RETAINED __attribute__((cf_returns_retained))
#define CF_RETURNS_NOT_RETAINED __attribute__((cf_returns_not_retained))
#define CF_RELEASES_ARGUMENT __attribute__((cf_consumed))
#define CF_CONSUMED __attribute__((cf_consumed))
#define CF_NOESCAPE __attribute__((noescape))
#define CF_BRIDGED_TYPE(T) __attribute__((objc_bridge(T)))
#define CF_BRIDGED_MUTABLE_TYPE(T) __attribute__((objc_bridge_mutable(T)))
#define CF_RELATED_TYPE(T, C, I) __attribute__((objc_bridge_related(T, C, I)))
#define CF_IMPLICIT_BRIDGING_ENABLED _Pragma("clang arc_cf_code_audited begin")
#define CF_IMPLICIT_BRIDGING_DISABLED _Pragma("clang arc_cf_code_audited end")
#define CF_ASSUME_NONNULL_BEGIN _Pragma("clang assume_nonnull begin")
#define CF_ASSUME_NONNULL_END _Pragma("clang assume_nonnull end")
#define CF_SWIFT_NAME(x)

// CFBase.h: the Objective-C classes the bridged CF types name.
#if defined(__OBJC__)
@class NSArray;
@class NSAttributedString;
@class NSString;
@class NSNull;
@class NSCharacterSet;
@class NSData;
@class NSDate;
@class NSTimeZone;
@class NSDictionary;
@class NSError;
@class NSLocale;
@class NSNumber;
@class NSSet;
@class NSURL;
#endif
#define CF_REFINED_FOR_SWIFT
#define CF_NOESCAPE __attribute__((noescape))
#define CF_FORMAT_FUNCTION(F, A) __attribute__((format(__CFString__, F, A)))
#define CF_FORMAT_ARGUMENT(A) __attribute__((format_arg(A)))
#define CF_ENUM_AVAILABLE(m, i)
#define CF_AVAILABLE(m, i)
#define CF_AVAILABLE_MAC(m)
#define API_AVAILABLE(...)
#define API_DEPRECATED(...)
#define API_UNAVAILABLE(...)

#if defined(__cplusplus)
#define CF_ENUM(_type, _name) \
  enum _name : _type _name;   \
  enum _name : _type
#define CF_CLOSED_ENUM(_type, _name) CF_ENUM(_type, _name)
#define CF_OPTIONS(_type, _name) \
  _type _name;                   \
  enum : _type
#else
#define CF_ENUM(_type, _name) \
  _type _name;                \
  enum
#define CF_CLOSED_ENUM(_type, _name) CF_ENUM(_type, _name)
#define CF_OPTIONS(_type, _name) \
  _type _name;                   \
  enum
#endif

CF_EXTERN_C_BEGIN

typedef unsigned long CFTypeID;
typedef unsigned long CFOptionFlags;
typedef unsigned long CFHashCode;
typedef signed long CFIndex;

typedef const void* CFTypeRef;

typedef const struct CF_BRIDGED_TYPE(NSString) __CFString* CFStringRef;
typedef struct CF_BRIDGED_MUTABLE_TYPE(NSMutableString) __CFString*
    CFMutableStringRef;
typedef CFTypeRef CFPropertyListRef;

typedef CF_ENUM(CFIndex, CFComparisonResult){
    kCFCompareLessThan = -1L,
    kCFCompareEqualTo = 0,
    kCFCompareGreaterThan = 1};

typedef CFComparisonResult (*CFComparatorFunction)(const void* val1,
                                                   const void* val2,
                                                   void* context);

static const CFIndex kCFNotFound = -1;

typedef struct {
  CFIndex location;
  CFIndex length;
} CFRange;

CF_INLINE CFRange CFRangeMake(CFIndex loc, CFIndex len) {
  CFRange range;
  range.location = loc;
  range.length = len;
  return range;
}

typedef const struct CF_BRIDGED_TYPE(NSNull) __CFNull* CFNullRef;
CF_EXPORT CFTypeID CFNullGetTypeID(void);
CF_EXPORT const CFNullRef kCFNull;

typedef const struct __CFAllocator* CFAllocatorRef;
CF_EXPORT const CFAllocatorRef kCFAllocatorDefault;
CF_EXPORT const CFAllocatorRef kCFAllocatorSystemDefault;
CF_EXPORT const CFAllocatorRef kCFAllocatorMalloc;
CF_EXPORT const CFAllocatorRef kCFAllocatorMallocZone;
CF_EXPORT const CFAllocatorRef kCFAllocatorNull;
CF_EXPORT const CFAllocatorRef kCFAllocatorUseContext;
CF_EXPORT CFTypeID CFAllocatorGetTypeID(void);

CF_EXPORT CFTypeID CFGetTypeID(CFTypeRef cf);
CF_EXPORT CFStringRef CFCopyTypeIDDescription(CFTypeID type_id);
CF_EXPORT CFTypeRef CFRetain(CFTypeRef cf);
CF_EXPORT void CFRelease(CFTypeRef cf);
CF_EXPORT CFTypeRef CFAutorelease(CFTypeRef CF_RELEASES_ARGUMENT arg);
CF_EXPORT CFIndex CFGetRetainCount(CFTypeRef cf);
CF_EXPORT Boolean CFEqual(CFTypeRef cf1, CFTypeRef cf2);
CF_EXPORT CFHashCode CFHash(CFTypeRef cf);
CF_EXPORT CFStringRef CFCopyDescription(CFTypeRef cf);
CF_EXPORT CFAllocatorRef CFGetAllocator(CFTypeRef cf);
CF_EXPORT CFTypeRef CFMakeCollectable(CFTypeRef cf);

// -- CFBoolean, CFNumber ---------------------------------------------------

typedef const struct CF_BRIDGED_TYPE(NSNumber) __CFBoolean* CFBooleanRef;
CF_EXPORT const CFBooleanRef kCFBooleanTrue;
CF_EXPORT const CFBooleanRef kCFBooleanFalse;
CF_EXPORT CFTypeID CFBooleanGetTypeID(void);
CF_EXPORT Boolean CFBooleanGetValue(CFBooleanRef boolean);

typedef CF_ENUM(CFIndex, CFNumberType){
    kCFNumberSInt8Type = 1,     kCFNumberSInt16Type = 2,
    kCFNumberSInt32Type = 3,    kCFNumberSInt64Type = 4,
    kCFNumberFloat32Type = 5,   kCFNumberFloat64Type = 6,
    kCFNumberCharType = 7,      kCFNumberShortType = 8,
    kCFNumberIntType = 9,       kCFNumberLongType = 10,
    kCFNumberLongLongType = 11, kCFNumberFloatType = 12,
    kCFNumberDoubleType = 13,   kCFNumberCFIndexType = 14,
    kCFNumberNSIntegerType = 15, kCFNumberCGFloatType = 16,
    kCFNumberMaxType = 16};

typedef const struct CF_BRIDGED_TYPE(NSNumber) __CFNumber* CFNumberRef;
CF_EXPORT CFTypeID CFNumberGetTypeID(void);
CF_EXPORT CFNumberRef CFNumberCreate(CFAllocatorRef allocator,
                                     CFNumberType theType,
                                     const void* valuePtr);
CF_EXPORT CFNumberType CFNumberGetType(CFNumberRef number);
CF_EXPORT CFIndex CFNumberGetByteSize(CFNumberRef number);
CF_EXPORT Boolean CFNumberIsFloatType(CFNumberRef number);
CF_EXPORT Boolean CFNumberGetValue(CFNumberRef number,
                                   CFNumberType theType,
                                   void* valuePtr);
CF_EXPORT CFComparisonResult CFNumberCompare(CFNumberRef number,
                                             CFNumberRef otherNumber,
                                             void* context);

// -- CFArray ----------------------------------------------------------------

typedef const void* (*CFArrayRetainCallBack)(CFAllocatorRef allocator,
                                             const void* value);
typedef void (*CFArrayReleaseCallBack)(CFAllocatorRef allocator,
                                       const void* value);
typedef CFStringRef (*CFArrayCopyDescriptionCallBack)(const void* value);
typedef Boolean (*CFArrayEqualCallBack)(const void* value1,
                                        const void* value2);
typedef struct {
  CFIndex version;
  CFArrayRetainCallBack retain;
  CFArrayReleaseCallBack release;
  CFArrayCopyDescriptionCallBack copyDescription;
  CFArrayEqualCallBack equal;
} CFArrayCallBacks;
CF_EXPORT const CFArrayCallBacks kCFTypeArrayCallBacks;
typedef void (*CFArrayApplierFunction)(const void* value, void* context);

typedef const struct CF_BRIDGED_TYPE(NSArray) __CFArray* CFArrayRef;
typedef struct CF_BRIDGED_MUTABLE_TYPE(NSMutableArray) __CFArray*
    CFMutableArrayRef;
CF_EXPORT CFTypeID CFArrayGetTypeID(void);
CF_EXPORT CFArrayRef CFArrayCreate(CFAllocatorRef allocator,
                                   const void** values,
                                   CFIndex numValues,
                                   const CFArrayCallBacks* callBacks);
CF_EXPORT CFArrayRef CFArrayCreateCopy(CFAllocatorRef allocator,
                                       CFArrayRef theArray);
CF_EXPORT CFMutableArrayRef CFArrayCreateMutable(
    CFAllocatorRef allocator,
    CFIndex capacity,
    const CFArrayCallBacks* callBacks);
CF_EXPORT CFMutableArrayRef CFArrayCreateMutableCopy(CFAllocatorRef allocator,
                                                     CFIndex capacity,
                                                     CFArrayRef theArray);
CF_EXPORT CFIndex CFArrayGetCount(CFArrayRef theArray);
CF_EXPORT CFIndex CFArrayGetCountOfValue(CFArrayRef theArray,
                                         CFRange range,
                                         const void* value);
CF_EXPORT Boolean CFArrayContainsValue(CFArrayRef theArray,
                                       CFRange range,
                                       const void* value);
CF_EXPORT const void* CFArrayGetValueAtIndex(CFArrayRef theArray,
                                             CFIndex idx);
CF_EXPORT void CFArrayGetValues(CFArrayRef theArray,
                                CFRange range,
                                const void** values);
CF_EXPORT void CFArrayApplyFunction(CFArrayRef theArray,
                                    CFRange range,
                                    CFArrayApplierFunction applier,
                                    void* context);
CF_EXPORT CFIndex CFArrayGetFirstIndexOfValue(CFArrayRef theArray,
                                              CFRange range,
                                              const void* value);
CF_EXPORT void CFArrayAppendValue(CFMutableArrayRef theArray,
                                  const void* value);
CF_EXPORT void CFArrayInsertValueAtIndex(CFMutableArrayRef theArray,
                                         CFIndex idx,
                                         const void* value);
CF_EXPORT void CFArraySetValueAtIndex(CFMutableArrayRef theArray,
                                      CFIndex idx,
                                      const void* value);
CF_EXPORT void CFArrayRemoveValueAtIndex(CFMutableArrayRef theArray,
                                         CFIndex idx);
CF_EXPORT void CFArrayRemoveAllValues(CFMutableArrayRef theArray);
CF_EXPORT void CFArrayAppendArray(CFMutableArrayRef theArray,
                                  CFArrayRef otherArray,
                                  CFRange otherRange);

// -- CFData -----------------------------------------------------------------

typedef const struct CF_BRIDGED_TYPE(NSData) __CFData* CFDataRef;
typedef struct CF_BRIDGED_MUTABLE_TYPE(NSMutableData) __CFData*
    CFMutableDataRef;
CF_EXPORT CFTypeID CFDataGetTypeID(void);
CF_EXPORT CFDataRef CFDataCreate(CFAllocatorRef allocator,
                                 const UInt8* bytes,
                                 CFIndex length);
CF_EXPORT CFDataRef CFDataCreateWithBytesNoCopy(CFAllocatorRef allocator,
                                                const UInt8* bytes,
                                                CFIndex length,
                                                CFAllocatorRef
                                                    bytesDeallocator);
CF_EXPORT CFDataRef CFDataCreateCopy(CFAllocatorRef allocator,
                                     CFDataRef theData);
CF_EXPORT CFMutableDataRef CFDataCreateMutable(CFAllocatorRef allocator,
                                               CFIndex capacity);
CF_EXPORT CFMutableDataRef CFDataCreateMutableCopy(CFAllocatorRef allocator,
                                                   CFIndex capacity,
                                                   CFDataRef theData);
CF_EXPORT CFIndex CFDataGetLength(CFDataRef theData);
CF_EXPORT const UInt8* CFDataGetBytePtr(CFDataRef theData);
CF_EXPORT UInt8* CFDataGetMutableBytePtr(CFMutableDataRef theData);
CF_EXPORT void CFDataGetBytes(CFDataRef theData, CFRange range, UInt8* buffer);
CF_EXPORT void CFDataSetLength(CFMutableDataRef theData, CFIndex length);
CF_EXPORT void CFDataAppendBytes(CFMutableDataRef theData,
                                 const UInt8* bytes,
                                 CFIndex length);

// -- CFDictionary -----------------------------------------------------------

typedef const void* (*CFDictionaryRetainCallBack)(CFAllocatorRef allocator,
                                                  const void* value);
typedef void (*CFDictionaryReleaseCallBack)(CFAllocatorRef allocator,
                                            const void* value);
typedef CFStringRef (*CFDictionaryCopyDescriptionCallBack)(const void* value);
typedef Boolean (*CFDictionaryEqualCallBack)(const void* value1,
                                             const void* value2);
typedef CFHashCode (*CFDictionaryHashCallBack)(const void* value);
typedef struct {
  CFIndex version;
  CFDictionaryRetainCallBack retain;
  CFDictionaryReleaseCallBack release;
  CFDictionaryCopyDescriptionCallBack copyDescription;
  CFDictionaryEqualCallBack equal;
  CFDictionaryHashCallBack hash;
} CFDictionaryKeyCallBacks;
typedef struct {
  CFIndex version;
  CFDictionaryRetainCallBack retain;
  CFDictionaryReleaseCallBack release;
  CFDictionaryCopyDescriptionCallBack copyDescription;
  CFDictionaryEqualCallBack equal;
} CFDictionaryValueCallBacks;
CF_EXPORT const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
CF_EXPORT const CFDictionaryKeyCallBacks kCFCopyStringDictionaryKeyCallBacks;
CF_EXPORT const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
typedef void (*CFDictionaryApplierFunction)(const void* key,
                                            const void* value,
                                            void* context);

typedef const struct CF_BRIDGED_TYPE(NSDictionary) __CFDictionary*
    CFDictionaryRef;
typedef struct CF_BRIDGED_MUTABLE_TYPE(NSMutableDictionary) __CFDictionary*
    CFMutableDictionaryRef;
CF_EXPORT CFTypeID CFDictionaryGetTypeID(void);
CF_EXPORT CFDictionaryRef CFDictionaryCreate(
    CFAllocatorRef allocator,
    const void** keys,
    const void** values,
    CFIndex numValues,
    const CFDictionaryKeyCallBacks* keyCallBacks,
    const CFDictionaryValueCallBacks* valueCallBacks);
CF_EXPORT CFDictionaryRef CFDictionaryCreateCopy(CFAllocatorRef allocator,
                                                 CFDictionaryRef theDict);
CF_EXPORT CFMutableDictionaryRef CFDictionaryCreateMutable(
    CFAllocatorRef allocator,
    CFIndex capacity,
    const CFDictionaryKeyCallBacks* keyCallBacks,
    const CFDictionaryValueCallBacks* valueCallBacks);
CF_EXPORT CFMutableDictionaryRef CFDictionaryCreateMutableCopy(
    CFAllocatorRef allocator,
    CFIndex capacity,
    CFDictionaryRef theDict);
CF_EXPORT CFIndex CFDictionaryGetCount(CFDictionaryRef theDict);
CF_EXPORT Boolean CFDictionaryContainsKey(CFDictionaryRef theDict,
                                          const void* key);
CF_EXPORT Boolean CFDictionaryContainsValue(CFDictionaryRef theDict,
                                            const void* value);
CF_EXPORT const void* CFDictionaryGetValue(CFDictionaryRef theDict,
                                           const void* key);
CF_EXPORT Boolean CFDictionaryGetValueIfPresent(CFDictionaryRef theDict,
                                                const void* key,
                                                const void** value);
CF_EXPORT void CFDictionaryGetKeysAndValues(CFDictionaryRef theDict,
                                            const void** keys,
                                            const void** values);
CF_EXPORT void CFDictionaryApplyFunction(CFDictionaryRef theDict,
                                         CFDictionaryApplierFunction applier,
                                         void* context);
CF_EXPORT void CFDictionaryAddValue(CFMutableDictionaryRef theDict,
                                    const void* key,
                                    const void* value);
CF_EXPORT void CFDictionarySetValue(CFMutableDictionaryRef theDict,
                                    const void* key,
                                    const void* value);
CF_EXPORT void CFDictionaryReplaceValue(CFMutableDictionaryRef theDict,
                                        const void* key,
                                        const void* value);
CF_EXPORT void CFDictionaryRemoveValue(CFMutableDictionaryRef theDict,
                                       const void* key);
CF_EXPORT void CFDictionaryRemoveAllValues(CFMutableDictionaryRef theDict);

// -- CFSet, CFBag -----------------------------------------------------------

typedef const struct CF_BRIDGED_TYPE(NSSet) __CFSet* CFSetRef;
typedef struct CF_BRIDGED_MUTABLE_TYPE(NSMutableSet) __CFSet* CFMutableSetRef;
CF_EXPORT CFTypeID CFSetGetTypeID(void);
CF_EXPORT CFIndex CFSetGetCount(CFSetRef theSet);
CF_EXPORT Boolean CFSetContainsValue(CFSetRef theSet, const void* value);
CF_EXPORT void CFSetGetValues(CFSetRef theSet, const void** values);
typedef const struct CF_BRIDGED_TYPE(id) __CFBag* CFBagRef;
typedef struct CF_BRIDGED_MUTABLE_TYPE(id) __CFBag* CFMutableBagRef;
CF_EXPORT CFTypeID CFBagGetTypeID(void);

// -- CFString ---------------------------------------------------------------

typedef UInt32 CFStringEncoding;
typedef CF_ENUM(CFStringEncoding, CFStringBuiltInEncodings){
    kCFStringEncodingMacRoman = 0,
    kCFStringEncodingWindowsLatin1 = 0x0500,
    kCFStringEncodingISOLatin1 = 0x0201,
    kCFStringEncodingNextStepLatin = 0x0B01,
    kCFStringEncodingASCII = 0x0600,
    kCFStringEncodingUnicode = 0x0100,
    kCFStringEncodingUTF8 = 0x08000100,
    kCFStringEncodingNonLossyASCII = 0x0BFF,
    kCFStringEncodingUTF16 = 0x0100,
    kCFStringEncodingUTF16BE = 0x10000100,
    kCFStringEncodingUTF16LE = 0x14000100,
    kCFStringEncodingUTF32 = 0x0c000100,
    kCFStringEncodingUTF32BE = 0x18000100,
    kCFStringEncodingUTF32LE = 0x1c000100};
static const CFStringEncoding kCFStringEncodingInvalidId = 0xffffffffU;

typedef CF_OPTIONS(CFOptionFlags, CFStringCompareFlags){
    kCFCompareCaseInsensitive = 1,
    kCFCompareBackwards = 4,
    kCFCompareAnchored = 8,
    kCFCompareNonliteral = 16,
    kCFCompareLocalized = 32,
    kCFCompareNumerically = 64,
    kCFCompareDiacriticInsensitive = 128,
    kCFCompareWidthInsensitive = 256,
    kCFCompareForcedOrdering = 512};

typedef const struct CF_BRIDGED_TYPE(NSLocale) __CFLocale* CFLocaleRef;
typedef const struct CF_BRIDGED_TYPE(NSCharacterSet) __CFCharacterSet*
    CFCharacterSetRef;
typedef struct CF_BRIDGED_MUTABLE_TYPE(NSMutableCharacterSet) __CFCharacterSet*
    CFMutableCharacterSetRef;

#define CFSTR(cStr) \
  ((CFStringRef)__builtin___CFStringMakeConstantString("" cStr ""))

CF_EXPORT CFTypeID CFStringGetTypeID(void);
CF_EXPORT CFStringRef CFStringCreateWithCString(CFAllocatorRef alloc,
                                                const char* cStr,
                                                CFStringEncoding encoding);
CF_EXPORT CFStringRef CFStringCreateWithBytes(CFAllocatorRef alloc,
                                              const UInt8* bytes,
                                              CFIndex numBytes,
                                              CFStringEncoding encoding,
                                              Boolean isExternalRepresentation);
CF_EXPORT CFStringRef CFStringCreateWithBytesNoCopy(
    CFAllocatorRef alloc,
    const UInt8* bytes,
    CFIndex numBytes,
    CFStringEncoding encoding,
    Boolean isExternalRepresentation,
    CFAllocatorRef contentsDeallocator);
CF_EXPORT CFStringRef CFStringCreateWithCharacters(CFAllocatorRef alloc,
                                                   const UniChar* chars,
                                                   CFIndex numChars);
CF_EXPORT CFStringRef CFStringCreateWithCharactersNoCopy(
    CFAllocatorRef alloc,
    const UniChar* chars,
    CFIndex numChars,
    CFAllocatorRef contentsDeallocator);
CF_EXPORT CFStringRef CFStringCreateWithFormat(CFAllocatorRef alloc,
                                               CFDictionaryRef formatOptions,
                                               CFStringRef format,
                                               ...) CF_FORMAT_FUNCTION(3, 4);
CF_EXPORT CFStringRef CFStringCreateCopy(CFAllocatorRef alloc,
                                         CFStringRef theString);
CF_EXPORT CFStringRef CFStringCreateWithSubstring(CFAllocatorRef alloc,
                                                  CFStringRef str,
                                                  CFRange range);
CF_EXPORT CFMutableStringRef CFStringCreateMutable(CFAllocatorRef alloc,
                                                   CFIndex maxLength);
CF_EXPORT CFMutableStringRef CFStringCreateMutableCopy(CFAllocatorRef alloc,
                                                       CFIndex maxLength,
                                                       CFStringRef theString);
CF_EXPORT CFIndex CFStringGetLength(CFStringRef theString);
CF_EXPORT UniChar CFStringGetCharacterAtIndex(CFStringRef theString,
                                              CFIndex idx);
CF_EXPORT void CFStringGetCharacters(CFStringRef theString,
                                     CFRange range,
                                     UniChar* buffer);
CF_EXPORT const UniChar* CFStringGetCharactersPtr(CFStringRef theString);
CF_EXPORT Boolean CFStringGetCString(CFStringRef theString,
                                     char* buffer,
                                     CFIndex bufferSize,
                                     CFStringEncoding encoding);
CF_EXPORT const char* CFStringGetCStringPtr(CFStringRef theString,
                                            CFStringEncoding encoding);
CF_EXPORT CFIndex CFStringGetBytes(CFStringRef theString,
                                   CFRange range,
                                   CFStringEncoding encoding,
                                   UInt8 lossByte,
                                   Boolean isExternalRepresentation,
                                   UInt8* buffer,
                                   CFIndex maxBufLen,
                                   CFIndex* usedBufLen);
CF_EXPORT CFIndex CFStringGetMaximumSizeForEncoding(CFIndex length,
                                                    CFStringEncoding encoding);
CF_EXPORT CFComparisonResult CFStringCompare(CFStringRef theString1,
                                             CFStringRef theString2,
                                             CFStringCompareFlags
                                                 compareOptions);
CF_EXPORT CFComparisonResult CFStringCompareWithOptions(
    CFStringRef theString1,
    CFStringRef theString2,
    CFRange rangeToCompare,
    CFStringCompareFlags compareOptions);
CF_EXPORT Boolean CFStringHasPrefix(CFStringRef theString,
                                    CFStringRef prefix);
CF_EXPORT Boolean CFStringHasSuffix(CFStringRef theString,
                                    CFStringRef suffix);
CF_EXPORT CFRange CFStringFind(CFStringRef theString,
                               CFStringRef stringToFind,
                               CFStringCompareFlags compareOptions);
CF_EXPORT void CFStringAppend(CFMutableStringRef theString,
                              CFStringRef appendedString);
CF_EXPORT void CFStringAppendCString(CFMutableStringRef theString,
                                     const char* cStr,
                                     CFStringEncoding encoding);
CF_EXPORT void CFStringLowercase(CFMutableStringRef theString,
                                 CFLocaleRef locale);
CF_EXPORT void CFStringUppercase(CFMutableStringRef theString,
                                 CFLocaleRef locale);
CF_EXPORT SInt32 CFStringGetIntValue(CFStringRef str);
CF_EXPORT double CFStringGetDoubleValue(CFStringRef str);
CF_EXPORT CFArrayRef CFStringCreateArrayBySeparatingStrings(
    CFAllocatorRef alloc,
    CFStringRef theString,
    CFStringRef separatorString);
CF_EXPORT void CFShow(CFTypeRef obj);

// -- CFCharacterSet ---------------------------------------------------------

typedef UInt32 UTF32Char;
CF_EXPORT CFTypeID CFCharacterSetGetTypeID(void);
CF_EXPORT Boolean CFCharacterSetIsCharacterMember(CFCharacterSetRef theSet,
                                                  UniChar theChar);
CF_EXPORT Boolean CFCharacterSetIsLongCharacterMember(
    CFCharacterSetRef theSet,
    UTF32Char theChar);
CF_EXPORT CFCharacterSetRef CFCharacterSetCreateWithCharactersInString(
    CFAllocatorRef alloc,
    CFStringRef theString);
CF_EXPORT CFMutableCharacterSetRef CFCharacterSetCreateMutable(
    CFAllocatorRef alloc);

// -- CFAttributedString -----------------------------------------------------

typedef const struct CF_BRIDGED_TYPE(NSAttributedString) __CFAttributedString*
    CFAttributedStringRef;
typedef struct CF_BRIDGED_MUTABLE_TYPE(NSMutableAttributedString)
    __CFAttributedString* CFMutableAttributedStringRef;
CF_EXPORT CFTypeID CFAttributedStringGetTypeID(void);
CF_EXPORT CFAttributedStringRef CFAttributedStringCreate(
    CFAllocatorRef alloc,
    CFStringRef str,
    CFDictionaryRef attributes);
CF_EXPORT CFStringRef CFAttributedStringGetString(CFAttributedStringRef aStr);
CF_EXPORT CFIndex CFAttributedStringGetLength(CFAttributedStringRef aStr);

// -- CFDate, CFTimeZone, CFCalendar, CFLocale -------------------------------

typedef double CFTimeInterval;
typedef CFTimeInterval CFAbsoluteTime;
CF_EXPORT const CFTimeInterval kCFAbsoluteTimeIntervalSince1970;
CF_EXPORT const CFTimeInterval kCFAbsoluteTimeIntervalSince1904;
CF_EXPORT CFAbsoluteTime CFAbsoluteTimeGetCurrent(void);
typedef const struct CF_BRIDGED_TYPE(NSDate) __CFDate* CFDateRef;
CF_EXPORT CFTypeID CFDateGetTypeID(void);
CF_EXPORT CFDateRef CFDateCreate(CFAllocatorRef allocator, CFAbsoluteTime at);
CF_EXPORT CFAbsoluteTime CFDateGetAbsoluteTime(CFDateRef theDate);
typedef const struct CF_BRIDGED_TYPE(NSTimeZone) __CFTimeZone* CFTimeZoneRef;
CF_EXPORT CFTypeID CFTimeZoneGetTypeID(void);
CF_EXPORT CFTimeZoneRef CFTimeZoneCopySystem(void);
CF_EXPORT CFTimeZoneRef CFTimeZoneCopyDefault(void);
CF_EXPORT CFStringRef CFTimeZoneGetName(CFTimeZoneRef tz);
typedef struct CF_BRIDGED_TYPE(NSCalendar) __CFCalendar* CFCalendarRef;
CF_EXPORT CFTypeID CFCalendarGetTypeID(void);
CF_EXPORT CFTypeID CFLocaleGetTypeID(void);
CF_EXPORT CFLocaleRef CFLocaleCopyCurrent(void);
CF_EXPORT CFArrayRef CFLocaleCopyPreferredLanguages(void);
CF_EXPORT CFStringRef CFLocaleGetIdentifier(CFLocaleRef locale);

// -- CFError ----------------------------------------------------------------

typedef struct CF_BRIDGED_TYPE(NSError) __CFError* CFErrorRef;
CF_EXPORT CFTypeID CFErrorGetTypeID(void);
CF_EXPORT CFStringRef CFErrorGetDomain(CFErrorRef err);
CF_EXPORT CFIndex CFErrorGetCode(CFErrorRef err);
CF_EXPORT CFStringRef CFErrorCopyDescription(CFErrorRef err);
CF_EXPORT CFDictionaryRef CFErrorCopyUserInfo(CFErrorRef err);

// -- CFPropertyList.h --------------------------------------------------------

typedef CF_OPTIONS(CFOptionFlags, CFPropertyListMutabilityOptions){
    kCFPropertyListImmutable = 0,
    kCFPropertyListMutableContainers = 1 << 0,
    kCFPropertyListMutableContainersAndLeaves = 1 << 1,
};

typedef CF_ENUM(CFIndex, CFPropertyListFormat){
    kCFPropertyListOpenStepFormat = 1,
    kCFPropertyListXMLFormat_v1_0 = 100,
    kCFPropertyListBinaryFormat_v1_0 = 200};

CF_EXPORT CFPropertyListRef CFPropertyListCreateWithData(
    CFAllocatorRef allocator,
    CFDataRef data,
    CFOptionFlags options,
    CFPropertyListFormat* format,
    CFErrorRef* error);
CF_EXPORT CFDataRef CFPropertyListCreateData(CFAllocatorRef allocator,
                                             CFPropertyListRef propertyList,
                                             CFPropertyListFormat format,
                                             CFOptionFlags options,
                                             CFErrorRef* error);

// -- CFURL, CFUUID, CFBundle ------------------------------------------------

typedef const struct CF_BRIDGED_TYPE(NSURL) __CFURL* CFURLRef;
CF_EXPORT CFTypeID CFURLGetTypeID(void);
typedef CF_ENUM(CFIndex, CFURLPathStyle){kCFURLPOSIXPathStyle = 0,
                                         kCFURLWindowsPathStyle = 2};
CF_EXPORT CFURLRef CFURLCreateWithBytes(CFAllocatorRef allocator,
                                        const UInt8* URLBytes,
                                        CFIndex length,
                                        CFStringEncoding encoding,
                                        CFURLRef baseURL);
CF_EXPORT CFURLRef CFURLCreateWithString(CFAllocatorRef allocator,
                                         CFStringRef URLString,
                                         CFURLRef baseURL);
CF_EXPORT CFURLRef CFURLCreateWithFileSystemPath(CFAllocatorRef allocator,
                                                 CFStringRef filePath,
                                                 CFURLPathStyle pathStyle,
                                                 Boolean isDirectory);
CF_EXPORT CFURLRef CFURLCreateFromFileSystemRepresentation(
    CFAllocatorRef allocator,
    const UInt8* buffer,
    CFIndex bufLen,
    Boolean isDirectory);
CF_EXPORT Boolean CFURLGetFileSystemRepresentation(CFURLRef url,
                                                   Boolean resolveAgainstBase,
                                                   UInt8* buffer,
                                                   CFIndex maxBufLen);
CF_EXPORT CFStringRef CFURLGetString(CFURLRef anURL);
CF_EXPORT CFStringRef CFURLCopyFileSystemPath(CFURLRef anURL,
                                              CFURLPathStyle pathStyle);
typedef const struct CF_BRIDGED_TYPE(id) __CFUUID* CFUUIDRef;
CF_EXPORT CFTypeID CFUUIDGetTypeID(void);
CF_EXPORT CFUUIDRef CFUUIDCreate(CFAllocatorRef alloc);
CF_EXPORT CFStringRef CFUUIDCreateString(CFAllocatorRef alloc, CFUUIDRef uuid);
typedef struct CF_BRIDGED_TYPE(id) __CFBundle* CFBundleRef;
CF_EXPORT CFTypeID CFBundleGetTypeID(void);
CF_EXPORT CFBundleRef CFBundleGetMainBundle(void);
CF_EXPORT CFBundleRef CFBundleGetBundleWithIdentifier(CFStringRef bundleID);
CF_EXPORT CFStringRef CFBundleGetIdentifier(CFBundleRef bundle);
CF_EXPORT CFTypeRef CFBundleGetValueForInfoDictionaryKey(CFBundleRef bundle,
                                                         CFStringRef key);
CF_EXPORT CFDictionaryRef CFBundleGetInfoDictionary(CFBundleRef bundle);
CF_EXPORT void CFBundleGetPackageInfo(CFBundleRef bundle,
                                      UInt32* packageType,
                                      UInt32* packageCreator);
CF_EXPORT CFURLRef CFBundleCopyBundleURL(CFBundleRef bundle);
CF_EXPORT void* CFBundleGetFunctionPointerForName(CFBundleRef bundle,
                                                  CFStringRef functionName);
CF_EXPORT const CFStringRef kCFBundleVersionKey;
CF_EXPORT const CFStringRef kCFBundleIdentifierKey;
CF_EXPORT const CFStringRef kCFBundleNameKey;

// -- CFRunLoop, CFStream (the types bridging.h names) -----------------------

typedef struct CF_BRIDGED_MUTABLE_TYPE(NSTimer) __CFRunLoopTimer*
    CFRunLoopTimerRef;
CF_EXPORT CFTypeID CFRunLoopTimerGetTypeID(void);
typedef struct __CFRunLoop* CFRunLoopRef;
typedef struct __CFRunLoopSource* CFRunLoopSourceRef;
CF_EXPORT CFRunLoopRef CFRunLoopGetCurrent(void);
CF_EXPORT CFRunLoopRef CFRunLoopGetMain(void);
CF_EXPORT const CFStringRef kCFRunLoopDefaultMode;
CF_EXPORT const CFStringRef kCFRunLoopCommonModes;

typedef struct __CFRunLoopObserver* CFRunLoopObserverRef;

typedef CF_OPTIONS(CFOptionFlags, CFRunLoopActivity){
    kCFRunLoopEntry = (1UL << 0),         kCFRunLoopBeforeTimers = (1UL << 1),
    kCFRunLoopBeforeSources = (1UL << 2), kCFRunLoopBeforeWaiting = (1UL << 5),
    kCFRunLoopAfterWaiting = (1UL << 6),  kCFRunLoopExit = (1UL << 7),
    kCFRunLoopAllActivities = 0x0FFFFFFFU};

typedef CF_ENUM(SInt32, CFRunLoopRunResult){
    kCFRunLoopRunFinished = 1, kCFRunLoopRunStopped = 2,
    kCFRunLoopRunTimedOut = 3, kCFRunLoopRunHandledSource = 4};

typedef struct {
  CFIndex version;
  void* info;
  const void* (*retain)(const void* info);
  void (*release)(const void* info);
  CFStringRef (*copyDescription)(const void* info);
  Boolean (*equal)(const void* info1, const void* info2);
  CFHashCode (*hash)(const void* info);
  void (*schedule)(void* info, CFRunLoopRef rl, CFStringRef mode);
  void (*cancel)(void* info, CFRunLoopRef rl, CFStringRef mode);
  void (*perform)(void* info);
} CFRunLoopSourceContext;

typedef struct {
  CFIndex version;
  void* info;
  const void* (*retain)(const void* info);
  void (*release)(const void* info);
  CFStringRef (*copyDescription)(const void* info);
} CFRunLoopObserverContext;

typedef void (*CFRunLoopObserverCallBack)(CFRunLoopObserverRef observer,
                                          CFRunLoopActivity activity,
                                          void* info);

CF_EXPORT CFStringRef CFRunLoopCopyCurrentMode(CFRunLoopRef rl);
CF_EXPORT void CFRunLoopRun(void);
CF_EXPORT CFRunLoopRunResult CFRunLoopRunInMode(
    CFStringRef mode, CFTimeInterval seconds,
    Boolean returnAfterSourceHandled);
CF_EXPORT void CFRunLoopWakeUp(CFRunLoopRef rl);
CF_EXPORT void CFRunLoopPerformBlock(CFRunLoopRef rl, CFTypeRef mode,
                                     void (^block)(void));
CF_EXPORT void CFRunLoopStop(CFRunLoopRef rl);
CF_EXPORT Boolean CFRunLoopIsWaiting(CFRunLoopRef rl);
CF_EXPORT void CFRunLoopAddSource(CFRunLoopRef rl, CFRunLoopSourceRef source,
                                  CFStringRef mode);
CF_EXPORT void CFRunLoopRemoveSource(CFRunLoopRef rl,
                                     CFRunLoopSourceRef source,
                                     CFStringRef mode);
CF_EXPORT void CFRunLoopAddObserver(CFRunLoopRef rl,
                                    CFRunLoopObserverRef observer,
                                    CFStringRef mode);
CF_EXPORT void CFRunLoopRemoveObserver(CFRunLoopRef rl,
                                       CFRunLoopObserverRef observer,
                                       CFStringRef mode);
CF_EXPORT void CFRunLoopAddTimer(CFRunLoopRef rl, CFRunLoopTimerRef timer,
                                 CFStringRef mode);
CF_EXPORT void CFRunLoopRemoveTimer(CFRunLoopRef rl, CFRunLoopTimerRef timer,
                                    CFStringRef mode);
CF_EXPORT CFRunLoopSourceRef CFRunLoopSourceCreate(
    CFAllocatorRef allocator, CFIndex order,
    CFRunLoopSourceContext* context);
CF_EXPORT void CFRunLoopSourceInvalidate(CFRunLoopSourceRef source);
CF_EXPORT void CFRunLoopSourceSignal(CFRunLoopSourceRef source);
CF_EXPORT CFRunLoopObserverRef CFRunLoopObserverCreate(
    CFAllocatorRef allocator, CFOptionFlags activities, Boolean repeats,
    CFIndex order, CFRunLoopObserverCallBack callout,
    CFRunLoopObserverContext* context);
CF_EXPORT void CFRunLoopObserverInvalidate(CFRunLoopObserverRef observer);
typedef struct CF_BRIDGED_MUTABLE_TYPE(NSInputStream) __CFReadStream*
    CFReadStreamRef;
typedef struct CF_BRIDGED_MUTABLE_TYPE(NSOutputStream) __CFWriteStream*
    CFWriteStreamRef;
CF_EXPORT CFTypeID CFReadStreamGetTypeID(void);
CF_EXPORT CFTypeID CFWriteStreamGetTypeID(void);

// -- CFPreferences ----------------------------------------------------------

CF_EXPORT const CFStringRef kCFPreferencesCurrentApplication;
CF_EXPORT const CFStringRef kCFPreferencesAnyApplication;
CF_EXPORT CFPropertyListRef CFPreferencesCopyAppValue(CFStringRef key,
                                                      CFStringRef
                                                          applicationID);
CF_EXPORT Boolean CFPreferencesGetAppBooleanValue(CFStringRef key,
                                                  CFStringRef applicationID,
                                                  Boolean* keyExistsAndHasValidFormat);
CF_EXPORT CFIndex CFPreferencesGetAppIntegerValue(
    CFStringRef key, CFStringRef applicationID,
    Boolean* keyExistsAndHasValidFormat);
CF_EXPORT void CFPreferencesSetAppValue(CFStringRef key,
                                        CFPropertyListRef value,
                                        CFStringRef applicationID);
CF_EXPORT Boolean CFPreferencesAppSynchronize(CFStringRef applicationID);

CF_EXTERN_C_END

#endif  // COMPILE_CHECK_STAND_IN_COREFOUNDATION_H_
