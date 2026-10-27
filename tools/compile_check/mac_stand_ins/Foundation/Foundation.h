// compile_check STAND-IN for the macOS SDK's Foundation framework (see
// ../README.md): the classes, methods and functions Chromium's Mac code
// and the checked files use, with the SDK's names and signatures.

#ifndef COMPILE_CHECK_STAND_IN_FOUNDATION_H_
#define COMPILE_CHECK_STAND_IN_FOUNDATION_H_

#include <CoreFoundation/CoreFoundation.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>

#if defined(__OBJC__)
#import <objc/NSObjCRuntime.h>
#import <objc/NSObject.h>
#import <objc/objc.h>
#import <objc/runtime.h>
#else
#include <objc/NSObjCRuntime.h>
#include <objc/objc.h>
#endif

// -- NSObjCRuntime.h ---------------------------------------------------------

#if defined(__cplusplus)
#define FOUNDATION_EXTERN extern "C"
#else
#define FOUNDATION_EXTERN extern
#endif
#define FOUNDATION_EXPORT FOUNDATION_EXTERN
#define FOUNDATION_STATIC_INLINE static __inline__
#define NS_ENUM(_type, _name) CF_ENUM(_type, _name)
#define NS_CLOSED_ENUM(_type, _name) CF_CLOSED_ENUM(_type, _name)
#define NS_OPTIONS(_type, _name) CF_OPTIONS(_type, _name)
#define NS_ERROR_ENUM(_type, _name, _domain) CF_ENUM(_type, _name)
#define NS_STRING_ENUM
#define NS_EXTENSIBLE_STRING_ENUM
#define NS_TYPED_ENUM
#define NS_TYPED_EXTENSIBLE_ENUM
#define NS_SWIFT_NAME(x)
#define NS_SWIFT_UNAVAILABLE(x)
#define NS_SWIFT_SENDABLE
#define NS_SWIFT_UI_ACTOR
#define NS_REFINED_FOR_SWIFT
#define NS_ASSUME_NONNULL_BEGIN _Pragma("clang assume_nonnull begin")
#define NS_ASSUME_NONNULL_END _Pragma("clang assume_nonnull end")
#define NS_DESIGNATED_INITIALIZER __attribute__((objc_designated_initializer))
#define NS_UNAVAILABLE __attribute__((unavailable))
#define NS_REQUIRES_SUPER __attribute__((objc_requires_super))
#define NS_RETURNS_RETAINED __attribute__((ns_returns_retained))
#define NS_RETURNS_NOT_RETAINED __attribute__((ns_returns_not_retained))
#define NS_RETURNS_INNER_POINTER __attribute__((objc_returns_inner_pointer))
#define NS_RELEASES_ARGUMENT __attribute__((ns_consumed))
#define NS_FORMAT_FUNCTION(F, A) __attribute__((format(__NSString__, F, A)))
#define NS_FORMAT_ARGUMENT(A) __attribute__((format_arg(A)))
#define NS_REQUIRES_NIL_TERMINATION __attribute__((sentinel(0, 1)))
#define NS_NOESCAPE __attribute__((noescape))
#define NS_AVAILABLE(m, i)
#define NS_AVAILABLE_MAC(m)
#define NS_DEPRECATED_MAC(...)
#define NS_ENUM_AVAILABLE(m, i)
#define NS_INLINE static __inline__ __attribute__((always_inline))
#define NS_NONATOMIC_IOSONLY nonatomic
#define NS_AUTOMATED_REFCOUNT_UNAVAILABLE
#define NS_HEADER_AUDIT_BEGIN(...)
#define NS_HEADER_AUDIT_END(...)

typedef double NSTimeInterval;

typedef struct _NSRange {
  NSUInteger location;
  NSUInteger length;
} NSRange;
typedef NSRange* NSRangePointer;

NS_INLINE NSRange NSMakeRange(NSUInteger loc, NSUInteger len) {
  NSRange r;
  r.location = loc;
  r.length = len;
  return r;
}

static const NSInteger NSNotFound = LONG_MAX;

typedef NS_ENUM(NSInteger, NSComparisonResult){
    NSOrderedAscending = -1L, NSOrderedSame, NSOrderedDescending};

typedef NSUInteger NSStringEncoding;
enum {
  NSASCIIStringEncoding = 1,
  NSUTF8StringEncoding = 4,
  NSISOLatin1StringEncoding = 5,
  NSUnicodeStringEncoding = 10,
  NSUTF16StringEncoding = NSUnicodeStringEncoding,
  NSUTF16BigEndianStringEncoding = 0x90000100,
  NSUTF16LittleEndianStringEncoding = 0x94000100,
  NSUTF32StringEncoding = 0x8c000100,
};

typedef NS_OPTIONS(NSUInteger, NSStringCompareOptions){
    NSCaseInsensitiveSearch = 1,     NSLiteralSearch = 2,
    NSBackwardsSearch = 4,           NSAnchoredSearch = 8,
    NSNumericSearch = 64,            NSDiacriticInsensitiveSearch = 128,
    NSWidthInsensitiveSearch = 256,  NSForcedOrderingSearch = 512,
    NSRegularExpressionSearch = 1024};

#if defined(__OBJC__)

typedef struct _NSZone NSZone;
typedef unsigned short unichar;

@class NSString, NSData, NSURL, NSError, NSLocale, NSDate, NSNumber,
    NSCoder, NSBundle, NSNotification, NSRunLoop, NSTimer, NSMutableString,
    NSMutableData, NSAttributedString, NSCharacterSet, NSUUID, NSIndexSet,
    NSValue, NSOperationQueue;
@class NSArray<ObjectType>, NSMutableArray<ObjectType>, NSSet<ObjectType>,
    NSMutableSet<ObjectType>, NSEnumerator<ObjectType>,
    NSDictionary<KeyType, ObjectType>, NSMutableDictionary<KeyType, ObjectType>;

typedef NSString* NSErrorDomain;
typedef NSString* NSNotificationName;
typedef NSString* NSRunLoopMode;
typedef NSString* NSAttributedStringKey;
typedef NSString* NSErrorUserInfoKey;

// -- Protocols ---------------------------------------------------------------

@protocol NSCopying
- (id)copyWithZone:(nullable NSZone*)zone;
@end

@protocol NSMutableCopying
- (id)mutableCopyWithZone:(nullable NSZone*)zone;
@end

@protocol NSCoding
- (void)encodeWithCoder:(NSCoder*)coder;
- (nullable instancetype)initWithCoder:(NSCoder*)coder;
@end

@protocol NSSecureCoding <NSCoding>
@property(class, readonly) BOOL supportsSecureCoding;
@end

typedef struct {
  unsigned long state;
  id __unsafe_unretained _Nullable* _Nullable itemsPtr;
  unsigned long* _Nullable mutationsPtr;
  unsigned long extra[5];
} NSFastEnumerationState;

@protocol NSFastEnumeration
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState*)state
                                  objects:(id __unsafe_unretained _Nullable[_Nonnull])buffer
                                    count:(NSUInteger)len;
@end

// -- NSObject additions ------------------------------------------------------

@interface NSObject (NSComparisonMethods)
- (BOOL)isEqualTo:(nullable id)object;
@end

@interface NSObject (NSDelayedPerforming)
- (void)performSelector:(SEL)aSelector
             withObject:(nullable id)anArgument
             afterDelay:(NSTimeInterval)delay;
@end

@interface NSObject (NSThreadPerformAdditions)
- (void)performSelectorOnMainThread:(SEL)aSelector
                         withObject:(nullable id)arg
                      waitUntilDone:(BOOL)wait;
@end

// -- NSString ----------------------------------------------------------------

@interface NSString : NSObject <NSCopying, NSMutableCopying, NSSecureCoding>
@property(readonly) NSUInteger length;
- (unichar)characterAtIndex:(NSUInteger)index;
- (instancetype)init NS_DESIGNATED_INITIALIZER;
@end

@interface NSString (NSStringExtensionMethods)
+ (instancetype)string;
+ (instancetype)stringWithString:(NSString*)string;
+ (nullable instancetype)stringWithUTF8String:(const char*)nullTerminatedCString;
+ (instancetype)stringWithFormat:(NSString*)format, ... NS_FORMAT_FUNCTION(1, 2);
+ (instancetype)stringWithCharacters:(const unichar*)characters
                              length:(NSUInteger)length;
+ (nullable instancetype)stringWithCString:(const char*)cString
                                  encoding:(NSStringEncoding)enc;
- (nullable instancetype)initWithUTF8String:(const char*)nullTerminatedCString;
- (instancetype)initWithFormat:(NSString*)format, ... NS_FORMAT_FUNCTION(1, 2);
- (nullable instancetype)initWithBytes:(const void*)bytes
                                length:(NSUInteger)len
                              encoding:(NSStringEncoding)encoding;
- (nullable instancetype)initWithData:(NSData*)data
                             encoding:(NSStringEncoding)encoding;
- (instancetype)initWithCharacters:(const unichar*)characters
                            length:(NSUInteger)length;
- (instancetype)initWithString:(NSString*)aString;
@property(nullable, readonly) const char* UTF8String NS_RETURNS_INNER_POINTER;
- (BOOL)isEqualToString:(NSString*)aString;
- (BOOL)hasPrefix:(NSString*)str;
- (BOOL)hasSuffix:(NSString*)str;
- (NSComparisonResult)compare:(NSString*)string;
- (NSComparisonResult)compare:(NSString*)string
                      options:(NSStringCompareOptions)mask;
- (NSComparisonResult)caseInsensitiveCompare:(NSString*)string;
- (NSComparisonResult)localizedCaseInsensitiveCompare:(NSString*)string;
- (NSRange)rangeOfString:(NSString*)searchString;
- (NSRange)rangeOfString:(NSString*)searchString
                 options:(NSStringCompareOptions)mask;
- (BOOL)containsString:(NSString*)str;
- (NSString*)substringFromIndex:(NSUInteger)from;
- (NSString*)substringToIndex:(NSUInteger)to;
- (NSString*)substringWithRange:(NSRange)range;
- (NSString*)stringByAppendingString:(NSString*)aString;
- (NSString*)stringByAppendingFormat:(NSString*)format, ... NS_FORMAT_FUNCTION(1, 2);
- (NSString*)stringByReplacingOccurrencesOfString:(NSString*)target
                                       withString:(NSString*)replacement;
- (NSString*)stringByTrimmingCharactersInSet:(NSCharacterSet*)set;
- (NSArray<NSString*>*)componentsSeparatedByString:(NSString*)separator;
- (nullable NSData*)dataUsingEncoding:(NSStringEncoding)encoding;
- (void)getCharacters:(unichar*)buffer range:(NSRange)range;
- (NSUInteger)lengthOfBytesUsingEncoding:(NSStringEncoding)enc;
@property(readonly, copy) NSString* lowercaseString;
@property(readonly, copy) NSString* uppercaseString;
@property(readonly) int intValue;
@property(readonly) NSInteger integerValue;
@property(readonly) double doubleValue;
@property(readonly) BOOL boolValue;
@property(readonly) const char* fileSystemRepresentation;
@property(readonly, copy) NSString* lastPathComponent;
@property(readonly, copy) NSString* stringByDeletingLastPathComponent;
@property(readonly, copy) NSString* pathExtension;
- (NSString*)stringByAppendingPathComponent:(NSString*)str;
@end

@interface NSMutableString : NSString
+ (NSMutableString*)stringWithCapacity:(NSUInteger)capacity;
- (void)appendString:(NSString*)aString;
- (void)appendFormat:(NSString*)format, ... NS_FORMAT_FUNCTION(1, 2);
- (void)setString:(NSString*)aString;
- (void)replaceCharactersInRange:(NSRange)range withString:(NSString*)aString;
- (void)insertString:(NSString*)aString atIndex:(NSUInteger)loc;
- (void)deleteCharactersInRange:(NSRange)range;
@end

FOUNDATION_EXPORT NSString* NSStringFromSelector(SEL aSelector);
FOUNDATION_EXPORT SEL NSSelectorFromString(NSString* aSelectorName);
FOUNDATION_EXPORT NSString* NSStringFromClass(Class aClass);
FOUNDATION_EXPORT Class _Nullable NSClassFromString(NSString* aClassName);
FOUNDATION_EXPORT NSString* NSStringFromRange(NSRange range);
FOUNDATION_EXPORT void NSLog(NSString* format, ...) NS_FORMAT_FUNCTION(1, 2);

// -- NSValue, NSNumber -------------------------------------------------------

@interface NSValue : NSObject <NSCopying, NSSecureCoding>
+ (NSValue*)valueWithPointer:(nullable const void*)pointer;
+ (NSValue*)valueWithRange:(NSRange)range;
@property(nullable, readonly) void* pointerValue;
@property(readonly) NSRange rangeValue;
@end

@interface NSNumber : NSValue
+ (NSNumber*)numberWithChar:(char)value;
+ (NSNumber*)numberWithUnsignedChar:(unsigned char)value;
+ (NSNumber*)numberWithShort:(short)value;
+ (NSNumber*)numberWithUnsignedShort:(unsigned short)value;
+ (NSNumber*)numberWithInt:(int)value;
+ (NSNumber*)numberWithUnsignedInt:(unsigned int)value;
+ (NSNumber*)numberWithLong:(long)value;
+ (NSNumber*)numberWithUnsignedLong:(unsigned long)value;
+ (NSNumber*)numberWithLongLong:(long long)value;
+ (NSNumber*)numberWithUnsignedLongLong:(unsigned long long)value;
+ (NSNumber*)numberWithFloat:(float)value;
+ (NSNumber*)numberWithDouble:(double)value;
+ (NSNumber*)numberWithBool:(BOOL)value;
+ (NSNumber*)numberWithInteger:(NSInteger)value;
+ (NSNumber*)numberWithUnsignedInteger:(NSUInteger)value;
- (NSNumber*)initWithInt:(int)value;
- (NSNumber*)initWithDouble:(double)value;
- (NSNumber*)initWithBool:(BOOL)value;
@property(readonly) char charValue;
@property(readonly) unsigned char unsignedCharValue;
@property(readonly) short shortValue;
@property(readonly) unsigned short unsignedShortValue;
@property(readonly) int intValue;
@property(readonly) unsigned int unsignedIntValue;
@property(readonly) long longValue;
@property(readonly) unsigned long unsignedLongValue;
@property(readonly) long long longLongValue;
@property(readonly) unsigned long long unsignedLongLongValue;
@property(readonly) float floatValue;
@property(readonly) double doubleValue;
@property(readonly) BOOL boolValue;
@property(readonly) NSInteger integerValue;
@property(readonly) NSUInteger unsignedIntegerValue;
@property(readonly, copy) NSString* stringValue;
- (NSComparisonResult)compare:(NSNumber*)otherNumber;
- (BOOL)isEqualToNumber:(NSNumber*)number;
@end

// -- Collections -------------------------------------------------------------

@interface NSEnumerator<ObjectType> : NSObject <NSFastEnumeration>
- (nullable ObjectType)nextObject;
@property(readonly, copy) NSArray<ObjectType>* allObjects;
@end

@interface NSArray<__covariant ObjectType>
    : NSObject <NSCopying, NSMutableCopying, NSSecureCoding, NSFastEnumeration>
@property(readonly) NSUInteger count;
- (ObjectType)objectAtIndex:(NSUInteger)index;
- (instancetype)init NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithObjects:(const ObjectType _Nonnull[_Nullable])objects
                          count:(NSUInteger)cnt NS_DESIGNATED_INITIALIZER;
@end

@interface NSArray<ObjectType> (NSExtendedArray)
+ (instancetype)array;
+ (instancetype)arrayWithObject:(ObjectType)anObject;
+ (instancetype)arrayWithObjects:(const ObjectType _Nonnull[_Nonnull])objects
                           count:(NSUInteger)cnt;
+ (instancetype)arrayWithObjects:(ObjectType)firstObj, ... NS_REQUIRES_NIL_TERMINATION;
+ (instancetype)arrayWithArray:(NSArray<ObjectType>*)array;
- (instancetype)initWithArray:(NSArray<ObjectType>*)array;
- (instancetype)initWithObjects:(ObjectType)firstObj, ... NS_REQUIRES_NIL_TERMINATION;
- (BOOL)containsObject:(ObjectType)anObject;
- (NSUInteger)indexOfObject:(ObjectType)anObject;
- (ObjectType)objectAtIndexedSubscript:(NSUInteger)idx;
@property(nullable, nonatomic, readonly) ObjectType firstObject;
@property(nullable, nonatomic, readonly) ObjectType lastObject;
- (NSArray<ObjectType>*)arrayByAddingObject:(ObjectType)anObject;
- (NSArray<ObjectType>*)arrayByAddingObjectsFromArray:
    (NSArray<ObjectType>*)otherArray;
- (NSString*)componentsJoinedByString:(NSString*)separator;
- (NSEnumerator<ObjectType>*)objectEnumerator;
- (NSEnumerator<ObjectType>*)reverseObjectEnumerator;
- (NSArray<ObjectType>*)subarrayWithRange:(NSRange)range;
- (void)enumerateObjectsUsingBlock:
    (void(NS_NOESCAPE ^)(ObjectType obj, NSUInteger idx, BOOL* stop))block;
- (BOOL)isEqualToArray:(NSArray<ObjectType>*)otherArray;
@end

@interface NSMutableArray<ObjectType> : NSArray <ObjectType>
- (void)addObject:(ObjectType)anObject;
- (void)insertObject:(ObjectType)anObject atIndex:(NSUInteger)index;
- (void)removeLastObject;
- (void)removeObjectAtIndex:(NSUInteger)index;
- (void)replaceObjectAtIndex:(NSUInteger)index withObject:(ObjectType)anObject;
- (instancetype)initWithCapacity:(NSUInteger)numItems NS_DESIGNATED_INITIALIZER;
+ (instancetype)arrayWithCapacity:(NSUInteger)numItems;
- (void)addObjectsFromArray:(NSArray<ObjectType>*)otherArray;
- (void)removeAllObjects;
- (void)removeObject:(ObjectType)anObject;
- (void)setObject:(ObjectType)obj atIndexedSubscript:(NSUInteger)idx;
@end

@interface NSDictionary<__covariant KeyType, __covariant ObjectType>
    : NSObject <NSCopying, NSMutableCopying, NSSecureCoding, NSFastEnumeration>
@property(readonly) NSUInteger count;
- (nullable ObjectType)objectForKey:(KeyType)aKey;
- (NSEnumerator<KeyType>*)keyEnumerator;
- (instancetype)init NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithObjects:(const ObjectType _Nonnull[_Nullable])objects
                        forKeys:(const KeyType<NSCopying> _Nonnull[_Nullable])keys
                          count:(NSUInteger)cnt NS_DESIGNATED_INITIALIZER;
@end

@interface NSDictionary<KeyType, ObjectType> (NSExtendedDictionary)
+ (instancetype)dictionary;
+ (instancetype)dictionaryWithObject:(ObjectType)object
                              forKey:(KeyType<NSCopying>)key;
+ (instancetype)dictionaryWithObjects:(const ObjectType _Nonnull[_Nullable])objects
                              forKeys:(const KeyType<NSCopying> _Nonnull[_Nullable])keys
                                count:(NSUInteger)cnt;
+ (instancetype)dictionaryWithObjectsAndKeys:(id)firstObject, ... NS_REQUIRES_NIL_TERMINATION;
+ (instancetype)dictionaryWithDictionary:(NSDictionary<KeyType, ObjectType>*)dict;
- (instancetype)initWithDictionary:(NSDictionary<KeyType, ObjectType>*)otherDictionary;
@property(readonly, copy) NSArray<KeyType>* allKeys;
@property(readonly, copy) NSArray<ObjectType>* allValues;
- (nullable ObjectType)objectForKeyedSubscript:(KeyType)key;
- (NSEnumerator<ObjectType>*)objectEnumerator;
- (void)enumerateKeysAndObjectsUsingBlock:
    (void(NS_NOESCAPE ^)(KeyType key, ObjectType obj, BOOL* stop))block;
- (BOOL)isEqualToDictionary:(NSDictionary<KeyType, ObjectType>*)otherDictionary;
@end

@interface NSMutableDictionary<KeyType, ObjectType>
    : NSDictionary <KeyType, ObjectType>
- (void)removeObjectForKey:(KeyType)aKey;
- (void)setObject:(ObjectType)anObject forKey:(KeyType<NSCopying>)aKey;
- (instancetype)initWithCapacity:(NSUInteger)numItems NS_DESIGNATED_INITIALIZER;
+ (instancetype)dictionaryWithCapacity:(NSUInteger)numItems;
- (void)setObject:(nullable ObjectType)obj
    forKeyedSubscript:(KeyType<NSCopying>)key;
- (void)removeAllObjects;
- (void)addEntriesFromDictionary:(NSDictionary<KeyType, ObjectType>*)otherDictionary;
@end

@interface NSSet<__covariant ObjectType>
    : NSObject <NSCopying, NSMutableCopying, NSSecureCoding, NSFastEnumeration>
@property(readonly) NSUInteger count;
- (nullable ObjectType)member:(ObjectType)object;
- (NSEnumerator<ObjectType>*)objectEnumerator;
+ (instancetype)set;
+ (instancetype)setWithObject:(ObjectType)object;
+ (instancetype)setWithArray:(NSArray<ObjectType>*)array;
+ (instancetype)setWithObjects:(ObjectType)firstObj, ... NS_REQUIRES_NIL_TERMINATION;
- (BOOL)containsObject:(ObjectType)anObject;
@property(readonly, copy) NSArray<ObjectType>* allObjects;
@end

@interface NSMutableSet<ObjectType> : NSSet <ObjectType>
- (void)addObject:(ObjectType)object;
- (void)removeObject:(ObjectType)object;
- (void)removeAllObjects;
+ (instancetype)setWithCapacity:(NSUInteger)numItems;
@end

@interface NSIndexSet : NSObject <NSCopying, NSMutableCopying, NSSecureCoding>
+ (instancetype)indexSetWithIndex:(NSUInteger)value;
@property(readonly) NSUInteger count;
@end

@interface NSNull : NSObject <NSCopying, NSSecureCoding>
+ (NSNull*)null;
@end

// -- NSData ------------------------------------------------------------------

@interface NSData : NSObject <NSCopying, NSMutableCopying, NSSecureCoding>
@property(readonly) NSUInteger length;
@property(readonly) const void* bytes NS_RETURNS_INNER_POINTER;
+ (instancetype)data;
+ (instancetype)dataWithBytes:(nullable const void*)bytes
                       length:(NSUInteger)length;
+ (instancetype)dataWithBytesNoCopy:(void*)bytes
                             length:(NSUInteger)length
                       freeWhenDone:(BOOL)b;
+ (nullable instancetype)dataWithContentsOfFile:(NSString*)path;
- (instancetype)initWithBytes:(nullable const void*)bytes
                       length:(NSUInteger)length;
- (instancetype)initWithBytesNoCopy:(void*)bytes
                             length:(NSUInteger)length
                       freeWhenDone:(BOOL)b;
- (void)getBytes:(void*)buffer length:(NSUInteger)length;
- (BOOL)isEqualToData:(NSData*)other;
@end

@interface NSMutableData : NSData
@property(readonly) void* mutableBytes NS_RETURNS_INNER_POINTER;
@property NSUInteger length;
+ (nullable instancetype)dataWithLength:(NSUInteger)length;
- (void)appendBytes:(const void*)bytes length:(NSUInteger)length;
- (void)appendData:(NSData*)other;
@end

// -- NSDate, NSLocale, NSCalendar, NSTimeZone, NSCharacterSet, NSUUID --------

@interface NSDate : NSObject <NSCopying, NSSecureCoding>
@property(readonly) NSTimeInterval timeIntervalSinceReferenceDate;
+ (instancetype)date;
+ (instancetype)dateWithTimeIntervalSinceNow:(NSTimeInterval)secs;
+ (instancetype)dateWithTimeIntervalSinceReferenceDate:(NSTimeInterval)ti;
+ (instancetype)dateWithTimeIntervalSince1970:(NSTimeInterval)secs;
+ (NSDate*)distantFuture;
+ (NSDate*)distantPast;
@property(readonly) NSTimeInterval timeIntervalSince1970;
@property(readonly) NSTimeInterval timeIntervalSinceNow;
- (NSTimeInterval)timeIntervalSinceDate:(NSDate*)anotherDate;
@end

@interface NSLocale : NSObject <NSCopying, NSSecureCoding>
@property(class, readonly, copy) NSLocale* currentLocale;
@property(class, readonly, copy) NSArray<NSString*>* preferredLanguages;
@property(readonly, copy) NSString* localeIdentifier;
@property(readonly, copy) NSString* languageCode;
@property(nullable, readonly, copy) NSString* countryCode;
- (instancetype)initWithLocaleIdentifier:(NSString*)string;
+ (instancetype)localeWithLocaleIdentifier:(NSString*)ident;
@end

@interface NSCalendar : NSObject <NSCopying, NSSecureCoding>
@property(class, readonly, copy) NSCalendar* currentCalendar;
@end

@interface NSTimeZone : NSObject <NSCopying, NSSecureCoding>
@property(class, readonly, copy) NSTimeZone* localTimeZone;
@property(class, readonly, copy) NSTimeZone* systemTimeZone;
@property(readonly, copy) NSString* name;
@end

@interface NSCharacterSet : NSObject <NSCopying, NSMutableCopying, NSSecureCoding>
@property(readonly, class, copy) NSCharacterSet* whitespaceCharacterSet;
@property(readonly, class, copy) NSCharacterSet* whitespaceAndNewlineCharacterSet;
+ (NSCharacterSet*)characterSetWithCharactersInString:(NSString*)aString;
- (BOOL)characterIsMember:(unichar)aCharacter;
- (BOOL)longCharacterIsMember:(UTF32Char)theLongChar;
@end

@interface NSMutableCharacterSet : NSCharacterSet
- (void)addCharactersInString:(NSString*)aString;
@end

@interface NSUUID : NSObject <NSCopying, NSSecureCoding>
+ (instancetype)UUID;
@property(readonly, copy) NSString* UUIDString;
@end

// -- NSAttributedString ------------------------------------------------------

@interface NSAttributedString : NSObject <NSCopying, NSMutableCopying, NSSecureCoding>
@property(readonly, copy) NSString* string;
@property(readonly) NSUInteger length;
- (instancetype)initWithString:(NSString*)str;
- (instancetype)initWithString:(NSString*)str
                    attributes:(nullable NSDictionary<NSAttributedStringKey, id>*)attrs;
- (NSDictionary<NSAttributedStringKey, id>*)attributesAtIndex:(NSUInteger)location
                                               effectiveRange:(nullable NSRangePointer)range;
@end

@interface NSMutableAttributedString : NSAttributedString
- (void)addAttribute:(NSAttributedStringKey)name
               value:(id)value
               range:(NSRange)range;
- (void)appendAttributedString:(NSAttributedString*)attrString;
@end

// -- NSError, NSBundle, NSURL ------------------------------------------------

@interface NSError : NSObject <NSCopying, NSSecureCoding>
@property(readonly, copy) NSErrorDomain domain;
@property(readonly) NSInteger code;
@property(readonly, copy) NSDictionary<NSErrorUserInfoKey, id>* userInfo;
@property(readonly, copy) NSString* localizedDescription;
+ (instancetype)errorWithDomain:(NSErrorDomain)domain
                           code:(NSInteger)code
                       userInfo:(nullable NSDictionary<NSErrorUserInfoKey, id>*)dict;
@end
FOUNDATION_EXPORT NSErrorDomain const NSCocoaErrorDomain;
FOUNDATION_EXPORT NSErrorDomain const NSPOSIXErrorDomain;
FOUNDATION_EXPORT NSErrorDomain const NSOSStatusErrorDomain;
FOUNDATION_EXPORT NSErrorUserInfoKey const NSLocalizedDescriptionKey;

@interface NSURL : NSObject <NSSecureCoding, NSCopying>
+ (nullable instancetype)URLWithString:(NSString*)URLString;
+ (NSURL*)fileURLWithPath:(NSString*)path;
+ (NSURL*)fileURLWithPath:(NSString*)path isDirectory:(BOOL)isDir;
- (nullable instancetype)initWithString:(NSString*)URLString;
- (instancetype)initFileURLWithPath:(NSString*)path;
@property(nullable, readonly, copy) NSString* absoluteString;
@property(nullable, readonly, copy) NSString* path;
@property(nullable, readonly, copy) NSString* scheme;
@property(nullable, readonly, copy) NSString* host;
@property(readonly, getter=isFileURL) BOOL fileURL;
@property(readonly) const char* fileSystemRepresentation NS_RETURNS_INNER_POINTER;
- (NSURL*)URLByAppendingPathComponent:(NSString*)pathComponent;
@end

@interface NSBundle : NSObject
@property(class, readonly, strong) NSBundle* mainBundle;
+ (nullable instancetype)bundleWithPath:(NSString*)path;
+ (nullable instancetype)bundleWithIdentifier:(NSString*)identifier;
@property(readonly, copy) NSString* bundlePath;
@property(readonly, copy) NSURL* bundleURL;
@property(nullable, readonly, copy) NSString* bundleIdentifier;
@property(nullable, readonly, copy) NSDictionary<NSString*, id>* infoDictionary;
@property(nullable, readonly, copy) NSString* resourcePath;
@property(nullable, readonly, copy) NSString* executablePath;
- (nullable id)objectForInfoDictionaryKey:(NSString*)key;
- (nullable NSString*)pathForResource:(nullable NSString*)name
                               ofType:(nullable NSString*)ext;
@end

// -- NSTimer, NSStream, NSRunLoop, NSNotificationCenter ----------------------

@interface NSTimer : NSObject
+ (NSTimer*)scheduledTimerWithTimeInterval:(NSTimeInterval)ti
                                    target:(id)aTarget
                                  selector:(SEL)aSelector
                                  userInfo:(nullable id)userInfo
                                   repeats:(BOOL)yesOrNo;
- (void)invalidate;
@property(readonly, getter=isValid) BOOL valid;
@end

@interface NSStream : NSObject
@end
@interface NSInputStream : NSStream
@end
@interface NSOutputStream : NSStream
@end

@interface NSRunLoop : NSObject
@property(class, readonly, strong) NSRunLoop* currentRunLoop;
@property(class, readonly, strong) NSRunLoop* mainRunLoop;
@end
FOUNDATION_EXPORT NSRunLoopMode const NSDefaultRunLoopMode;
FOUNDATION_EXPORT NSRunLoopMode const NSRunLoopCommonModes;

@interface NSNotification : NSObject <NSCopying, NSCoding>
@property(readonly, copy) NSNotificationName name;
@property(nullable, readonly, retain) id object;
@property(nullable, readonly, copy) NSDictionary* userInfo;
@end

@interface NSNotificationCenter : NSObject
@property(class, readonly, strong) NSNotificationCenter* defaultCenter;
- (void)addObserver:(id)observer
           selector:(SEL)aSelector
               name:(nullable NSNotificationName)aName
             object:(nullable id)anObject;
- (void)removeObserver:(id)observer;
- (void)removeObserver:(id)observer
                  name:(nullable NSNotificationName)aName
                object:(nullable id)anObject;
- (void)postNotificationName:(NSNotificationName)aName
                      object:(nullable id)anObject;
- (id<NSObject>)addObserverForName:(nullable NSNotificationName)name
                            object:(nullable id)obj
                             queue:(nullable NSOperationQueue*)queue
                        usingBlock:(void (^)(NSNotification* notification))block;
@end

@interface NSProcessInfo : NSObject
@property(class, readonly, strong) NSProcessInfo* processInfo;
@property(readonly, copy) NSDictionary<NSString*, NSString*>* environment;
@property(readonly, copy) NSArray<NSString*>* arguments;
@property(readonly) int processIdentifier;
@property(readonly) unsigned long long physicalMemory;
@end

// -- NSPathUtilities.h -------------------------------------------------------

typedef NS_ENUM(NSUInteger, NSSearchPathDirectory){
    NSApplicationDirectory = 1,
    NSDemoApplicationDirectory,
    NSDeveloperApplicationDirectory,
    NSAdminApplicationDirectory,
    NSLibraryDirectory,
    NSDeveloperDirectory,
    NSUserDirectory,
    NSDocumentationDirectory,
    NSDocumentDirectory,
    NSCoreServiceDirectory,
    NSAutosavedInformationDirectory = 11,
    NSDesktopDirectory = 12,
    NSCachesDirectory = 13,
    NSApplicationSupportDirectory = 14,
    NSDownloadsDirectory = 15,
    NSInputMethodsDirectory = 16,
    NSMoviesDirectory = 17,
    NSMusicDirectory = 18,
    NSPicturesDirectory = 19,
    NSPrinterDescriptionDirectory = 20,
    NSSharedPublicDirectory = 21,
    NSPreferencePanesDirectory = 22,
    NSApplicationScriptsDirectory = 23,
    NSItemReplacementDirectory = 99,
    NSAllApplicationsDirectory = 100,
    NSAllLibrariesDirectory = 101,
    NSTrashDirectory = 102};

typedef NS_OPTIONS(NSUInteger, NSSearchPathDomainMask){
    NSUserDomainMask = 1, NSLocalDomainMask = 2, NSNetworkDomainMask = 4,
    NSSystemDomainMask = 8, NSAllDomainsMask = 0x0ffff};

FOUNDATION_EXPORT NSArray<NSString*>* NSSearchPathForDirectoriesInDomains(
    NSSearchPathDirectory directory,
    NSSearchPathDomainMask domainMask,
    BOOL expandTilde);
FOUNDATION_EXPORT NSString* NSHomeDirectory(void);
FOUNDATION_EXPORT NSString* NSTemporaryDirectory(void);

@interface NSFileManager : NSObject
@property(class, readonly, strong) NSFileManager* defaultManager;
- (BOOL)fileExistsAtPath:(NSString*)path;
@end

@interface NSUserDefaults : NSObject
@property(class, readonly, strong) NSUserDefaults* standardUserDefaults;
- (nullable id)objectForKey:(NSString*)defaultName;
- (BOOL)boolForKey:(NSString*)defaultName;
- (void)setObject:(nullable id)value forKey:(NSString*)defaultName;
- (void)setBool:(BOOL)value forKey:(NSString*)defaultName;
@end

@interface NSThread : NSObject
@property(class, readonly) BOOL isMainThread;
@end

@interface NSAutoreleasePool : NSObject
@end

#endif  // __OBJC__

#endif  // COMPILE_CHECK_STAND_IN_FOUNDATION_H_
