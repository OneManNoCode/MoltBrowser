// compile_check STAND-IN for the macOS SDK's AppKit framework (see
// ../README.md): the declarations Chromium's Mac code and the checked
// files use, with the SDK's names, types and signatures.

#ifndef COMPILE_CHECK_STAND_IN_APPKIT_H_
#define COMPILE_CHECK_STAND_IN_APPKIT_H_

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#define APPKIT_EXTERN FOUNDATION_EXTERN
#define APPKIT_API_UNAVAILABLE_BEGIN_MACCATALYST
#define APPKIT_API_UNAVAILABLE_END

typedef CGFloat NSFontWeight;
typedef CGFloat NSFontWidth;
typedef CGPoint NSPoint;
typedef CGSize NSSize;
typedef CGRect NSRect;

#if defined(__OBJC__)

APPKIT_EXTERN const NSFontWeight NSFontWeightUltraLight;
APPKIT_EXTERN const NSFontWeight NSFontWeightThin;
APPKIT_EXTERN const NSFontWeight NSFontWeightLight;
APPKIT_EXTERN const NSFontWeight NSFontWeightRegular;
APPKIT_EXTERN const NSFontWeight NSFontWeightMedium;
APPKIT_EXTERN const NSFontWeight NSFontWeightSemibold;
APPKIT_EXTERN const NSFontWeight NSFontWeightBold;
APPKIT_EXTERN const NSFontWeight NSFontWeightHeavy;
APPKIT_EXTERN const NSFontWeight NSFontWeightBlack;

@class NSFontDescriptor;

@interface NSFont : NSObject <NSCopying, NSSecureCoding>
+ (nullable NSFont*)fontWithName:(NSString*)fontName size:(CGFloat)fontSize;
+ (NSFont*)systemFontOfSize:(CGFloat)fontSize;
+ (NSFont*)systemFontOfSize:(CGFloat)fontSize weight:(NSFontWeight)weight;
+ (NSFont*)boldSystemFontOfSize:(CGFloat)fontSize;
+ (NSFont*)monospacedSystemFontOfSize:(CGFloat)fontSize
                               weight:(NSFontWeight)weight;
+ (CGFloat)systemFontSize;
+ (CGFloat)smallSystemFontSize;
@property(readonly, copy) NSString* fontName;
@property(readonly) CGFloat pointSize;
@property(nullable, readonly, copy) NSString* familyName;
@property(nullable, readonly, copy) NSString* displayName;
@property(readonly, strong) NSFontDescriptor* fontDescriptor;
@property(readonly) CGFloat ascender;
@property(readonly) CGFloat descender;
@property(readonly) CGFloat capHeight;
@property(readonly) CGFloat xHeight;
@property(readonly) CGFloat leading;
@property(readonly, getter=isFixedPitch) BOOL fixedPitch;
@end

@interface NSFontDescriptor : NSObject <NSCopying, NSSecureCoding>
@property(readonly, copy) NSDictionary<NSString*, id>* fontAttributes;
@end

typedef NSUInteger NSFontTraitMask;
enum {
  NSItalicFontMask = 0x00000001,
  NSBoldFontMask = 0x00000002,
  NSUnboldFontMask = 0x00000004,
  NSNonStandardCharacterSetFontMask = 0x00000008,
  NSNarrowFontMask = 0x00000010,
  NSExpandedFontMask = 0x00000020,
  NSCondensedFontMask = 0x00000040,
  NSSmallCapsFontMask = 0x00000080,
  NSPosterFontMask = 0x00000100,
  NSCompressedFontMask = 0x00000200,
  NSFixedPitchFontMask = 0x00000400,
  NSUnitalicFontMask = 0x01000000,
};

@interface NSFontManager : NSObject
@property(class, readonly, strong) NSFontManager* sharedFontManager;
@property(readonly, copy) NSArray<NSString*>* availableFonts;
@property(readonly, copy) NSArray<NSString*>* availableFontFamilies;
- (nullable NSArray<NSArray*>*)availableMembersOfFontFamily:(NSString*)fam;
- (nullable NSFont*)fontWithFamily:(NSString*)family
                            traits:(NSFontTraitMask)traits
                            weight:(NSInteger)weight
                              size:(CGFloat)size;
- (NSFontTraitMask)traitsOfFont:(NSFont*)fontObj;
- (NSInteger)weightOfFont:(NSFont*)fontObj;
@end

#endif  // __OBJC__

#endif  // COMPILE_CHECK_STAND_IN_APPKIT_H_
