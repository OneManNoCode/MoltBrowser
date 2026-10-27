// compile_check STAND-IN for the macOS SDK's CoreText framework (see
// ../README.md): the declarations Chromium's Mac code and the checked
// files use, with the SDK's names, types and signatures.

#ifndef COMPILE_CHECK_STAND_IN_CORETEXT_H_
#define COMPILE_CHECK_STAND_IN_CORETEXT_H_

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#define CT_EXPORT extern __attribute__((visibility("default")))
#define CT_AVAILABLE(...)
#define CT_DEPRECATED(...)
#define CT_ENUM_AVAILABLE(...)

CF_EXTERN_C_BEGIN

// -- CTFontDescriptor.h ------------------------------------------------------

typedef const struct CF_BRIDGED_TYPE(NSFontDescriptor) __CTFontDescriptor*
    CTFontDescriptorRef;
CT_EXPORT CFTypeID CTFontDescriptorGetTypeID(void);

CT_EXPORT const CFStringRef kCTFontURLAttribute;
CT_EXPORT const CFStringRef kCTFontNameAttribute;
CT_EXPORT const CFStringRef kCTFontDisplayNameAttribute;
CT_EXPORT const CFStringRef kCTFontFamilyNameAttribute;
CT_EXPORT const CFStringRef kCTFontStyleNameAttribute;
CT_EXPORT const CFStringRef kCTFontTraitsAttribute;
CT_EXPORT const CFStringRef kCTFontVariationAttribute;
CT_EXPORT const CFStringRef kCTFontSizeAttribute;
CT_EXPORT const CFStringRef kCTFontMatrixAttribute;
CT_EXPORT const CFStringRef kCTFontCascadeListAttribute;
CT_EXPORT const CFStringRef kCTFontCharacterSetAttribute;
CT_EXPORT const CFStringRef kCTFontLanguagesAttribute;
CT_EXPORT const CFStringRef kCTFontFeaturesAttribute;
CT_EXPORT const CFStringRef kCTFontFeatureSettingsAttribute;
CT_EXPORT const CFStringRef kCTFontFixedAdvanceAttribute;
CT_EXPORT const CFStringRef kCTFontOrientationAttribute;
CT_EXPORT const CFStringRef kCTFontFormatAttribute;
CT_EXPORT const CFStringRef kCTFontPriorityAttribute;
CT_EXPORT const CFStringRef kCTFontEnabledAttribute;

CT_EXPORT CTFontDescriptorRef CTFontDescriptorCreateWithNameAndSize(
    CFStringRef name,
    CGFloat size);
CT_EXPORT CTFontDescriptorRef CTFontDescriptorCreateWithAttributes(
    CFDictionaryRef attributes);
CT_EXPORT CTFontDescriptorRef CTFontDescriptorCreateCopyWithAttributes(
    CTFontDescriptorRef original,
    CFDictionaryRef attributes);
CT_EXPORT CTFontDescriptorRef CTFontDescriptorCreateCopyWithFamily(
    CTFontDescriptorRef original,
    CFStringRef family);
CT_EXPORT CTFontDescriptorRef CTFontDescriptorCreateMatchingFontDescriptor(
    CTFontDescriptorRef descriptor,
    CFSetRef mandatoryAttributes);
CT_EXPORT CFArrayRef CTFontDescriptorCreateMatchingFontDescriptors(
    CTFontDescriptorRef descriptor,
    CFSetRef mandatoryAttributes);
CT_EXPORT CFDictionaryRef CTFontDescriptorCopyAttributes(
    CTFontDescriptorRef descriptor);
CT_EXPORT CFTypeRef CTFontDescriptorCopyAttribute(
    CTFontDescriptorRef descriptor,
    CFStringRef attribute);

// -- CTFontTraits.h ----------------------------------------------------------

CT_EXPORT const CFStringRef kCTFontSymbolicTrait;
CT_EXPORT const CFStringRef kCTFontWeightTrait;
CT_EXPORT const CFStringRef kCTFontWidthTrait;
CT_EXPORT const CFStringRef kCTFontSlantTrait;

typedef CF_OPTIONS(uint32_t, CTFontSymbolicTraits){
    kCTFontTraitItalic = (1 << 0),
    kCTFontTraitBold = (1 << 1),
    kCTFontTraitExpanded = (1 << 5),
    kCTFontTraitCondensed = (1 << 6),
    kCTFontTraitMonoSpace = (1 << 10),
    kCTFontTraitVertical = (1 << 11),
    kCTFontTraitUIOptimized = (1 << 12),
    kCTFontTraitColorGlyphs = (1 << 13),
    kCTFontTraitComposite = (1 << 14),
    kCTFontTraitClassMask = (15U << 28),
    kCTFontItalicTrait = kCTFontTraitItalic,
    kCTFontBoldTrait = kCTFontTraitBold,
    kCTFontExpandedTrait = kCTFontTraitExpanded,
    kCTFontCondensedTrait = kCTFontTraitCondensed,
    kCTFontMonoSpaceTrait = kCTFontTraitMonoSpace,
    kCTFontVerticalTrait = kCTFontTraitVertical,
    kCTFontUIOptimizedTrait = kCTFontTraitUIOptimized,
    kCTFontColorGlyphsTrait = kCTFontTraitColorGlyphs,
    kCTFontCompositeTrait = kCTFontTraitComposite,
    kCTFontClassMaskTrait = kCTFontTraitClassMask};

// -- CTFont.h ----------------------------------------------------------------

typedef const struct CF_BRIDGED_TYPE(NSFont) __CTFont* CTFontRef;
CT_EXPORT CFTypeID CTFontGetTypeID(void);

CT_EXPORT const CFStringRef kCTFontCopyrightNameKey;
CT_EXPORT const CFStringRef kCTFontFamilyNameKey;
CT_EXPORT const CFStringRef kCTFontSubFamilyNameKey;
CT_EXPORT const CFStringRef kCTFontStyleNameKey;
CT_EXPORT const CFStringRef kCTFontUniqueNameKey;
CT_EXPORT const CFStringRef kCTFontFullNameKey;
CT_EXPORT const CFStringRef kCTFontVersionNameKey;
CT_EXPORT const CFStringRef kCTFontPostScriptNameKey;

CT_EXPORT const CFStringRef kCTFontVariationAxisIdentifierKey;
CT_EXPORT const CFStringRef kCTFontVariationAxisMinimumValueKey;
CT_EXPORT const CFStringRef kCTFontVariationAxisMaximumValueKey;
CT_EXPORT const CFStringRef kCTFontVariationAxisDefaultValueKey;
CT_EXPORT const CFStringRef kCTFontVariationAxisNameKey;

typedef CF_ENUM(uint32_t, CTFontUIFontType){
    kCTFontUIFontNone = (uint32_t)-1, kCTFontUIFontUser = 0,
    kCTFontUIFontUserFixedPitch = 1,  kCTFontUIFontSystem = 2,
    kCTFontUIFontEmphasizedSystem = 3, kCTFontUIFontSmallSystem = 4,
    kCTFontUIFontApplication = 9,     kCTFontUIFontLabel = 10,
    kCTFontUIFontMenuItem = 12,       kCTFontUIFontMessage = 23,
    kCTFontUIFontToolTip = 25};

typedef CF_OPTIONS(CFOptionFlags, CTFontOptions){
    kCTFontOptionsDefault = 0,
    kCTFontOptionsPreventAutoActivation = 1 << 0,
    kCTFontOptionsPreventAutoDownload = 1 << 1,
    kCTFontOptionsPreferSystemFont = 1 << 2};

typedef CF_ENUM(uint32_t, CTFontOrientation){
    kCTFontOrientationDefault = 0, kCTFontOrientationHorizontal = 1,
    kCTFontOrientationVertical = 2};

CT_EXPORT CTFontRef CTFontCreateWithName(CFStringRef name,
                                         CGFloat size,
                                         const CGAffineTransform* matrix);
CT_EXPORT CTFontRef CTFontCreateWithFontDescriptor(
    CTFontDescriptorRef descriptor,
    CGFloat size,
    const CGAffineTransform* matrix);
CT_EXPORT CTFontRef CTFontCreateWithNameAndOptions(
    CFStringRef name,
    CGFloat size,
    const CGAffineTransform* matrix,
    CTFontOptions options);
CT_EXPORT CTFontRef CTFontCreateUIFontForLanguage(CTFontUIFontType uiType,
                                                  CGFloat size,
                                                  CFStringRef language);
CT_EXPORT CTFontRef CTFontCreateCopyWithAttributes(
    CTFontRef font,
    CGFloat size,
    const CGAffineTransform* matrix,
    CTFontDescriptorRef attributes);
CT_EXPORT CTFontRef CTFontCreateCopyWithSymbolicTraits(
    CTFontRef font,
    CGFloat size,
    const CGAffineTransform* matrix,
    CTFontSymbolicTraits symTraitValue,
    CTFontSymbolicTraits symTraitMask);
CT_EXPORT CTFontRef CTFontCreateCopyWithFamily(CTFontRef font,
                                               CGFloat size,
                                               const CGAffineTransform* matrix,
                                               CFStringRef family);
CT_EXPORT CTFontRef CTFontCreateForString(CTFontRef currentFont,
                                          CFStringRef string,
                                          CFRange range);
CT_EXPORT CTFontRef CTFontCreateForStringWithLanguage(CTFontRef currentFont,
                                                      CFStringRef string,
                                                      CFRange range,
                                                      CFStringRef language);
CT_EXPORT CTFontRef CTFontCreateWithGraphicsFont(
    CGFontRef graphicsFont,
    CGFloat size,
    const CGAffineTransform* matrix,
    CTFontDescriptorRef attributes);
CT_EXPORT CTFontDescriptorRef CTFontCopyFontDescriptor(CTFontRef font);
CT_EXPORT CFTypeRef CTFontCopyAttribute(CTFontRef font,
                                        CFStringRef attribute);
CT_EXPORT CGFloat CTFontGetSize(CTFontRef font);
CT_EXPORT CGAffineTransform CTFontGetMatrix(CTFontRef font);
CT_EXPORT CTFontSymbolicTraits CTFontGetSymbolicTraits(CTFontRef font);
CT_EXPORT CFDictionaryRef CTFontCopyTraits(CTFontRef font);
CT_EXPORT CFArrayRef CTFontCopyDefaultCascadeListForLanguages(
    CTFontRef font,
    CFArrayRef languagePrefList);
CT_EXPORT CFStringRef CTFontCopyPostScriptName(CTFontRef font);
CT_EXPORT CFStringRef CTFontCopyFamilyName(CTFontRef font);
CT_EXPORT CFStringRef CTFontCopyFullName(CTFontRef font);
CT_EXPORT CFStringRef CTFontCopyDisplayName(CTFontRef font);
CT_EXPORT CFStringRef CTFontCopyName(CTFontRef font, CFStringRef nameKey);
CT_EXPORT CFStringRef CTFontCopyLocalizedName(CTFontRef font,
                                              CFStringRef nameKey,
                                              CFStringRef* actualLanguage);
CT_EXPORT CFCharacterSetRef CTFontCopyCharacterSet(CTFontRef font);
CT_EXPORT CFStringEncoding CTFontGetStringEncoding(CTFontRef font);
CT_EXPORT CFArrayRef CTFontCopySupportedLanguages(CTFontRef font);
CT_EXPORT bool CTFontGetGlyphsForCharacters(CTFontRef font,
                                            const UniChar characters[],
                                            CGGlyph glyphs[],
                                            CFIndex count);
CT_EXPORT CGFloat CTFontGetAscent(CTFontRef font);
CT_EXPORT CGFloat CTFontGetDescent(CTFontRef font);
CT_EXPORT CGFloat CTFontGetLeading(CTFontRef font);
CT_EXPORT unsigned CTFontGetUnitsPerEm(CTFontRef font);
CT_EXPORT CFIndex CTFontGetGlyphCount(CTFontRef font);
CT_EXPORT CGRect CTFontGetBoundingBox(CTFontRef font);
CT_EXPORT CGFloat CTFontGetUnderlinePosition(CTFontRef font);
CT_EXPORT CGFloat CTFontGetUnderlineThickness(CTFontRef font);
CT_EXPORT CGFloat CTFontGetSlantAngle(CTFontRef font);
CT_EXPORT CGFloat CTFontGetCapHeight(CTFontRef font);
CT_EXPORT CGFloat CTFontGetXHeight(CTFontRef font);
CT_EXPORT CGGlyph CTFontGetGlyphWithName(CTFontRef font,
                                         CFStringRef glyphName);
CT_EXPORT CGRect CTFontGetBoundingRectsForGlyphs(CTFontRef font,
                                                 CTFontOrientation orientation,
                                                 const CGGlyph glyphs[],
                                                 CGRect boundingRects[],
                                                 CFIndex count);
CT_EXPORT double CTFontGetAdvancesForGlyphs(CTFontRef font,
                                            CTFontOrientation orientation,
                                            const CGGlyph glyphs[],
                                            CGSize advances[],
                                            CFIndex count);
CT_EXPORT CGPathRef CTFontCreatePathForGlyph(CTFontRef font,
                                             CGGlyph glyph,
                                             const CGAffineTransform* matrix);
CT_EXPORT CFArrayRef CTFontCopyVariationAxes(CTFontRef font);
CT_EXPORT CFDictionaryRef CTFontCopyVariation(CTFontRef font);
CT_EXPORT CFArrayRef CTFontCopyFeatures(CTFontRef font);
CT_EXPORT CGFontRef CTFontCopyGraphicsFont(CTFontRef font,
                                           CTFontDescriptorRef* attributes);
CT_EXPORT CFArrayRef CTFontCopyAvailableTables(CTFontRef font,
                                               uint32_t options);
CT_EXPORT CFDataRef CTFontCopyTable(CTFontRef font,
                                    uint32_t table,
                                    uint32_t options);

// -- CTFontManager.h, CTFontCollection.h -------------------------------------

typedef const struct CF_BRIDGED_TYPE(id) __CTFontCollection*
    CTFontCollectionRef;
CT_EXPORT CTFontCollectionRef CTFontCollectionCreateFromAvailableFonts(
    CFDictionaryRef options);
CT_EXPORT CFArrayRef CTFontCollectionCreateMatchingFontDescriptors(
    CTFontCollectionRef collection);
CT_EXPORT CFArrayRef CTFontManagerCopyAvailableFontFamilyNames(void);
CT_EXPORT CFArrayRef CTFontManagerCopyAvailablePostScriptNames(void);
CT_EXPORT CFArrayRef CTFontManagerCreateFontDescriptorsFromData(CFDataRef data);
CT_EXPORT CTFontDescriptorRef CTFontManagerCreateFontDescriptorFromData(
    CFDataRef data);

// -- CTFontDescriptor weights (CTFontTraits.h / CoreText's NSFont weights) ---

CT_EXPORT const CGFloat kCTFontWeightUltraLight;
CT_EXPORT const CGFloat kCTFontWeightThin;
CT_EXPORT const CGFloat kCTFontWeightLight;
CT_EXPORT const CGFloat kCTFontWeightRegular;
CT_EXPORT const CGFloat kCTFontWeightMedium;
CT_EXPORT const CGFloat kCTFontWeightSemibold;
CT_EXPORT const CGFloat kCTFontWeightBold;
CT_EXPORT const CGFloat kCTFontWeightHeavy;
CT_EXPORT const CGFloat kCTFontWeightBlack;

// -- CTRun, CTLine (the types base/apple names) ------------------------------

typedef const struct CF_BRIDGED_TYPE(id) __CTRun* CTRunRef;
CT_EXPORT CFTypeID CTRunGetTypeID(void);
typedef const struct CF_BRIDGED_TYPE(id) __CTLine* CTLineRef;
CT_EXPORT CFTypeID CTLineGetTypeID(void);
CT_EXPORT CTLineRef CTLineCreateWithAttributedString(
    CFAttributedStringRef attrString);
CT_EXPORT CFArrayRef CTLineGetGlyphRuns(CTLineRef line);
CT_EXPORT CFIndex CTRunGetGlyphCount(CTRunRef run);
CT_EXPORT CFDictionaryRef CTRunGetAttributes(CTRunRef run);
CT_EXPORT const CFStringRef kCTFontAttributeName;
CT_EXPORT const CFStringRef kCTForegroundColorAttributeName;

CF_EXTERN_C_END

#endif  // COMPILE_CHECK_STAND_IN_CORETEXT_H_
