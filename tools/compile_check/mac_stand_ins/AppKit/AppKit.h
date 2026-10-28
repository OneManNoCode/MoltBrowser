// compile_check STAND-IN for the macOS SDK's AppKit framework (see
// ../README.md): the declarations Chromium's Mac code and the checked
// files use, with the SDK's names, types and signatures.
//
// Source: the SDK's AppKit.framework headers named in each section below,
// as Apple's documentation gives them (developer.apple.com/documentation/
// appkit). A section declares what the checked files and the Chromium
// headers they include use, not the whole SDK header; sections are in
// dependency order, not the SDK's file order.

#ifndef COMPILE_CHECK_STAND_IN_APPKIT_H_
#define COMPILE_CHECK_STAND_IN_APPKIT_H_

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <sys/types.h>

// -- AppKitDefines.h ---------------------------------------------------------

#define APPKIT_EXTERN FOUNDATION_EXTERN
#define APPKIT_API_UNAVAILABLE_BEGIN_MACCATALYST
#define APPKIT_API_UNAVAILABLE_END

typedef CGFloat NSFontWeight;
typedef CGFloat NSFontWidth;

#if defined(__OBJC__)

NS_ASSUME_NONNULL_BEGIN

@class NSException;

// -- NSFont.h, NSFontDescriptor.h, NSFontManager.h ---------------------------



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

// -- NSGraphics.h ------------------------------------------------------------

typedef NS_ENUM(NSInteger, NSWindowOrderingMode){
    NSWindowAbove = 1,
    NSWindowBelow = -1,
    NSWindowOut = 0,
};

typedef NS_ENUM(NSUInteger, NSFocusRingPlacement){
    NSFocusRingOnly = 0,
    NSFocusRingBelow = 1,
    NSFocusRingAbove = 2,
};

typedef NS_ENUM(NSUInteger, NSFocusRingType){
    NSFocusRingTypeDefault = 0,
    NSFocusRingTypeNone = 1,
    NSFocusRingTypeExterior = 2,
};

typedef NS_ENUM(NSUInteger, NSCompositingOperation){
    NSCompositingOperationClear = 0,
    NSCompositingOperationCopy = 1,
    NSCompositingOperationSourceOver = 2,
    NSCompositingOperationSourceIn = 3,
    NSCompositingOperationSourceOut = 4,
    NSCompositingOperationSourceAtop = 5,
    NSCompositingOperationDestinationOver = 6,
    NSCompositingOperationDestinationIn = 7,
    NSCompositingOperationDestinationOut = 8,
    NSCompositingOperationDestinationAtop = 9,
    NSCompositingOperationXOR = 10,
    NSCompositingOperationPlusDarker = 11,
    NSCompositingOperationPlusLighter = 13,
};

typedef NS_ENUM(NSInteger, NSColorRenderingIntent){
    NSColorRenderingIntentDefault,
    NSColorRenderingIntentAbsoluteColorimetric,
    NSColorRenderingIntentRelativeColorimetric,
    NSColorRenderingIntentPerceptual,
    NSColorRenderingIntentSaturation,
};

typedef NS_ENUM(NSInteger, NSDisplayGamut){
    NSDisplayGamutSRGB = 1,
    NSDisplayGamutP3,
};

typedef NSString* NSDeviceDescriptionKey NS_TYPED_EXTENSIBLE_ENUM;
APPKIT_EXTERN NSDeviceDescriptionKey const NSDeviceResolution;
APPKIT_EXTERN NSDeviceDescriptionKey const NSDeviceColorSpaceName;
APPKIT_EXTERN NSDeviceDescriptionKey const NSDeviceBitsPerSample;
APPKIT_EXTERN NSDeviceDescriptionKey const NSDeviceIsScreen;
APPKIT_EXTERN NSDeviceDescriptionKey const NSDeviceIsPrinter;
APPKIT_EXTERN NSDeviceDescriptionKey const NSDeviceSize;

APPKIT_EXTERN void NSRectFill(NSRect rect);
APPKIT_EXTERN void NSRectFillUsingOperation(NSRect rect,
                                            NSCompositingOperation op);
APPKIT_EXTERN void NSFrameRect(NSRect rect);
APPKIT_EXTERN void NSBeep(void);

// -- NSUserInterfaceLayout.h -------------------------------------------------

typedef NS_ENUM(NSInteger, NSUserInterfaceLayoutDirection){
    NSUserInterfaceLayoutDirectionLeftToRight = 0,
    NSUserInterfaceLayoutDirectionRightToLeft = 1,
};

typedef NS_ENUM(NSInteger, NSUserInterfaceLayoutOrientation){
    NSUserInterfaceLayoutOrientationHorizontal = 0,
    NSUserInterfaceLayoutOrientationVertical = 1,
};

// -- NSUserInterfaceValidation.h ---------------------------------------------

@protocol NSValidatedUserInterfaceItem
@property(readonly, nullable) SEL action;
@property(readonly) NSInteger tag;
@end

@protocol NSUserInterfaceValidations
- (BOOL)validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item;
@end

// -- NSUserInterfaceItemIdentification.h -------------------------------------

typedef NSString* NSUserInterfaceItemIdentifier NS_SWIFT_BRIDGED_TYPEDEF;

@protocol NSUserInterfaceItemIdentification
@property(nullable, copy) NSUserInterfaceItemIdentifier identifier;
@end

// -- NSAppearance.h ----------------------------------------------------------

typedef NSString* NSAppearanceName NS_SWIFT_BRIDGED_TYPEDEF;

@interface NSAppearance : NSObject <NSSecureCoding>
@property(readonly, copy) NSAppearanceName name;
@property(class, null_resettable, strong) NSAppearance* currentAppearance;
@property(class, readonly, strong) NSAppearance* currentDrawingAppearance;
- (void)performAsCurrentDrawingAppearance:(void(NS_NOESCAPE ^)(void))block;
+ (nullable NSAppearance*)appearanceNamed:(NSAppearanceName)name;
- (nullable NSAppearanceName)bestMatchFromAppearancesWithNames:
    (NSArray<NSAppearanceName>*)appearances;
@property(readonly) BOOL allowsVibrancy;
@end

APPKIT_EXTERN NSAppearanceName const NSAppearanceNameAqua;
APPKIT_EXTERN NSAppearanceName const NSAppearanceNameDarkAqua;
APPKIT_EXTERN NSAppearanceName const NSAppearanceNameVibrantDark;
APPKIT_EXTERN NSAppearanceName const NSAppearanceNameVibrantLight;
APPKIT_EXTERN NSAppearanceName const NSAppearanceNameAccessibilityHighContrastAqua;
APPKIT_EXTERN NSAppearanceName const NSAppearanceNameAccessibilityHighContrastDarkAqua;

@protocol NSAppearanceCustomization <NSObject>
@property(nullable, strong) NSAppearance* appearance;
@property(readonly, strong) NSAppearance* effectiveAppearance;
@end

// -- NSColorSpace.h, NSColor.h -----------------------------------------------

@interface NSColorSpace : NSObject <NSSecureCoding>
- (nullable instancetype)initWithCGColorSpace:(CGColorSpaceRef)cgColorSpace;
@property(nullable, readonly) CGColorSpaceRef CGColorSpace;
@property(class, readonly, strong) NSColorSpace* sRGBColorSpace;
@property(class, readonly, strong) NSColorSpace* genericRGBColorSpace;
@property(class, readonly, strong) NSColorSpace* deviceRGBColorSpace;
@property(class, readonly, strong) NSColorSpace* displayP3ColorSpace;
@property(class, readonly, strong) NSColorSpace* extendedSRGBColorSpace;
@property(nullable, readonly, copy) NSString* localizedName;
@end

typedef NSString* NSColorName NS_SWIFT_BRIDGED_TYPEDEF;
typedef NSString* NSColorListName NS_SWIFT_BRIDGED_TYPEDEF;

@interface NSColor : NSObject <NSCopying, NSSecureCoding>
+ (NSColor*)colorWithSRGBRed:(CGFloat)red
                       green:(CGFloat)green
                        blue:(CGFloat)blue
                       alpha:(CGFloat)alpha;
+ (NSColor*)colorWithDisplayP3Red:(CGFloat)red
                            green:(CGFloat)green
                             blue:(CGFloat)blue
                            alpha:(CGFloat)alpha;
+ (NSColor*)colorWithCalibratedRed:(CGFloat)red
                             green:(CGFloat)green
                              blue:(CGFloat)blue
                             alpha:(CGFloat)alpha;
+ (NSColor*)colorWithDeviceRed:(CGFloat)red
                         green:(CGFloat)green
                          blue:(CGFloat)blue
                         alpha:(CGFloat)alpha;
+ (NSColor*)colorWithCalibratedWhite:(CGFloat)white alpha:(CGFloat)alpha;
+ (NSColor*)colorWithDeviceWhite:(CGFloat)white alpha:(CGFloat)alpha;
+ (NSColor*)colorWithWhite:(CGFloat)white alpha:(CGFloat)alpha;
+ (NSColor*)colorWithRed:(CGFloat)red
                   green:(CGFloat)green
                    blue:(CGFloat)blue
                   alpha:(CGFloat)alpha;
+ (nullable NSColor*)colorWithCGColor:(CGColorRef)cgColor;
+ (NSColor*)colorWithColorSpace:(NSColorSpace*)space
                     components:(const CGFloat*)components
                          count:(NSInteger)numberOfComponents;
+ (NSColor*)colorWithName:(nullable NSColorName)colorName
          dynamicProvider:
              (NSColor* (^)(NSAppearance* appearance))dynamicProvider;
@property(class, strong, readonly) NSColor* blackColor;
@property(class, strong, readonly) NSColor* darkGrayColor;
@property(class, strong, readonly) NSColor* lightGrayColor;
@property(class, strong, readonly) NSColor* whiteColor;
@property(class, strong, readonly) NSColor* grayColor;
@property(class, strong, readonly) NSColor* redColor;
@property(class, strong, readonly) NSColor* greenColor;
@property(class, strong, readonly) NSColor* blueColor;
@property(class, strong, readonly) NSColor* cyanColor;
@property(class, strong, readonly) NSColor* yellowColor;
@property(class, strong, readonly) NSColor* magentaColor;
@property(class, strong, readonly) NSColor* orangeColor;
@property(class, strong, readonly) NSColor* purpleColor;
@property(class, strong, readonly) NSColor* brownColor;
@property(class, strong, readonly) NSColor* clearColor;
@property(class, strong, readonly) NSColor* labelColor;
@property(class, strong, readonly) NSColor* secondaryLabelColor;
@property(class, strong, readonly) NSColor* tertiaryLabelColor;
@property(class, strong, readonly) NSColor* textColor;
@property(class, strong, readonly) NSColor* textBackgroundColor;
@property(class, strong, readonly) NSColor* selectedTextColor;
@property(class, strong, readonly) NSColor* selectedTextBackgroundColor;
@property(class, strong, readonly) NSColor* controlColor;
@property(class, strong, readonly) NSColor* controlTextColor;
@property(class, strong, readonly) NSColor* controlBackgroundColor;
@property(class, strong, readonly) NSColor* controlAccentColor;
@property(class, strong, readonly) NSColor* windowBackgroundColor;
@property(class, strong, readonly) NSColor* windowFrameTextColor;
@property(class, strong, readonly) NSColor* underPageBackgroundColor;
@property(class, strong, readonly) NSColor* separatorColor;
@property(class, strong, readonly) NSColor* gridColor;
@property(class, strong, readonly) NSColor* headerTextColor;
@property(class, strong, readonly) NSColor* keyboardFocusIndicatorColor;
@property(class, strong, readonly) NSColor* selectedContentBackgroundColor;
@property(class, strong, readonly) NSColor* unemphasizedSelectedContentBackgroundColor;
@property(class, strong, readonly) NSColor* alternateSelectedControlTextColor;
@property(class, strong, readonly) NSColor* systemBlueColor;
@property(class, strong, readonly) NSColor* systemGrayColor;
@property(class, strong, readonly) NSColor* systemRedColor;
@property(class, strong, readonly) NSColor* linkColor;
@property(class, strong, readonly) NSColor* shadowColor;
@property(class, strong, readonly) NSColor* highlightColor;
@property(class, strong, readonly) NSColor* scrubberTexturedBackgroundColor;
- (NSColor*)colorWithAlphaComponent:(CGFloat)alpha;
- (nullable NSColor*)blendedColorWithFraction:(CGFloat)fraction
                                      ofColor:(NSColor*)color;
- (nullable NSColor*)colorUsingColorSpace:(NSColorSpace*)space;
- (void)set;
- (void)setFill;
- (void)setStroke;
@property(readonly) CGColorRef CGColor;
@property(readonly, strong) NSColorSpace* colorSpace;
@property(readonly) NSInteger numberOfComponents;
- (void)getComponents:(CGFloat*)components;
@property(readonly) CGFloat redComponent;
@property(readonly) CGFloat greenComponent;
@property(readonly) CGFloat blueComponent;
@property(readonly) CGFloat whiteComponent;
@property(readonly) CGFloat alphaComponent;
- (void)getRed:(nullable CGFloat*)red
         green:(nullable CGFloat*)green
          blue:(nullable CGFloat*)blue
         alpha:(nullable CGFloat*)alpha;
@end

// -- NSImage.h, NSImageRep.h -------------------------------------------------

typedef NSString* NSImageName NS_SWIFT_BRIDGED_TYPEDEF;
typedef NSString* NSImageHintKey NS_TYPED_EXTENSIBLE_ENUM;
@class NSGraphicsContext;

@interface NSImageRep : NSObject <NSCopying, NSCoding>
@property NSSize size;
@property NSInteger pixelsWide;
@property NSInteger pixelsHigh;
- (BOOL)draw;
@end

@interface NSBitmapImageRep : NSImageRep <NSSecureCoding>
- (instancetype)initWithCGImage:(CGImageRef)cgImage;
@property(nullable, readonly) CGImageRef CGImage;
@end

@interface NSImage : NSObject <NSCopying, NSSecureCoding>
+ (nullable NSImage*)imageNamed:(NSImageName)name;
+ (nullable NSImage*)imageWithSystemSymbolName:(NSString*)name
                      accessibilityDescription:(nullable NSString*)description;
- (instancetype)initWithSize:(NSSize)size NS_DESIGNATED_INITIALIZER;
- (nullable instancetype)initWithData:(NSData*)data;
- (nullable instancetype)initWithContentsOfFile:(NSString*)fileName;
- (nullable instancetype)initWithContentsOfURL:(NSURL*)url;
- (instancetype)initWithCGImage:(CGImageRef)cgImage size:(NSSize)size;
+ (instancetype)imageWithSize:(NSSize)size
                      flipped:(BOOL)drawingHandlerShouldBeCalledWithFlippedContext
               drawingHandler:(BOOL (^)(NSRect dstRect))drawingHandler;
@property NSSize size;
- (void)setTemplate:(BOOL)isTemplate;
- (BOOL)isTemplate;
@property(readonly, copy) NSArray<NSImageRep*>* representations;
- (void)addRepresentation:(NSImageRep*)imageRep;
- (void)removeRepresentation:(NSImageRep*)imageRep;
@property(nullable, copy) NSString* accessibilityDescription;
- (nullable CGImageRef)CGImageForProposedRect:(nullable NSRect*)proposedDestRect
                                      context:(nullable NSGraphicsContext*)
                                                  referenceContext
                                        hints:(nullable NSDictionary<
                                                  NSImageHintKey, id>*)hints;
- (void)drawInRect:(NSRect)rect;
- (void)drawInRect:(NSRect)rect
          fromRect:(NSRect)fromRect
         operation:(NSCompositingOperation)op
          fraction:(CGFloat)delta;
- (void)drawAtPoint:(NSPoint)point
           fromRect:(NSRect)fromRect
          operation:(NSCompositingOperation)op
           fraction:(CGFloat)delta;
- (void)lockFocus;
- (void)unlockFocus;
@property(nullable, readonly) NSData* TIFFRepresentation;
@end

// -- NSResponder.h -----------------------------------------------------------

@class NSEvent, NSMenu, NSWindow, NSView, NSTouchBar, NSUndoManager;

typedef NS_ENUM(NSInteger, NSEventGestureAxis){
    NSEventGestureAxisNone = 0,
    NSEventGestureAxisHorizontal,
    NSEventGestureAxisVertical,
};

@protocol NSStandardKeyBindingResponding <NSObject>
@optional
- (void)insertText:(id)insertString;
- (void)doCommandBySelector:(SEL)selector;
- (void)moveForward:(nullable id)sender;
- (void)moveRight:(nullable id)sender;
- (void)moveBackward:(nullable id)sender;
- (void)moveLeft:(nullable id)sender;
- (void)moveUp:(nullable id)sender;
- (void)moveDown:(nullable id)sender;
- (void)moveWordForward:(nullable id)sender;
- (void)moveWordBackward:(nullable id)sender;
- (void)moveToBeginningOfLine:(nullable id)sender;
- (void)moveToEndOfLine:(nullable id)sender;
- (void)moveToBeginningOfParagraph:(nullable id)sender;
- (void)moveToEndOfParagraph:(nullable id)sender;
- (void)moveToEndOfDocument:(nullable id)sender;
- (void)moveToBeginningOfDocument:(nullable id)sender;
- (void)pageDown:(nullable id)sender;
- (void)pageUp:(nullable id)sender;
- (void)centerSelectionInVisibleArea:(nullable id)sender;
- (void)scrollPageUp:(nullable id)sender;
- (void)scrollPageDown:(nullable id)sender;
- (void)scrollLineUp:(nullable id)sender;
- (void)scrollLineDown:(nullable id)sender;
- (void)scrollToBeginningOfDocument:(nullable id)sender;
- (void)scrollToEndOfDocument:(nullable id)sender;
- (void)selectAll:(nullable id)sender;
- (void)deleteForward:(nullable id)sender;
- (void)deleteBackward:(nullable id)sender;
- (void)insertTab:(nullable id)sender;
- (void)insertBacktab:(nullable id)sender;
- (void)insertNewline:(nullable id)sender;
- (void)cancelOperation:(nullable id)sender;
@end

@interface NSResponder : NSObject <NSCoding>
- (instancetype)init NS_DESIGNATED_INITIALIZER;
@property(nullable, weak) NSResponder* nextResponder;
- (BOOL)tryToPerform:(SEL)action with:(nullable id)object;
- (BOOL)performKeyEquivalent:(NSEvent*)event;
- (nullable id)validRequestorForSendType:(nullable NSString*)sendType
                              returnType:(nullable NSString*)returnType;
- (void)mouseDown:(NSEvent*)event;
- (void)rightMouseDown:(NSEvent*)event;
- (void)otherMouseDown:(NSEvent*)event;
- (void)mouseUp:(NSEvent*)event;
- (void)rightMouseUp:(NSEvent*)event;
- (void)otherMouseUp:(NSEvent*)event;
- (void)mouseMoved:(NSEvent*)event;
- (void)mouseDragged:(NSEvent*)event;
- (void)scrollWheel:(NSEvent*)event;
- (void)rightMouseDragged:(NSEvent*)event;
- (void)otherMouseDragged:(NSEvent*)event;
- (void)mouseEntered:(NSEvent*)event;
- (void)mouseExited:(NSEvent*)event;
- (void)keyDown:(NSEvent*)event;
- (void)keyUp:(NSEvent*)event;
- (void)flagsChanged:(NSEvent*)event;
- (void)tabletPoint:(NSEvent*)event;
- (void)tabletProximity:(NSEvent*)event;
- (void)cursorUpdate:(NSEvent*)event;
- (void)magnifyWithEvent:(NSEvent*)event;
- (void)rotateWithEvent:(NSEvent*)event;
- (void)swipeWithEvent:(NSEvent*)event;
- (void)beginGestureWithEvent:(NSEvent*)event;
- (void)endGestureWithEvent:(NSEvent*)event;
- (void)smartMagnifyWithEvent:(NSEvent*)event;
- (void)pressureChangeWithEvent:(NSEvent*)event;
- (void)touchesBeganWithEvent:(NSEvent*)event;
- (void)touchesMovedWithEvent:(NSEvent*)event;
- (void)touchesEndedWithEvent:(NSEvent*)event;
- (void)touchesCancelledWithEvent:(NSEvent*)event;
- (void)quickLookWithEvent:(NSEvent*)event;
- (BOOL)wantsScrollEventsForSwipeTrackingOnAxis:(NSEventGestureAxis)axis;
- (BOOL)wantsForwardedScrollEventsForAxis:(NSEventGestureAxis)axis;
- (void)noResponderFor:(SEL)eventSelector;
@property(readonly) BOOL acceptsFirstResponder;
- (BOOL)becomeFirstResponder;
- (BOOL)resignFirstResponder;
- (void)interpretKeyEvents:(NSArray<NSEvent*>*)eventArray;
- (void)flushBufferedKeyEvents;
@property(nullable, strong) NSMenu* menu;
- (void)showContextHelp:(nullable id)sender;
- (void)helpRequested:(NSEvent*)eventPtr;
- (BOOL)shouldBeTreatedAsInkEvent:(NSEvent*)event;
- (BOOL)validateProposedFirstResponder:(NSResponder*)responder
                              forEvent:(nullable NSEvent*)event;
@property(nullable, readonly, strong) NSUndoManager* undoManager;
@property(nullable, strong) NSTouchBar* touchBar;
- (nullable NSTouchBar*)makeTouchBar;
- (void)encodeRestorableStateWithCoder:(NSCoder*)coder;
- (void)restoreStateWithCoder:(NSCoder*)coder;
- (void)invalidateRestorableState;
- (void)updateUserActivityState:(NSUserActivity*)userActivity;
@property(nullable, strong) NSUserActivity* userActivity;
@end

@interface NSResponder (NSStandardKeyBindingMethods) <
    NSStandardKeyBindingResponding>
@end

// -- NSTouchBar.h ------------------------------------------------------------

typedef NSString* NSTouchBarItemIdentifier NS_TYPED_EXTENSIBLE_ENUM;
typedef NSString* NSTouchBarCustomizationIdentifier NS_TYPED_EXTENSIBLE_ENUM;

@interface NSTouchBar : NSObject <NSCoding>
- (instancetype)init NS_DESIGNATED_INITIALIZER;
@property(nullable, copy) NSArray<NSTouchBarItemIdentifier>* defaultItemIdentifiers;
@property(nullable, weak) id delegate;
@end

@protocol NSTouchBarProvider <NSObject>
@property(strong, readonly, nullable) NSTouchBar* touchBar;
@end

// -- NSEvent.h ---------------------------------------------------------------

typedef NS_ENUM(NSUInteger, NSEventType){
    NSEventTypeLeftMouseDown = 1,
    NSEventTypeLeftMouseUp = 2,
    NSEventTypeRightMouseDown = 3,
    NSEventTypeRightMouseUp = 4,
    NSEventTypeMouseMoved = 5,
    NSEventTypeLeftMouseDragged = 6,
    NSEventTypeRightMouseDragged = 7,
    NSEventTypeMouseEntered = 8,
    NSEventTypeMouseExited = 9,
    NSEventTypeKeyDown = 10,
    NSEventTypeKeyUp = 11,
    NSEventTypeFlagsChanged = 12,
    NSEventTypeAppKitDefined = 13,
    NSEventTypeSystemDefined = 14,
    NSEventTypeApplicationDefined = 15,
    NSEventTypePeriodic = 16,
    NSEventTypeCursorUpdate = 17,
    NSEventTypeScrollWheel = 22,
    NSEventTypeTabletPoint = 23,
    NSEventTypeTabletProximity = 24,
    NSEventTypeOtherMouseDown = 25,
    NSEventTypeOtherMouseUp = 26,
    NSEventTypeOtherMouseDragged = 27,
    NSEventTypeGesture = 29,
    NSEventTypeMagnify = 30,
    NSEventTypeSwipe = 31,
    NSEventTypeRotate = 18,
    NSEventTypeBeginGesture = 19,
    NSEventTypeEndGesture = 20,
    NSEventTypeSmartMagnify = 32,
    NSEventTypeQuickLook = 33,
    NSEventTypePressure = 34,
    NSEventTypeDirectTouch = 37,
    NSEventTypeChangeMode = 38,
};

typedef NS_OPTIONS(unsigned long long, NSEventMask){
    NSEventMaskLeftMouseDown = 1ULL << NSEventTypeLeftMouseDown,
    NSEventMaskLeftMouseUp = 1ULL << NSEventTypeLeftMouseUp,
    NSEventMaskRightMouseDown = 1ULL << NSEventTypeRightMouseDown,
    NSEventMaskRightMouseUp = 1ULL << NSEventTypeRightMouseUp,
    NSEventMaskMouseMoved = 1ULL << NSEventTypeMouseMoved,
    NSEventMaskLeftMouseDragged = 1ULL << NSEventTypeLeftMouseDragged,
    NSEventMaskRightMouseDragged = 1ULL << NSEventTypeRightMouseDragged,
    NSEventMaskMouseEntered = 1ULL << NSEventTypeMouseEntered,
    NSEventMaskMouseExited = 1ULL << NSEventTypeMouseExited,
    NSEventMaskKeyDown = 1ULL << NSEventTypeKeyDown,
    NSEventMaskKeyUp = 1ULL << NSEventTypeKeyUp,
    NSEventMaskFlagsChanged = 1ULL << NSEventTypeFlagsChanged,
    NSEventMaskAppKitDefined = 1ULL << NSEventTypeAppKitDefined,
    NSEventMaskSystemDefined = 1ULL << NSEventTypeSystemDefined,
    NSEventMaskApplicationDefined = 1ULL << NSEventTypeApplicationDefined,
    NSEventMaskPeriodic = 1ULL << NSEventTypePeriodic,
    NSEventMaskCursorUpdate = 1ULL << NSEventTypeCursorUpdate,
    NSEventMaskScrollWheel = 1ULL << NSEventTypeScrollWheel,
    NSEventMaskTabletPoint = 1ULL << NSEventTypeTabletPoint,
    NSEventMaskTabletProximity = 1ULL << NSEventTypeTabletProximity,
    NSEventMaskOtherMouseDown = 1ULL << NSEventTypeOtherMouseDown,
    NSEventMaskOtherMouseUp = 1ULL << NSEventTypeOtherMouseUp,
    NSEventMaskOtherMouseDragged = 1ULL << NSEventTypeOtherMouseDragged,
    NSEventMaskGesture = 1ULL << NSEventTypeGesture,
    NSEventMaskMagnify = 1ULL << NSEventTypeMagnify,
    NSEventMaskSwipe = 1ULL << NSEventTypeSwipe,
    NSEventMaskRotate = 1ULL << NSEventTypeRotate,
    NSEventMaskBeginGesture = 1ULL << NSEventTypeBeginGesture,
    NSEventMaskEndGesture = 1ULL << NSEventTypeEndGesture,
    NSEventMaskSmartMagnify = 1ULL << NSEventTypeSmartMagnify,
    NSEventMaskPressure = 1ULL << NSEventTypePressure,
    NSEventMaskDirectTouch = 1ULL << NSEventTypeDirectTouch,
    NSEventMaskChangeMode = 1ULL << NSEventTypeChangeMode,
    NSEventMaskAny = NSUIntegerMax,
};

NS_INLINE NSEventMask NSEventMaskFromType(NSEventType type) {
  return (1ULL << type);
}

typedef NS_OPTIONS(NSUInteger, NSEventModifierFlags){
    NSEventModifierFlagCapsLock = 1 << 16,
    NSEventModifierFlagShift = 1 << 17,
    NSEventModifierFlagControl = 1 << 18,
    NSEventModifierFlagOption = 1 << 19,
    NSEventModifierFlagCommand = 1 << 20,
    NSEventModifierFlagNumericPad = 1 << 21,
    NSEventModifierFlagHelp = 1 << 22,
    NSEventModifierFlagFunction = 1 << 23,
    NSEventModifierFlagDeviceIndependentFlagsMask = 0xffff0000UL,
};

typedef NS_ENUM(short, NSEventSubtype){
    NSEventSubtypeWindowExposed = 0,
    NSEventSubtypeApplicationActivated = 1,
    NSEventSubtypeApplicationDeactivated = 2,
    NSEventSubtypeWindowMoved = 4,
    NSEventSubtypeScreenChanged = 8,
    NSEventSubtypePowerOff = 1,
    NSEventSubtypeMouseEvent = 0,
    NSEventSubtypeTabletPoint = 1,
    NSEventSubtypeTabletProximity = 2,
    NSEventSubtypeTouch = 3,
};

typedef NS_OPTIONS(NSUInteger, NSEventPhase){
    NSEventPhaseNone = 0,
    NSEventPhaseBegan = 0x1 << 0,
    NSEventPhaseStationary = 0x1 << 1,
    NSEventPhaseChanged = 0x1 << 2,
    NSEventPhaseEnded = 0x1 << 3,
    NSEventPhaseCancelled = 0x1 << 4,
    NSEventPhaseMayBegin = 0x1 << 5,
};

typedef NS_OPTIONS(NSUInteger, NSEventSwipeTrackingOptions){
    NSEventSwipeTrackingLockDirection = 0x1 << 0,
    NSEventSwipeTrackingClampGestureAmount = 0x1 << 1,
};

typedef NS_ENUM(NSUInteger, NSPointingDeviceType){
    NSPointingDeviceTypeUnknown = 0,
    NSPointingDeviceTypePen = 1,
    NSPointingDeviceTypeCursor = 2,
    NSPointingDeviceTypeEraser = 3,
};

typedef NS_ENUM(NSInteger, NSPressureBehavior){
    NSPressureBehaviorUnknown = -1,
    NSPressureBehaviorPrimaryDefault = 0,
    NSPressureBehaviorPrimaryClick = 1,
    NSPressureBehaviorPrimaryGeneric = 2,
    NSPressureBehaviorPrimaryAccelerator = 3,
    NSPressureBehaviorPrimaryDeepClick = 5,
    NSPressureBehaviorPrimaryDeepDrag = 6,
};

@class NSTrackingArea, NSGraphicsContext;

@interface NSEvent : NSObject <NSCopying, NSCoding>
@property(readonly) NSEventType type;
@property(readonly) NSEventModifierFlags modifierFlags;
@property(readonly) NSTimeInterval timestamp;
@property(nullable, readonly, weak) NSWindow* window;
@property(readonly) NSInteger windowNumber;
@property(readonly) NSInteger clickCount;
@property(readonly) NSInteger buttonNumber;
@property(readonly) NSInteger eventNumber;
@property(readonly) float pressure;
@property(readonly) NSPoint locationInWindow;
@property(readonly) CGFloat deltaX;
@property(readonly) CGFloat deltaY;
@property(readonly) CGFloat deltaZ;
@property(readonly) BOOL hasPreciseScrollingDeltas;
@property(readonly) CGFloat scrollingDeltaX;
@property(readonly) CGFloat scrollingDeltaY;
@property(readonly) NSEventPhase momentumPhase;
@property(getter=isDirectionInvertedFromDevice, readonly)
    BOOL directionInvertedFromDevice;
@property(nullable, readonly, copy) NSString* characters;
@property(nullable, readonly, copy) NSString* charactersIgnoringModifiers;
- (nullable NSString*)charactersByApplyingModifiers:
    (NSEventModifierFlags)modifiers;
@property(getter=isARepeat, readonly) BOOL ARepeat;
@property(readonly) unsigned short keyCode;
@property(readonly) NSInteger trackingNumber;
@property(nullable, readonly) void* userData;
@property(nullable, readonly, strong) NSTrackingArea* trackingArea;
@property(readonly) NSEventSubtype subtype;
@property(readonly) NSInteger data1;
@property(readonly) NSInteger data2;
@property(nullable, readonly) const void* eventRef;
+ (nullable NSEvent*)eventWithEventRef:(const void*)eventRef;
@property(nullable, readonly) CGEventRef CGEvent;
+ (nullable NSEvent*)eventWithCGEvent:(CGEventRef)cgEvent;
@property(class, getter=isMouseCoalescingEnabled) BOOL mouseCoalescingEnabled;
@property(readonly) CGFloat magnification;
@property(readonly) NSUInteger deviceID;
@property(readonly) float rotation;
@property(readonly) NSInteger absoluteX;
@property(readonly) NSInteger absoluteY;
@property(readonly) NSInteger absoluteZ;
@property(readonly) NSUInteger buttonMask;
@property(readonly) NSPoint tilt;
@property(readonly) float tangentialPressure;
@property(readonly) id vendorDefined;
@property(readonly) NSUInteger vendorID;
@property(readonly) NSUInteger tabletID;
@property(readonly) NSUInteger pointingDeviceID;
@property(readonly) NSUInteger systemTabletID;
@property(readonly) NSUInteger vendorPointingDeviceType;
@property(readonly) NSUInteger pointingDeviceSerialNumber;
@property(readonly) unsigned long long uniqueID;
@property(readonly) NSUInteger capabilityMask;
@property(readonly) NSPointingDeviceType pointingDeviceType;
@property(readonly, getter=isEnteringProximity) BOOL enteringProximity;
@property(readonly) NSEventPhase phase;
@property(readonly) NSInteger stage;
@property(readonly) CGFloat stageTransition;
@property(readonly) NSEventMask associatedEventsMask;
@property(readonly) NSPressureBehavior pressureBehavior;
@property(class, readonly, getter=isSwipeTrackingFromScrollEventsEnabled)
    BOOL swipeTrackingFromScrollEventsEnabled;
- (void)trackSwipeEventWithOptions:(NSEventSwipeTrackingOptions)options
          dampenAmountThresholdMin:(CGFloat)minDampenThreshold
                               max:(CGFloat)maxDampenThreshold
                      usingHandler:(void (^)(CGFloat gestureAmount,
                                             NSEventPhase phase,
                                             BOOL isComplete,
                                             BOOL* stop))trackingHandler;
+ (void)startPeriodicEventsAfterDelay:(NSTimeInterval)delay
                           withPeriod:(NSTimeInterval)period;
+ (void)stopPeriodicEvents;
+ (nullable NSEvent*)mouseEventWithType:(NSEventType)type
                               location:(NSPoint)location
                          modifierFlags:(NSEventModifierFlags)flags
                              timestamp:(NSTimeInterval)time
                           windowNumber:(NSInteger)wNum
                                context:(nullable NSGraphicsContext*)unused
                            eventNumber:(NSInteger)eNum
                             clickCount:(NSInteger)cNum
                               pressure:(float)pressure;
+ (nullable NSEvent*)keyEventWithType:(NSEventType)type
                             location:(NSPoint)location
                        modifierFlags:(NSEventModifierFlags)flags
                            timestamp:(NSTimeInterval)time
                         windowNumber:(NSInteger)wNum
                              context:(nullable NSGraphicsContext*)unused
                           characters:(NSString*)keys
          charactersIgnoringModifiers:(NSString*)ukeys
                            isARepeat:(BOOL)flag
                              keyCode:(unsigned short)code;
+ (nullable NSEvent*)enterExitEventWithType:(NSEventType)type
                                   location:(NSPoint)location
                              modifierFlags:(NSEventModifierFlags)flags
                                  timestamp:(NSTimeInterval)time
                               windowNumber:(NSInteger)wNum
                                    context:(nullable NSGraphicsContext*)unused
                                eventNumber:(NSInteger)eNum
                             trackingNumber:(NSInteger)tNum
                                   userData:(nullable void*)data;
+ (nullable NSEvent*)otherEventWithType:(NSEventType)type
                               location:(NSPoint)location
                          modifierFlags:(NSEventModifierFlags)flags
                              timestamp:(NSTimeInterval)time
                           windowNumber:(NSInteger)wNum
                                context:(nullable NSGraphicsContext*)unused
                                subtype:(short)subtype
                                  data1:(NSInteger)d1
                                  data2:(NSInteger)d2;
@property(class, readonly) NSPoint mouseLocation;
@property(class, readonly) NSEventModifierFlags modifierFlags;
@property(class, readonly) NSUInteger pressedMouseButtons;
@property(class, readonly) NSTimeInterval doubleClickInterval;
@property(class, readonly) NSTimeInterval keyRepeatDelay;
@property(class, readonly) NSTimeInterval keyRepeatInterval;
+ (nullable id)addGlobalMonitorForEventsMatchingMask:(NSEventMask)mask
                                             handler:(void (^)(NSEvent* event))
                                                         block;
+ (nullable id)addLocalMonitorForEventsMatchingMask:(NSEventMask)mask
                                            handler:(NSEvent* _Nullable (^)(
                                                        NSEvent* event))block;
+ (void)removeMonitor:(id)eventMonitor;
@end

enum {
  NSUpArrowFunctionKey = 0xF700,
  NSDownArrowFunctionKey = 0xF701,
  NSLeftArrowFunctionKey = 0xF702,
  NSRightArrowFunctionKey = 0xF703,
  NSF1FunctionKey = 0xF704,
  NSF2FunctionKey = 0xF705,
  NSF3FunctionKey = 0xF706,
  NSF4FunctionKey = 0xF707,
  NSF5FunctionKey = 0xF708,
  NSF6FunctionKey = 0xF709,
  NSF7FunctionKey = 0xF70A,
  NSF8FunctionKey = 0xF70B,
  NSF9FunctionKey = 0xF70C,
  NSF10FunctionKey = 0xF70D,
  NSF11FunctionKey = 0xF70E,
  NSF12FunctionKey = 0xF70F,
  NSF13FunctionKey = 0xF710,
  NSF14FunctionKey = 0xF711,
  NSF15FunctionKey = 0xF712,
  NSF16FunctionKey = 0xF713,
  NSF17FunctionKey = 0xF714,
  NSF18FunctionKey = 0xF715,
  NSF19FunctionKey = 0xF716,
  NSF20FunctionKey = 0xF717,
  NSF21FunctionKey = 0xF718,
  NSF22FunctionKey = 0xF719,
  NSF23FunctionKey = 0xF71A,
  NSF24FunctionKey = 0xF71B,
  NSInsertFunctionKey = 0xF727,
  NSDeleteFunctionKey = 0xF728,
  NSHomeFunctionKey = 0xF729,
  NSBeginFunctionKey = 0xF72A,
  NSEndFunctionKey = 0xF72B,
  NSPageUpFunctionKey = 0xF72C,
  NSPageDownFunctionKey = 0xF72D,
  NSPrintScreenFunctionKey = 0xF72E,
  NSScrollLockFunctionKey = 0xF72F,
  NSPauseFunctionKey = 0xF730,
  NSSysReqFunctionKey = 0xF731,
  NSBreakFunctionKey = 0xF732,
  NSResetFunctionKey = 0xF733,
  NSStopFunctionKey = 0xF734,
  NSMenuFunctionKey = 0xF735,
  NSUserFunctionKey = 0xF736,
  NSSystemFunctionKey = 0xF737,
  NSPrintFunctionKey = 0xF738,
  NSClearLineFunctionKey = 0xF739,
  NSClearDisplayFunctionKey = 0xF73A,
  NSInsertLineFunctionKey = 0xF73B,
  NSDeleteLineFunctionKey = 0xF73C,
  NSInsertCharFunctionKey = 0xF73D,
  NSDeleteCharFunctionKey = 0xF73E,
  NSPrevFunctionKey = 0xF73F,
  NSNextFunctionKey = 0xF740,
  NSSelectFunctionKey = 0xF741,
  NSExecuteFunctionKey = 0xF742,
  NSUndoFunctionKey = 0xF743,
  NSRedoFunctionKey = 0xF744,
  NSFindFunctionKey = 0xF745,
  NSHelpFunctionKey = 0xF746,
  NSModeSwitchFunctionKey = 0xF747,
};

// -- NSTrackingArea.h --------------------------------------------------------

typedef NS_OPTIONS(NSUInteger, NSTrackingAreaOptions){
    NSTrackingMouseEnteredAndExited = 0x01,
    NSTrackingMouseMoved = 0x02,
    NSTrackingCursorUpdate = 0x04,
    NSTrackingActiveWhenFirstResponder = 0x10,
    NSTrackingActiveInKeyWindow = 0x20,
    NSTrackingActiveInActiveApp = 0x40,
    NSTrackingActiveAlways = 0x80,
    NSTrackingAssumeInside = 0x100,
    NSTrackingInVisibleRect = 0x200,
    NSTrackingEnabledDuringMouseDrag = 0x400,
};

@interface NSTrackingArea : NSObject <NSCopying, NSCoding>
- (instancetype)initWithRect:(NSRect)rect
                     options:(NSTrackingAreaOptions)options
                       owner:(nullable id)owner
                    userInfo:(nullable NSDictionary<id, id>*)userInfo;
@property(readonly) NSRect rect;
@property(readonly) NSTrackingAreaOptions options;
@property(nullable, readonly, weak) id owner;
@property(nullable, readonly, copy) NSDictionary<id, id>* userInfo;
@end

// -- NSCursor.h --------------------------------------------------------------

@interface NSCursor : NSObject <NSSecureCoding>
@property(class, readonly, strong) NSCursor* currentCursor;
@property(class, readonly, strong) NSCursor* currentSystemCursor;
@property(class, readonly, strong) NSCursor* arrowCursor;
@property(class, readonly, strong) NSCursor* IBeamCursor;
@property(class, readonly, strong) NSCursor* pointingHandCursor;
@property(class, readonly, strong) NSCursor* closedHandCursor;
@property(class, readonly, strong) NSCursor* openHandCursor;
@property(class, readonly, strong) NSCursor* resizeLeftCursor;
@property(class, readonly, strong) NSCursor* resizeRightCursor;
@property(class, readonly, strong) NSCursor* resizeLeftRightCursor;
@property(class, readonly, strong) NSCursor* resizeUpCursor;
@property(class, readonly, strong) NSCursor* resizeDownCursor;
@property(class, readonly, strong) NSCursor* resizeUpDownCursor;
@property(class, readonly, strong) NSCursor* crosshairCursor;
@property(class, readonly, strong) NSCursor* disappearingItemCursor;
@property(class, readonly, strong) NSCursor* operationNotAllowedCursor;
@property(class, readonly, strong) NSCursor* dragLinkCursor;
@property(class, readonly, strong) NSCursor* dragCopyCursor;
@property(class, readonly, strong) NSCursor* contextualMenuCursor;
@property(class, readonly, strong) NSCursor* IBeamCursorForVerticalLayout;
- (instancetype)initWithImage:(NSImage*)newImage
                      hotSpot:(NSPoint)point NS_DESIGNATED_INITIALIZER;
+ (void)hide;
+ (void)unhide;
+ (void)setHiddenUntilMouseMoves:(BOOL)flag;
- (void)set;
- (void)push;
- (void)pop;
@property(readonly, strong) NSImage* image;
@property(readonly) NSPoint hotSpot;
@end

// -- NSPasteboard.h ----------------------------------------------------------

typedef NSString* NSPasteboardType NS_TYPED_EXTENSIBLE_ENUM;
typedef NSString* NSPasteboardName NS_TYPED_EXTENSIBLE_ENUM;
typedef NSString* NSPasteboardReadingOptionKey NS_TYPED_ENUM;

APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeString;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypePDF;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeTIFF;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypePNG;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeRTF;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeRTFD;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeHTML;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeTabularText;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeFont;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeRuler;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeColor;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeSound;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeMultipleTextSelection;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeTextFinderOptions;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeURL;
APPKIT_EXTERN NSPasteboardType const NSPasteboardTypeFileURL;
APPKIT_EXTERN NSPasteboardName const NSPasteboardNameGeneral;
APPKIT_EXTERN NSPasteboardName const NSPasteboardNameFont;
APPKIT_EXTERN NSPasteboardName const NSPasteboardNameRuler;
APPKIT_EXTERN NSPasteboardName const NSPasteboardNameFind;
APPKIT_EXTERN NSPasteboardName const NSPasteboardNameDrag;

@class NSPasteboard, NSPasteboardItem;

@protocol NSPasteboardWriting <NSObject>
- (NSArray<NSPasteboardType>*)writableTypesForPasteboard:
    (NSPasteboard*)pasteboard;
- (nullable id)pasteboardPropertyListForType:(NSPasteboardType)type;
@end

@protocol NSPasteboardReading <NSObject>
@end

@interface NSString (NSStringPasteboardSupport) <NSPasteboardWriting,
                                                 NSPasteboardReading>
@end

@interface NSURL (NSPasteboardSupport) <NSPasteboardWriting,
                                        NSPasteboardReading>
+ (nullable NSURL*)URLFromPasteboard:(NSPasteboard*)pasteBoard;
- (void)writeToPasteboard:(NSPasteboard*)pasteBoard;
@end

@interface NSPasteboard : NSObject
@property(class, readonly, strong) NSPasteboard* generalPasteboard;
+ (NSPasteboard*)pasteboardWithName:(NSPasteboardName)name;
+ (NSPasteboard*)pasteboardWithUniqueName;
@property(readonly, strong) NSPasteboardName name;
@property(readonly) NSInteger changeCount;
- (void)releaseGlobally;
- (NSInteger)clearContents;
- (BOOL)writeObjects:(NSArray<id<NSPasteboardWriting>>*)objects;
- (nullable NSArray*)readObjectsForClasses:(NSArray<Class>*)classArray
                                   options:(nullable NSDictionary<
                                               NSPasteboardReadingOptionKey,
                                               id>*)options;
@property(nullable, readonly, copy) NSArray<NSPasteboardItem*>* pasteboardItems;
- (NSInteger)declareTypes:(NSArray<NSPasteboardType>*)newTypes
                    owner:(nullable id)newOwner;
- (NSInteger)addTypes:(NSArray<NSPasteboardType>*)newTypes
                owner:(nullable id)newOwner;
@property(nullable, readonly, copy) NSArray<NSPasteboardType>* types;
- (nullable NSPasteboardType)availableTypeFromArray:
    (NSArray<NSPasteboardType>*)types;
- (BOOL)setData:(nullable NSData*)data forType:(NSPasteboardType)dataType;
- (BOOL)setPropertyList:(id)plist forType:(NSPasteboardType)dataType;
- (BOOL)setString:(NSString*)string forType:(NSPasteboardType)dataType;
- (nullable NSData*)dataForType:(NSPasteboardType)dataType;
- (nullable id)propertyListForType:(NSPasteboardType)dataType;
- (nullable NSString*)stringForType:(NSPasteboardType)dataType;
@end

@interface NSPasteboardItem : NSObject <NSPasteboardWriting, NSPasteboardReading>
@property(readonly, copy) NSArray<NSPasteboardType>* types;
- (BOOL)setData:(NSData*)data forType:(NSPasteboardType)type;
- (BOOL)setString:(NSString*)string forType:(NSPasteboardType)type;
- (BOOL)setPropertyList:(id)propertyList forType:(NSPasteboardType)type;
- (nullable NSData*)dataForType:(NSPasteboardType)type;
- (nullable NSString*)stringForType:(NSPasteboardType)type;
- (nullable id)propertyListForType:(NSPasteboardType)type;
@end

// -- NSDragging.h, NSDraggingItem.h, NSDraggingSession.h ---------------------

typedef NS_OPTIONS(NSUInteger, NSDragOperation){
    NSDragOperationNone = 0,
    NSDragOperationCopy = 1,
    NSDragOperationLink = 2,
    NSDragOperationGeneric = 4,
    NSDragOperationPrivate = 8,
    NSDragOperationMove = 16,
    NSDragOperationDelete = 32,
    NSDragOperationEvery = NSUIntegerMax,
};

typedef NS_ENUM(NSInteger, NSDraggingFormation){
    NSDraggingFormationDefault = 0,
    NSDraggingFormationNone,
    NSDraggingFormationPile,
    NSDraggingFormationList,
    NSDraggingFormationStack,
};

typedef NS_ENUM(NSInteger, NSDraggingContext){
    NSDraggingContextOutsideApplication = 0,
    NSDraggingContextWithinApplication,
};

typedef NS_OPTIONS(NSUInteger, NSDraggingItemEnumerationOptions){
    NSDraggingItemEnumerationConcurrent = NSEnumerationConcurrent,
    NSDraggingItemEnumerationClearNonenumeratedImages = (1UL << 16),
};

typedef NS_ENUM(NSInteger, NSSpringLoadingHighlight){
    NSSpringLoadingHighlightNone = 0,
    NSSpringLoadingHighlightStandard,
    NSSpringLoadingHighlightEmphasized,
};

typedef NSString* NSDraggingImageComponentKey NS_TYPED_EXTENSIBLE_ENUM;
APPKIT_EXTERN NSDraggingImageComponentKey const NSDraggingImageComponentIconKey;
APPKIT_EXTERN NSDraggingImageComponentKey const NSDraggingImageComponentLabelKey;

@class NSDraggingItem, NSDraggingSession;

@protocol NSDraggingInfo <NSObject>
@property(nullable, readonly, assign) NSWindow* draggingDestinationWindow;
@property(readonly) NSDragOperation draggingSourceOperationMask;
@property(readonly) NSPoint draggingLocation;
@property(readonly) NSPoint draggedImageLocation;
@property(readonly, strong) NSPasteboard* draggingPasteboard;
@property(nullable, readonly, strong) id draggingSource;
@property(readonly) NSInteger draggingSequenceNumber;
@property NSDraggingFormation draggingFormation;
@property BOOL animatesToDestination;
@property NSInteger numberOfValidItemsForDrop;
@end

@protocol NSDraggingDestination <NSObject>
@optional
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender;
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender;
- (void)draggingExited:(nullable id<NSDraggingInfo>)sender;
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender;
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender;
- (void)concludeDragOperation:(nullable id<NSDraggingInfo>)sender;
- (void)draggingEnded:(id<NSDraggingInfo>)sender;
- (BOOL)wantsPeriodicDraggingUpdates;
- (void)updateDraggingItemsForDrag:(nullable id<NSDraggingInfo>)sender;
@end

@protocol NSDraggingSource <NSObject>
@required
- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context;
@optional
- (void)draggingSession:(NSDraggingSession*)session
       willBeginAtPoint:(NSPoint)screenPoint;
- (void)draggingSession:(NSDraggingSession*)session
           movedToPoint:(NSPoint)screenPoint;
- (void)draggingSession:(NSDraggingSession*)session
           endedAtPoint:(NSPoint)screenPoint
              operation:(NSDragOperation)operation;
- (BOOL)ignoreModifierKeysForDraggingSession:(NSDraggingSession*)session;
@end

@interface NSDraggingImageComponent : NSObject
+ (NSDraggingImageComponent*)draggingImageComponentWithKey:
    (NSDraggingImageComponentKey)key;
- (instancetype)initWithKey:(NSDraggingImageComponentKey)key;
@property(copy) NSDraggingImageComponentKey key;
@property(nullable, strong) id contents;
@property NSRect frame;
@end

@interface NSDraggingItem : NSObject
- (instancetype)initWithPasteboardWriter:(id<NSPasteboardWriting>)pasteboardWriter
    NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
@property(readonly, strong) id item;
@property NSRect draggingFrame;
@property(nullable, copy) NSArray<NSDraggingImageComponent*>* (^imageComponentsProvider)(void);
- (void)setDraggingFrame:(NSRect)frame contents:(nullable id)contents;
@property(nullable, readonly, copy) NSArray<NSDraggingImageComponent*>* imageComponents;
@end

@interface NSDraggingSession : NSObject
@property NSDraggingFormation draggingFormation;
@property BOOL animatesToStartingPositionsOnCancelOrFail;
@property NSInteger draggingLeaderIndex;
@property(readonly) NSPasteboard* draggingPasteboard;
@property(readonly) NSInteger draggingSequenceNumber;
@property(readonly) NSPoint draggingLocation;
@end

// -- NSView.h ----------------------------------------------------------------

typedef NS_OPTIONS(NSUInteger, NSAutoresizingMaskOptions){
    NSViewNotSizable = 0,     NSViewMinXMargin = 1,
    NSViewWidthSizable = 2,   NSViewMaxXMargin = 4,
    NSViewMinYMargin = 8,     NSViewHeightSizable = 16,
    NSViewMaxYMargin = 32,
};

typedef NS_ENUM(NSUInteger, NSBorderType){
    NSNoBorder = 0,
    NSLineBorder = 1,
    NSBezelBorder = 2,
    NSGrooveBorder = 3,
};

typedef NS_ENUM(NSInteger, NSViewLayerContentsRedrawPolicy){
    NSViewLayerContentsRedrawNever = 0,
    NSViewLayerContentsRedrawOnSetNeedsDisplay = 1,
    NSViewLayerContentsRedrawDuringViewResize = 2,
    NSViewLayerContentsRedrawBeforeViewResize = 3,
    NSViewLayerContentsRedrawCrossfade = 4,
};

typedef NS_ENUM(NSInteger, NSViewLayerContentsPlacement){
    NSViewLayerContentsPlacementScaleAxesIndependently = 0,
    NSViewLayerContentsPlacementScaleProportionallyToFit = 1,
    NSViewLayerContentsPlacementScaleProportionallyToFill = 2,
    NSViewLayerContentsPlacementCenter = 3,
    NSViewLayerContentsPlacementTop = 4,
    NSViewLayerContentsPlacementTopRight = 5,
    NSViewLayerContentsPlacementRight = 6,
    NSViewLayerContentsPlacementBottomRight = 7,
    NSViewLayerContentsPlacementBottom = 8,
    NSViewLayerContentsPlacementBottomLeft = 9,
    NSViewLayerContentsPlacementLeft = 10,
    NSViewLayerContentsPlacementTopLeft = 11,
};

typedef NSInteger NSTrackingRectTag;
typedef NSInteger NSToolTipTag;

@class NSScrollView, NSLayoutConstraint, NSShadow, NSBitmapImageRep;

@interface NSView
    : NSResponder <NSAppearanceCustomization, NSDraggingDestination,
                   NSUserInterfaceItemIdentification>
- (instancetype)initWithFrame:(NSRect)frameRect NS_DESIGNATED_INITIALIZER;
- (nullable instancetype)initWithCoder:(NSCoder*)coder
    NS_DESIGNATED_INITIALIZER;
@property(nullable, readonly, assign) NSWindow* window;
@property(nullable, readonly, assign) NSView* superview;
@property(copy) NSArray<__kindof NSView*>* subviews;
- (BOOL)isDescendantOf:(NSView*)view;
- (nullable NSView*)ancestorSharedWithView:(NSView*)view;
@property(nullable, readonly, assign) NSView* opaqueAncestor;
@property(getter=isHidden) BOOL hidden;
@property(readonly, getter=isHiddenOrHasHiddenAncestor)
    BOOL hiddenOrHasHiddenAncestor;
- (void)getRectsBeingDrawn:(const NSRect* _Nullable* _Nullable)rects
                     count:(nullable NSInteger*)count;
- (BOOL)needsToDrawRect:(NSRect)rect;
@property(readonly) BOOL wantsDefaultClipping;
- (void)viewDidHide;
- (void)viewDidUnhide;
- (void)addSubview:(NSView*)view;
- (void)addSubview:(NSView*)view
        positioned:(NSWindowOrderingMode)place
        relativeTo:(nullable NSView*)otherView;
- (void)sortSubviewsUsingFunction:(NSComparisonResult (*)(__kindof NSView*,
                                                          __kindof NSView*,
                                                          void* _Nullable))
                                      compare
                          context:(nullable void*)context;
- (void)viewWillMoveToWindow:(nullable NSWindow*)newWindow;
- (void)viewDidMoveToWindow;
- (void)viewWillMoveToSuperview:(nullable NSView*)newSuperview;
- (void)viewDidMoveToSuperview;
- (void)didAddSubview:(NSView*)subview;
- (void)willRemoveSubview:(NSView*)subview;
- (void)removeFromSuperview;
- (void)replaceSubview:(NSView*)oldView with:(NSView*)newView;
- (void)removeFromSuperviewWithoutNeedingDisplay;
- (void)viewDidChangeBackingProperties;
@property BOOL postsFrameChangedNotifications;
- (void)resizeSubviewsWithOldSize:(NSSize)oldSize;
- (void)resizeWithOldSuperviewSize:(NSSize)oldSize;
@property BOOL autoresizesSubviews;
@property NSAutoresizingMaskOptions autoresizingMask;
- (void)setFrameOrigin:(NSPoint)newOrigin;
- (void)setFrameSize:(NSSize)newSize;
@property NSRect frame;
@property CGFloat frameRotation;
@property CGFloat frameCenterRotation;
- (void)setBoundsOrigin:(NSPoint)newOrigin;
- (void)setBoundsSize:(NSSize)newSize;
@property CGFloat boundsRotation;
- (void)translateOriginToPoint:(NSPoint)translation;
- (void)scaleUnitSquareToSize:(NSSize)newUnitSize;
- (void)rotateByAngle:(CGFloat)angle;
@property NSRect bounds;
@property(readonly, getter=isFlipped) BOOL flipped;
@property(readonly, getter=isRotatedFromBase) BOOL rotatedFromBase;
@property(readonly, getter=isRotatedOrScaledFromBase)
    BOOL rotatedOrScaledFromBase;
@property(readonly, getter=isOpaque) BOOL opaque;
- (NSPoint)convertPoint:(NSPoint)point fromView:(nullable NSView*)view;
- (NSPoint)convertPoint:(NSPoint)point toView:(nullable NSView*)view;
- (NSSize)convertSize:(NSSize)size fromView:(nullable NSView*)view;
- (NSSize)convertSize:(NSSize)size toView:(nullable NSView*)view;
- (NSRect)convertRect:(NSRect)rect fromView:(nullable NSView*)view;
- (NSRect)convertRect:(NSRect)rect toView:(nullable NSView*)view;
- (NSRect)backingAlignedRect:(NSRect)rect options:(NSAlignmentOptions)options;
- (NSRect)centerScanRect:(NSRect)rect;
- (NSPoint)convertPointToBacking:(NSPoint)point;
- (NSPoint)convertPointFromBacking:(NSPoint)point;
- (NSSize)convertSizeToBacking:(NSSize)size;
- (NSSize)convertSizeFromBacking:(NSSize)size;
- (NSRect)convertRectToBacking:(NSRect)rect;
- (NSRect)convertRectFromBacking:(NSRect)rect;
- (NSPoint)convertPointToLayer:(NSPoint)point;
- (NSPoint)convertPointFromLayer:(NSPoint)point;
- (NSRect)convertRectToLayer:(NSRect)rect;
- (NSRect)convertRectFromLayer:(NSRect)rect;
@property(readonly) BOOL canDrawConcurrently;
@property(readonly) BOOL canDraw;
- (void)setNeedsDisplayInRect:(NSRect)invalidRect;
@property BOOL needsDisplay;
- (void)lockFocus;
- (void)unlockFocus;
- (BOOL)lockFocusIfCanDraw;
@property(class, readonly, nullable) NSView* focusView;
@property(readonly) NSRect visibleRect;
- (void)display;
- (void)displayIfNeeded;
- (void)displayIfNeededIgnoringOpacity;
- (void)displayRect:(NSRect)rect;
- (void)displayIfNeededInRect:(NSRect)rect;
- (void)drawRect:(NSRect)dirtyRect;
- (void)viewWillDraw;
- (void)scrollPoint:(NSPoint)point;
- (BOOL)scrollRectToVisible:(NSRect)rect;
- (BOOL)autoscroll:(NSEvent*)event;
- (NSRect)adjustScroll:(NSRect)newVisible;
- (nullable NSView*)hitTest:(NSPoint)point;
- (BOOL)mouse:(NSPoint)point inRect:(NSRect)rect;
- (nullable __kindof NSView*)viewWithTag:(NSInteger)tag;
@property(readonly) NSInteger tag;
- (BOOL)performKeyEquivalent:(NSEvent*)event;
- (BOOL)acceptsFirstMouse:(nullable NSEvent*)event;
- (BOOL)shouldDelayWindowOrderingForEvent:(NSEvent*)event;
@property(readonly) BOOL needsPanelToBecomeKey;
@property(readonly) BOOL mouseDownCanMoveWindow;
@property BOOL acceptsTouchEvents;
@property BOOL wantsRestingTouches;
- (void)addCursorRect:(NSRect)rect cursor:(NSCursor*)object;
- (void)removeCursorRect:(NSRect)rect cursor:(NSCursor*)object;
- (void)discardCursorRects;
- (void)resetCursorRects;
- (NSTrackingRectTag)addTrackingRect:(NSRect)rect
                               owner:(id)owner
                            userData:(nullable void*)data
                        assumeInside:(BOOL)flag;
- (void)removeTrackingRect:(NSTrackingRectTag)tag;
@property(nullable, strong) CALayer* layer;
- (CALayer*)makeBackingLayer;
@property NSViewLayerContentsRedrawPolicy layerContentsRedrawPolicy;
@property NSViewLayerContentsPlacement layerContentsPlacement;
@property BOOL wantsLayer;
@property(readonly) BOOL wantsUpdateLayer;
- (void)updateLayer;
@property BOOL canDrawSubviewsIntoLayer;
@property(nullable, copy) NSArray* backgroundFilters;
@property(nullable, strong) id compositingFilter;
@property(nullable, copy) NSArray* contentFilters;
@property(nullable, copy) NSShadow* shadow;
@property CGFloat alphaValue;
@property BOOL clipsToBounds;
@property(nullable, copy) NSString* toolTip;
- (NSToolTipTag)addToolTipRect:(NSRect)rect
                         owner:(id)owner
                      userData:(nullable void*)data;
- (void)removeToolTip:(NSToolTipTag)tag;
- (void)removeAllToolTips;
- (void)addTrackingArea:(NSTrackingArea*)trackingArea;
- (void)removeTrackingArea:(NSTrackingArea*)trackingArea;
@property(readonly, copy) NSArray<NSTrackingArea*>* trackingAreas;
- (void)updateTrackingAreas;
@property(readonly) BOOL inLiveResize;
@property(readonly) BOOL preservesContentDuringLiveResize;
- (void)viewWillStartLiveResize;
- (void)viewDidEndLiveResize;
- (NSDraggingSession*)beginDraggingSessionWithItems:
                          (NSArray<NSDraggingItem*>*)items
                                              event:(NSEvent*)event
                                             source:(id<NSDraggingSource>)
                                                        source;
@property(readonly, copy) NSArray<NSPasteboardType>* registeredDraggedTypes;
- (void)registerForDraggedTypes:(NSArray<NSPasteboardType>*)newTypes;
- (void)unregisterDraggedTypes;
@property(nullable, readonly, strong) NSScrollView* enclosingScrollView;
@property(nullable, assign) NSView* nextKeyView;
@property(nullable, readonly, assign) NSView* previousKeyView;
@property(nullable, readonly, assign) NSView* nextValidKeyView;
@property(nullable, readonly, assign) NSView* previousValidKeyView;
@property(readonly) BOOL canBecomeKeyView;
@property(readonly) NSFocusRingType focusRingType;
@property(readonly) NSRect focusRingMaskBounds;
- (void)drawFocusRingMask;
- (void)noteFocusRingMaskChanged;
@property BOOL translatesAutoresizingMaskIntoConstraints;
@property BOOL needsLayout;
- (void)layout;
- (void)layoutSubtreeIfNeeded;
@property(readonly) NSEdgeInsets safeAreaInsets;
@property(readonly) NSRect safeAreaRect;
@property NSUserInterfaceLayoutDirection userInterfaceLayoutDirection;
- (void)viewDidChangeEffectiveAppearance;
- (nullable NSMenu*)menuForEvent:(NSEvent*)event;
+ (nullable NSMenu*)defaultMenu;
- (void)willOpenMenu:(NSMenu*)menu withEvent:(NSEvent*)event;
- (void)didCloseMenu:(NSMenu*)menu withEvent:(nullable NSEvent*)event;
- (nullable NSBitmapImageRep*)bitmapImageRepForCachingDisplayInRect:
    (NSRect)rect;
- (void)cacheDisplayInRect:(NSRect)rect
          toBitmapImageRep:(NSBitmapImageRep*)bitmapImageRep;
@end

@protocol NSViewToolTipOwner <NSObject>
- (NSString*)view:(NSView*)view
    stringForToolTip:(NSToolTipTag)tag
               point:(NSPoint)point
            userData:(nullable void*)data;
@end

APPKIT_EXTERN NSNotificationName const NSViewFrameDidChangeNotification;
APPKIT_EXTERN NSNotificationName const NSViewBoundsDidChangeNotification;
APPKIT_EXTERN NSNotificationName const NSViewGlobalFrameDidChangeNotification;
APPKIT_EXTERN NSNotificationName const NSViewDidUpdateTrackingAreasNotification;

// -- NSTextInputClient.h -----------------------------------------------------

typedef NSString* NSTextInputSourceIdentifier NS_SWIFT_BRIDGED_TYPEDEF;

@protocol NSTextInputClient
@required
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange;
- (void)doCommandBySelector:(SEL)selector;
- (void)setMarkedText:(id)string
        selectedRange:(NSRange)selectedRange
     replacementRange:(NSRange)replacementRange;
- (void)unmarkText;
- (NSRange)selectedRange;
- (NSRange)markedRange;
- (BOOL)hasMarkedText;
- (nullable NSAttributedString*)
    attributedSubstringForProposedRange:(NSRange)range
                            actualRange:(nullable NSRangePointer)actualRange;
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText;
- (NSRect)firstRectForCharacterRange:(NSRange)range
                         actualRange:(nullable NSRangePointer)actualRange;
- (NSUInteger)characterIndexForPoint:(NSPoint)point;
@optional
- (NSAttributedString*)attributedString;
- (CGFloat)fractionOfDistanceThroughGlyphForPoint:(NSPoint)point;
- (CGFloat)baselineDeltaForCharacterAtIndex:(NSUInteger)anIndex;
- (NSInteger)windowLevel;
- (BOOL)drawsVerticallyForCharacterAtIndex:(NSUInteger)charIndex;
- (NSRect)unionRectInVisibleSelectedRange;
- (NSRect)documentVisibleRect;
@end

// -- NSAnimation.h, NSAnimationContext.h -------------------------------------

typedef NS_ENUM(NSUInteger, NSAnimationCurve){
    NSAnimationEaseInOut,
    NSAnimationEaseIn,
    NSAnimationEaseOut,
    NSAnimationLinear,
};

typedef NS_ENUM(NSUInteger, NSAnimationBlockingMode){
    NSAnimationBlocking,
    NSAnimationNonblocking,
    NSAnimationNonblockingThreaded,
};

typedef float NSAnimationProgress;

@protocol NSAnimationDelegate;

@interface NSAnimation : NSObject <NSCopying, NSCoding>
- (instancetype)initWithDuration:(NSTimeInterval)duration
                  animationCurve:(NSAnimationCurve)animationCurve
    NS_DESIGNATED_INITIALIZER;
- (void)startAnimation;
- (void)stopAnimation;
@property(readonly, getter=isAnimating) BOOL animating;
@property NSAnimationProgress currentProgress;
@property NSTimeInterval duration;
@property NSAnimationBlockingMode animationBlockingMode;
@property float frameRate;
@property NSAnimationCurve animationCurve;
@property(readonly) float currentValue;
@property(nullable, weak) id<NSAnimationDelegate> delegate;
@property(copy) NSArray<NSNumber*>* progressMarks;
- (void)addProgressMark:(NSAnimationProgress)progressMark;
- (void)removeProgressMark:(NSAnimationProgress)progressMark;
@property(nullable, copy) NSArray<NSRunLoopMode>* runLoopModesForAnimating;
@end

@protocol NSAnimationDelegate <NSObject>
@optional
- (BOOL)animationShouldStart:(NSAnimation*)animation;
- (void)animationDidStop:(NSAnimation*)animation;
- (void)animationDidEnd:(NSAnimation*)animation;
- (float)animation:(NSAnimation*)animation
    valueForProgress:(NSAnimationProgress)progress;
- (void)animation:(NSAnimation*)animation
    didReachProgressMark:(NSAnimationProgress)progress;
@end

@class CAMediaTimingFunction;

@interface NSAnimationContext : NSObject
+ (void)runAnimationGroup:(void(NS_NOESCAPE ^)(NSAnimationContext* context))
                              changes
        completionHandler:(nullable void (^)(void))completionHandler;
+ (void)runAnimationGroup:(void(NS_NOESCAPE ^)(NSAnimationContext* context))
                              changes;
+ (void)beginGrouping;
+ (void)endGrouping;
@property(class, readonly, strong) NSAnimationContext* currentContext;
@property NSTimeInterval duration;
@property(nullable, strong) CAMediaTimingFunction* timingFunction;
@property(nullable, copy) void (^completionHandler)(void);
@property BOOL allowsImplicitAnimation;
@end

@protocol NSAnimatablePropertyContainer
- (instancetype)animator;
@end

@interface NSView (NSAnimatablePropertyContainer) <NSAnimatablePropertyContainer>
@end

// -- NSScreen.h --------------------------------------------------------------

@interface NSScreen : NSObject
@property(class, readonly, copy) NSArray<NSScreen*>* screens;
@property(class, readonly, nullable, strong) NSScreen* mainScreen;
@property(class, readonly, nullable, strong) NSScreen* deepestScreen;
@property(class, readonly) BOOL screensHaveSeparateSpaces;
@property(readonly) NSInteger depth;
@property(readonly) NSRect frame;
@property(readonly) NSRect visibleFrame;
@property(readonly, copy) NSDictionary<NSDeviceDescriptionKey, id>*
    deviceDescription;
@property(nullable, readonly, strong) NSColorSpace* colorSpace;
@property(readonly) CGFloat backingScaleFactor;
@property(readonly, copy) NSString* localizedName;
@property(readonly) NSEdgeInsets safeAreaInsets;
@property(readonly) NSRect auxiliaryTopLeftArea;
@property(readonly) NSRect auxiliaryTopRightArea;
@property(readonly) CGFloat maximumExtendedDynamicRangeColorComponentValue;
@property(readonly) CGFloat maximumPotentialExtendedDynamicRangeColorComponentValue;
@property(readonly) CGFloat maximumReferenceExtendedDynamicRangeColorComponentValue;
@property(readonly) NSInteger maximumFramesPerSecond;
@property(readonly) NSTimeInterval minimumRefreshInterval;
@property(readonly) NSTimeInterval maximumRefreshInterval;
- (BOOL)canRepresentDisplayGamut:(NSDisplayGamut)displayGamut;
- (NSRect)convertRectToBacking:(NSRect)rect;
- (NSRect)convertRectFromBacking:(NSRect)rect;
- (NSRect)backingAlignedRect:(NSRect)rect options:(NSAlignmentOptions)options;
@end

APPKIT_EXTERN NSNotificationName const NSScreenColorSpaceDidChangeNotification;

// -- NSWindow.h --------------------------------------------------------------

typedef NS_OPTIONS(NSUInteger, NSWindowStyleMask){
    NSWindowStyleMaskBorderless = 0,
    NSWindowStyleMaskTitled = 1 << 0,
    NSWindowStyleMaskClosable = 1 << 1,
    NSWindowStyleMaskMiniaturizable = 1 << 2,
    NSWindowStyleMaskResizable = 1 << 3,
    NSWindowStyleMaskTexturedBackground = 1 << 8,
    NSWindowStyleMaskUnifiedTitleAndToolbar = 1 << 12,
    NSWindowStyleMaskFullScreen = 1 << 14,
    NSWindowStyleMaskFullSizeContentView = 1 << 15,
    NSWindowStyleMaskUtilityWindow = 1 << 4,
    NSWindowStyleMaskDocModalWindow = 1 << 6,
    NSWindowStyleMaskNonactivatingPanel = 1 << 7,
    NSWindowStyleMaskHUDWindow = 1 << 13,
};

static const NSInteger NSDisplayWindowRunLoopOrdering = 600000;
static const NSInteger NSResetCursorRectsRunLoopOrdering = 700000;

typedef NS_ENUM(NSUInteger, NSWindowSharingType){
    NSWindowSharingNone = 0,
    NSWindowSharingReadOnly = 1,
};

typedef NS_OPTIONS(NSUInteger, NSWindowCollectionBehavior){
    NSWindowCollectionBehaviorDefault = 0,
    NSWindowCollectionBehaviorCanJoinAllSpaces = 1 << 0,
    NSWindowCollectionBehaviorMoveToActiveSpace = 1 << 1,
    NSWindowCollectionBehaviorManaged = 1 << 2,
    NSWindowCollectionBehaviorTransient = 1 << 3,
    NSWindowCollectionBehaviorStationary = 1 << 4,
    NSWindowCollectionBehaviorParticipatesInCycle = 1 << 5,
    NSWindowCollectionBehaviorIgnoresCycle = 1 << 6,
    NSWindowCollectionBehaviorFullScreenPrimary = 1 << 7,
    NSWindowCollectionBehaviorFullScreenAuxiliary = 1 << 8,
    NSWindowCollectionBehaviorFullScreenNone = 1 << 9,
    NSWindowCollectionBehaviorFullScreenAllowsTiling = 1 << 11,
    NSWindowCollectionBehaviorFullScreenDisallowsTiling = 1 << 12,
    NSWindowCollectionBehaviorPrimary = 1 << 16,
    NSWindowCollectionBehaviorAuxiliary = 1 << 17,
    NSWindowCollectionBehaviorCanJoinAllApplications = 1 << 18,
};

typedef NS_ENUM(NSInteger, NSWindowAnimationBehavior){
    NSWindowAnimationBehaviorDefault = 0,
    NSWindowAnimationBehaviorNone = 2,
    NSWindowAnimationBehaviorDocumentWindow = 3,
    NSWindowAnimationBehaviorUtilityWindow = 4,
    NSWindowAnimationBehaviorAlertPanel = 5,
};

typedef NS_OPTIONS(NSUInteger, NSWindowNumberListOptions){
    NSWindowNumberListAllApplications = 1 << 0,
    NSWindowNumberListAllSpaces = 1 << 4,
};

typedef NS_OPTIONS(NSUInteger, NSWindowOcclusionState){
    NSWindowOcclusionStateVisible = 1UL << 1,
};

typedef NSInteger NSWindowLevel;
static const NSWindowLevel NSNormalWindowLevel = kCGNormalWindowLevel;
static const NSWindowLevel NSFloatingWindowLevel = kCGFloatingWindowLevel;
static const NSWindowLevel NSSubmenuWindowLevel = kCGTornOffMenuWindowLevel;
static const NSWindowLevel NSTornOffMenuWindowLevel =
    kCGTornOffMenuWindowLevel;
static const NSWindowLevel NSMainMenuWindowLevel = kCGMainMenuWindowLevel;
static const NSWindowLevel NSStatusWindowLevel = kCGStatusWindowLevel;
static const NSWindowLevel NSModalPanelWindowLevel = kCGModalPanelWindowLevel;
static const NSWindowLevel NSPopUpMenuWindowLevel = kCGPopUpMenuWindowLevel;
static const NSWindowLevel NSScreenSaverWindowLevel =
    kCGScreenSaverWindowLevel;

typedef NS_ENUM(NSUInteger, NSSelectionDirection){
    NSDirectSelection = 0,
    NSSelectingNext,
    NSSelectingPrevious,
};

typedef NS_ENUM(NSInteger, NSWindowButton){
    NSWindowCloseButton,
    NSWindowMiniaturizeButton,
    NSWindowZoomButton,
    NSWindowToolbarButton,
    NSWindowDocumentIconButton,
    NSWindowDocumentVersionsButton = 6,
};

typedef NS_ENUM(NSInteger, NSWindowTitleVisibility){
    NSWindowTitleVisible = 0,
    NSWindowTitleHidden = 1,
};

typedef NS_ENUM(NSInteger, NSWindowToolbarStyle){
    NSWindowToolbarStyleAutomatic,
    NSWindowToolbarStyleExpanded,
    NSWindowToolbarStylePreference,
    NSWindowToolbarStyleUnified,
    NSWindowToolbarStyleUnifiedCompact,
};

typedef NS_ENUM(NSInteger, NSWindowTabbingMode){
    NSWindowTabbingModeAutomatic,
    NSWindowTabbingModePreferred,
    NSWindowTabbingModeDisallowed,
};

typedef NS_ENUM(NSInteger, NSWindowUserTabbingPreference){
    NSWindowUserTabbingPreferenceManual,
    NSWindowUserTabbingPreferenceAlways,
    NSWindowUserTabbingPreferenceInFullScreen,
};

typedef NS_ENUM(NSUInteger, NSBackingStoreType){
    NSBackingStoreRetained = 0,
    NSBackingStoreNonretained = 1,
    NSBackingStoreBuffered = 2,
};

typedef int NSWindowDepth;

typedef NS_OPTIONS(NSUInteger, NSApplicationPresentationOptions){
    NSApplicationPresentationDefault = 0,
    NSApplicationPresentationAutoHideDock = (1 << 0),
    NSApplicationPresentationHideDock = (1 << 1),
    NSApplicationPresentationAutoHideMenuBar = (1 << 2),
    NSApplicationPresentationHideMenuBar = (1 << 3),
    NSApplicationPresentationDisableAppleMenu = (1 << 4),
    NSApplicationPresentationDisableProcessSwitching = (1 << 5),
    NSApplicationPresentationDisableForceQuit = (1 << 6),
    NSApplicationPresentationDisableSessionTermination = (1 << 7),
    NSApplicationPresentationDisableHideApplication = (1 << 8),
    NSApplicationPresentationDisableMenuBarTransparency = (1 << 9),
    NSApplicationPresentationFullScreen = (1 << 10),
    NSApplicationPresentationAutoHideToolbar = (1 << 11),
    NSApplicationPresentationDisableCursorLocationAssistance = (1 << 12),
};

typedef NSInteger NSModalResponse;
static const NSModalResponse NSModalResponseOK = 1;
static const NSModalResponse NSModalResponseCancel = 0;

typedef NSString* NSWindowFrameAutosaveName NS_SWIFT_BRIDGED_TYPEDEF;
typedef NSString* NSWindowPersistableFrameDescriptor NS_SWIFT_BRIDGED_TYPEDEF;
typedef NSString* NSWindowTabbingIdentifier NS_SWIFT_BRIDGED_TYPEDEF;

@class NSButton, NSToolbar, NSWindowController, NSTitlebarAccessoryViewController,
    NSWindowTab, NSWindowTabGroup, NSViewController, NSDockTile, NSText;
@protocol NSWindowDelegate;

@interface NSWindow
    : NSResponder <NSAnimatablePropertyContainer, NSUserInterfaceValidations,
                   NSUserInterfaceItemIdentification,
                   NSAppearanceCustomization>
+ (NSRect)frameRectForContentRect:(NSRect)cRect
                        styleMask:(NSWindowStyleMask)style;
+ (NSRect)contentRectForFrameRect:(NSRect)fRect
                        styleMask:(NSWindowStyleMask)style;
+ (CGFloat)minFrameWidthWithTitle:(NSString*)title
                        styleMask:(NSWindowStyleMask)style;
@property(class, readonly) NSWindowDepth defaultDepthLimit;
- (NSRect)frameRectForContentRect:(NSRect)contentRect;
- (NSRect)contentRectForFrameRect:(NSRect)frameRect;
- (instancetype)initWithContentRect:(NSRect)contentRect
                          styleMask:(NSWindowStyleMask)style
                            backing:(NSBackingStoreType)backingStoreType
                              defer:(BOOL)flag NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithContentRect:(NSRect)contentRect
                          styleMask:(NSWindowStyleMask)style
                            backing:(NSBackingStoreType)backingStoreType
                              defer:(BOOL)flag
                             screen:(nullable NSScreen*)screen;
@property(copy) NSString* title;
@property(copy) NSString* subtitle;
@property NSWindowTitleVisibility titleVisibility;
@property(readonly) NSUserInterfaceLayoutDirection
    windowTitlebarLayoutDirection;
@property BOOL titlebarAppearsTransparent;
@property NSWindowToolbarStyle toolbarStyle;
@property(readonly) NSRect contentLayoutRect;
@property(copy) NSArray<__kindof NSTitlebarAccessoryViewController*>*
    titlebarAccessoryViewControllers;
- (void)addTitlebarAccessoryViewController:
    (NSTitlebarAccessoryViewController*)childViewController;
- (void)insertTitlebarAccessoryViewController:
            (NSTitlebarAccessoryViewController*)childViewController
                                      atIndex:(NSInteger)index;
- (void)removeTitlebarAccessoryViewControllerAtIndex:(NSInteger)index;
@property(nullable, copy) NSURL* representedURL;
@property(copy) NSString* representedFilename;
@property(getter=isExcludedFromWindowsMenu) BOOL excludedFromWindowsMenu;
@property(nullable, strong) __kindof NSView* contentView;
@property(nullable, weak) id<NSWindowDelegate> delegate;
@property(readonly) NSInteger windowNumber;
@property NSWindowStyleMask styleMask;
- (nullable NSText*)fieldEditor:(BOOL)createFlag
                      forObject:(nullable id)object;
- (void)endEditingFor:(nullable id)object;
- (NSRect)constrainFrameRect:(NSRect)frameRect
                    toScreen:(nullable NSScreen*)screen;
- (void)setFrame:(NSRect)frameRect display:(BOOL)flag;
- (void)setContentSize:(NSSize)size;
- (void)setFrameOrigin:(NSPoint)point;
- (void)setFrameTopLeftPoint:(NSPoint)point;
- (NSPoint)cascadeTopLeftFromPoint:(NSPoint)topLeftPoint;
@property(readonly) NSRect frame;
- (NSTimeInterval)animationResizeTime:(NSRect)newFrame;
- (void)setFrame:(NSRect)frameRect display:(BOOL)displayFlag
         animate:(BOOL)animateFlag;
@property(readonly) BOOL inLiveResize;
@property NSSize resizeIncrements;
@property NSSize aspectRatio;
@property NSSize contentResizeIncrements;
@property NSSize contentAspectRatio;
@property BOOL viewsNeedDisplay;
- (void)displayIfNeeded;
- (void)display;
@property BOOL preservesContentDuringLiveResize;
- (void)update;
- (BOOL)makeFirstResponder:(nullable NSResponder*)responder;
@property(nullable, readonly, weak) NSResponder* firstResponder;
@property(readonly) NSEventModifierFlags resizeFlags;
- (void)close;
@property(getter=isReleasedWhenClosed) BOOL releasedWhenClosed;
- (void)miniaturize:(nullable id)sender;
- (void)deminiaturize:(nullable id)sender;
@property(readonly, getter=isZoomed) BOOL zoomed;
- (void)zoom:(nullable id)sender;
@property(readonly, getter=isMiniaturized) BOOL miniaturized;
- (BOOL)tryToPerform:(SEL)action with:(nullable id)object;
- (nullable id)validRequestorForSendType:(nullable NSPasteboardType)sendType
                              returnType:(nullable NSPasteboardType)returnType;
@property(nullable, copy) NSColor* backgroundColor;
- (void)setContentBorderThickness:(CGFloat)thickness forEdge:(NSRectEdge)edge;
- (CGFloat)contentBorderThicknessForEdge:(NSRectEdge)edge;
@property(getter=isMovable) BOOL movable;
@property(getter=isMovableByWindowBackground) BOOL movableByWindowBackground;
@property BOOL hidesOnDeactivate;
@property BOOL canHide;
- (void)center;
- (void)makeKeyAndOrderFront:(nullable id)sender;
- (void)orderFront:(nullable id)sender;
- (void)orderBack:(nullable id)sender;
- (void)orderOut:(nullable id)sender;
- (void)orderWindow:(NSWindowOrderingMode)place
         relativeTo:(NSInteger)otherWin;
- (void)orderFrontRegardless;
@property(nullable, strong) NSImage* miniwindowImage;
@property(nullable, copy) NSString* miniwindowTitle;
@property(readonly, strong) NSDockTile* dockTile;
@property(getter=isDocumentEdited) BOOL documentEdited;
@property(readonly, getter=isVisible) BOOL visible;
@property(readonly, getter=isKeyWindow) BOOL keyWindow;
@property(readonly, getter=isMainWindow) BOOL mainWindow;
@property(readonly) BOOL canBecomeKeyWindow;
@property(readonly) BOOL canBecomeMainWindow;
- (void)makeKeyWindow;
- (void)makeMainWindow;
- (void)becomeKeyWindow;
- (void)resignKeyWindow;
- (void)becomeMainWindow;
- (void)resignMainWindow;
@property(readonly) BOOL worksWhenModal;
@property BOOL preventsApplicationTerminationWhenModal;
- (NSRect)convertRectToScreen:(NSRect)rect;
- (NSRect)convertRectFromScreen:(NSRect)rect;
- (NSPoint)convertPointToScreen:(NSPoint)point;
- (NSPoint)convertPointFromScreen:(NSPoint)point;
- (NSRect)convertRectToBacking:(NSRect)rect;
- (NSRect)convertRectFromBacking:(NSRect)rect;
- (NSPoint)convertPointToBacking:(NSPoint)point;
- (NSPoint)convertPointFromBacking:(NSPoint)point;
- (NSRect)backingAlignedRect:(NSRect)rect options:(NSAlignmentOptions)options;
@property(readonly) CGFloat backingScaleFactor;
- (void)performClose:(nullable id)sender;
- (void)performMiniaturize:(nullable id)sender;
- (void)performZoom:(nullable id)sender;
@property(getter=isOpaque) BOOL opaque;
@property BOOL hasShadow;
- (void)invalidateShadow;
@property CGFloat alphaValue;
@property BOOL displaysWhenScreenProfileChanges;
@property NSWindowSharingType sharingType;
@property BOOL allowsConcurrentViewDrawing;
@property NSWindowLevel level;
@property NSWindowDepth depthLimit;
@property BOOL hasDynamicDepthLimit;
@property(nullable, readonly, strong) NSScreen* screen;
@property(nullable, readonly, strong) NSScreen* deepestScreen;
@property(readonly, getter=isOnActiveSpace) BOOL onActiveSpace;
@property NSWindowCollectionBehavior collectionBehavior;
@property NSWindowAnimationBehavior animationBehavior;
@property(readonly) NSWindowOcclusionState occlusionState;
@property BOOL ignoresMouseEvents;
@property BOOL acceptsMouseMovedEvents;
@property(readonly) NSPoint mouseLocationOutsideOfEventStream;
- (void)disableCursorRects;
- (void)enableCursorRects;
- (void)discardCursorRects;
- (void)invalidateCursorRectsForView:(NSView*)view;
- (void)resetCursorRects;
- (void)sendEvent:(NSEvent*)event;
- (nullable NSEvent*)nextEventMatchingMask:(NSEventMask)mask;
- (nullable NSEvent*)nextEventMatchingMask:(NSEventMask)mask
                                 untilDate:(nullable NSDate*)expiration
                                    inMode:(NSRunLoopMode)mode
                                   dequeue:(BOOL)deqFlag;
- (void)discardEventsMatchingMask:(NSEventMask)mask
                      beforeEvent:(nullable NSEvent*)lastEvent;
- (void)postEvent:(NSEvent*)event atStart:(BOOL)flag;
@property(nullable, readonly, strong) NSEvent* currentEvent;
- (void)trackEventsMatchingMask:(NSEventMask)mask
                        timeout:(NSTimeInterval)timeout
                           mode:(NSRunLoopMode)mode
                        handler:(void(NS_NOESCAPE ^)(NSEvent* _Nullable event,
                                                     BOOL* stop))
                                    trackingHandler;
- (void)addChildWindow:(NSWindow*)childWin ordered:(NSWindowOrderingMode)place;
- (void)removeChildWindow:(NSWindow*)childWin;
@property(nullable, readonly, copy) NSArray<__kindof NSWindow*>* childWindows;
@property(nullable, weak) NSWindow* parentWindow;
@property(nullable, readonly, weak) __kindof NSWindow* sheetParent;
@property(readonly, getter=isSheet) BOOL sheet;
@property(nullable, readonly, strong) NSWindow* attachedSheet;
@property(readonly, copy) NSArray<__kindof NSWindow*>* sheets;
- (void)beginSheet:(NSWindow*)sheetWindow
    completionHandler:(void (^_Nullable)(NSModalResponse returnCode))handler;
- (void)beginCriticalSheet:(NSWindow*)sheetWindow
         completionHandler:
             (void (^_Nullable)(NSModalResponse returnCode))handler;
- (void)endSheet:(NSWindow*)sheetWindow;
- (void)endSheet:(NSWindow*)sheetWindow returnCode:(NSModalResponse)returnCode;
- (nullable NSButton*)standardWindowButton:(NSWindowButton)b;
+ (nullable NSButton*)standardWindowButton:(NSWindowButton)b
                              forStyleMask:(NSWindowStyleMask)styleMask;
@property BOOL showsToolbarButton;
@property(nullable, strong) NSToolbar* toolbar;
- (void)toggleToolbarShown:(nullable id)sender;
- (void)runToolbarCustomizationPalette:(nullable id)sender;
@property(class) BOOL allowsAutomaticWindowTabbing;
@property(class, readonly) NSWindowUserTabbingPreference userTabbingPreference;
@property NSWindowTabbingMode tabbingMode;
@property(copy) NSWindowTabbingIdentifier tabbingIdentifier;
- (void)selectNextTab:(nullable id)sender;
- (void)selectPreviousTab:(nullable id)sender;
- (void)moveTabToNewWindow:(nullable id)sender;
- (void)mergeAllWindows:(nullable id)sender;
- (void)toggleTabBar:(nullable id)sender;
- (void)toggleTabOverview:(nullable id)sender;
@property(nullable, readonly, copy) NSArray<NSWindow*>* tabbedWindows;
- (void)addTabbedWindow:(NSWindow*)window ordered:(NSWindowOrderingMode)ordered;
@property(strong, readonly) NSWindowTab* tab;
@property(readonly, weak, nullable) NSWindowTabGroup* tabGroup;
- (void)toggleFullScreen:(nullable id)sender;
@property BOOL restorable;
@property(nullable, assign) Class restorationClass;
- (void)disableSnapshotRestoration;
- (void)enableSnapshotRestoration;
@property NSSize minSize;
@property NSSize maxSize;
@property NSSize contentMinSize;
@property NSSize contentMaxSize;
@property NSSize minFullScreenContentSize;
@property NSSize maxFullScreenContentSize;
@property(nullable, weak) __kindof NSWindowController* windowController;
@property(nullable, strong) NSViewController* contentViewController;
- (BOOL)setFrameAutosaveName:(NSWindowFrameAutosaveName)name;
@property(readonly, copy) NSWindowFrameAutosaveName frameAutosaveName;
- (void)saveFrameUsingName:(NSWindowFrameAutosaveName)name;
- (BOOL)setFrameUsingName:(NSWindowFrameAutosaveName)name;
@property(readonly, copy) NSWindowPersistableFrameDescriptor stringWithSavedFrame;
- (void)setFrameFromString:(NSWindowPersistableFrameDescriptor)string;
@end

@protocol NSWindowDelegate <NSObject>
@optional
- (BOOL)windowShouldClose:(NSWindow*)sender;
- (nullable id)windowWillReturnFieldEditor:(NSWindow*)sender
                                  toObject:(nullable id)client;
- (NSSize)windowWillResize:(NSWindow*)sender toSize:(NSSize)frameSize;
- (NSRect)windowWillUseStandardFrame:(NSWindow*)window
                        defaultFrame:(NSRect)newFrame;
- (BOOL)windowShouldZoom:(NSWindow*)window toFrame:(NSRect)newFrame;
- (nullable NSUndoManager*)windowWillReturnUndoManager:(NSWindow*)window;
- (NSRect)window:(NSWindow*)window
    willPositionSheet:(NSWindow*)sheet
            usingRect:(NSRect)rect;
- (BOOL)window:(NSWindow*)window
    shouldPopUpDocumentPathMenu:(NSMenu*)menu;
- (NSSize)window:(NSWindow*)window
    willUseFullScreenContentSize:(NSSize)proposedSize;
- (NSApplicationPresentationOptions)window:(NSWindow*)window
    willUseFullScreenPresentationOptions:
        (NSApplicationPresentationOptions)proposedOptions;
- (nullable NSArray<NSWindow*>*)customWindowsToEnterFullScreenForWindow:
    (NSWindow*)window;
- (void)window:(NSWindow*)window
    startCustomAnimationToEnterFullScreenWithDuration:(NSTimeInterval)duration;
- (void)windowDidFailToEnterFullScreen:(NSWindow*)window;
- (nullable NSArray<NSWindow*>*)customWindowsToExitFullScreenForWindow:
    (NSWindow*)window;
- (void)window:(NSWindow*)window
    startCustomAnimationToExitFullScreenWithDuration:(NSTimeInterval)duration;
- (void)windowDidFailToExitFullScreen:(NSWindow*)window;
- (void)window:(NSWindow*)window willEncodeRestorableState:(NSCoder*)state;
- (void)window:(NSWindow*)window didDecodeRestorableState:(NSCoder*)state;
- (void)windowDidResize:(NSNotification*)notification;
- (void)windowDidExpose:(NSNotification*)notification;
- (void)windowWillMove:(NSNotification*)notification;
- (void)windowDidMove:(NSNotification*)notification;
- (void)windowDidBecomeKey:(NSNotification*)notification;
- (void)windowDidResignKey:(NSNotification*)notification;
- (void)windowDidBecomeMain:(NSNotification*)notification;
- (void)windowDidResignMain:(NSNotification*)notification;
- (void)windowWillClose:(NSNotification*)notification;
- (void)windowWillMiniaturize:(NSNotification*)notification;
- (void)windowDidMiniaturize:(NSNotification*)notification;
- (void)windowDidDeminiaturize:(NSNotification*)notification;
- (void)windowDidUpdate:(NSNotification*)notification;
- (void)windowDidChangeScreen:(NSNotification*)notification;
- (void)windowDidChangeScreenProfile:(NSNotification*)notification;
- (void)windowDidChangeBackingProperties:(NSNotification*)notification;
- (void)windowWillBeginSheet:(NSNotification*)notification;
- (void)windowDidEndSheet:(NSNotification*)notification;
- (void)windowWillStartLiveResize:(NSNotification*)notification;
- (void)windowDidEndLiveResize:(NSNotification*)notification;
- (void)windowWillEnterFullScreen:(NSNotification*)notification;
- (void)windowDidEnterFullScreen:(NSNotification*)notification;
- (void)windowWillExitFullScreen:(NSNotification*)notification;
- (void)windowDidExitFullScreen:(NSNotification*)notification;
- (void)windowWillEnterVersionBrowser:(NSNotification*)notification;
- (void)windowDidEnterVersionBrowser:(NSNotification*)notification;
- (void)windowWillExitVersionBrowser:(NSNotification*)notification;
- (void)windowDidExitVersionBrowser:(NSNotification*)notification;
- (void)windowDidChangeOcclusionState:(NSNotification*)notification;
@end

APPKIT_EXTERN NSNotificationName const NSWindowDidBecomeKeyNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidBecomeMainNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidChangeScreenNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidDeminiaturizeNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidExposeNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidMiniaturizeNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidMoveNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidResignKeyNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidResignMainNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidResizeNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidUpdateNotification;
APPKIT_EXTERN NSNotificationName const NSWindowWillCloseNotification;
APPKIT_EXTERN NSNotificationName const NSWindowWillMiniaturizeNotification;
APPKIT_EXTERN NSNotificationName const NSWindowWillMoveNotification;
APPKIT_EXTERN NSNotificationName const NSWindowWillBeginSheetNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidEndSheetNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidChangeBackingPropertiesNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidChangeScreenProfileNotification;
APPKIT_EXTERN NSNotificationName const NSWindowWillStartLiveResizeNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidEndLiveResizeNotification;
APPKIT_EXTERN NSNotificationName const NSWindowWillEnterFullScreenNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidEnterFullScreenNotification;
APPKIT_EXTERN NSNotificationName const NSWindowWillExitFullScreenNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidExitFullScreenNotification;
APPKIT_EXTERN NSNotificationName const NSWindowDidChangeOcclusionStateNotification;

// -- NSPanel.h ---------------------------------------------------------------

@interface NSPanel : NSWindow
@property(getter=isFloatingPanel) BOOL floatingPanel;
@property BOOL becomesKeyOnlyIfNeeded;
@property BOOL worksWhenModal;
@end

// -- NSViewController.h, NSTitlebarAccessoryViewController.h -----------------

@interface NSViewController : NSResponder <NSCoding>
- (instancetype)initWithNibName:(nullable NSString*)nibNameOrNil
                         bundle:(nullable NSBundle*)nibBundleOrNil
    NS_DESIGNATED_INITIALIZER;
- (nullable instancetype)initWithCoder:(NSCoder*)coder
    NS_DESIGNATED_INITIALIZER;
@property(nullable, strong) id representedObject;
@property(nullable, copy) NSString* title;
@property(strong) NSView* view;
- (void)loadView;
@property(readonly, getter=isViewLoaded) BOOL viewLoaded;
- (void)viewDidLoad;
- (void)viewWillAppear;
- (void)viewDidAppear;
- (void)viewWillDisappear;
- (void)viewDidDisappear;
@property NSSize preferredContentSize;
- (void)updateViewConstraints;
- (void)viewWillLayout;
- (void)viewDidLayout;
@property(nullable, readonly) NSViewController* parentViewController;
@property(copy) NSArray<__kindof NSViewController*>* childViewControllers;
- (void)addChildViewController:(NSViewController*)childViewController;
- (void)removeFromParentViewController;
@end

typedef NS_ENUM(NSInteger, NSLayoutAttribute){
    NSLayoutAttributeLeft = 1,
    NSLayoutAttributeRight,
    NSLayoutAttributeTop,
    NSLayoutAttributeBottom,
    NSLayoutAttributeLeading,
    NSLayoutAttributeTrailing,
    NSLayoutAttributeWidth,
    NSLayoutAttributeHeight,
    NSLayoutAttributeCenterX,
    NSLayoutAttributeCenterY,
    NSLayoutAttributeLastBaseline,
    NSLayoutAttributeBaseline = NSLayoutAttributeLastBaseline,
    NSLayoutAttributeFirstBaseline,
    NSLayoutAttributeNotAnAttribute = 0,
};

@interface NSTitlebarAccessoryViewController : NSViewController
@property NSLayoutAttribute layoutAttribute;
@property CGFloat fullScreenMinHeight;
@property(getter=isHidden) BOOL hidden;
@property BOOL automaticallyAdjustsSize;
- (void)viewWillAppear NS_REQUIRES_SUPER;
- (void)viewDidAppear NS_REQUIRES_SUPER;
- (void)viewDidDisappear NS_REQUIRES_SUPER;
@end

// -- NSWindowController.h ----------------------------------------------------

@interface NSWindowController : NSResponder <NSCoding>
- (instancetype)initWithWindow:(nullable NSWindow*)window
    NS_DESIGNATED_INITIALIZER;
- (nullable instancetype)initWithCoder:(NSCoder*)coder
    NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithWindowNibName:(NSString*)windowNibName;
- (instancetype)initWithWindowNibName:(NSString*)windowNibName
                                owner:(id)owner;
@property(nullable, readonly, copy) NSString* windowNibName;
@property(nullable, readonly, copy) NSString* windowNibPath;
@property(nullable, readonly, weak) id owner;
@property BOOL shouldCascadeWindows;
@property(nullable, weak) id document;
@property BOOL shouldCloseDocument;
@property(nullable, strong) NSWindow* window;
@property(readonly, getter=isWindowLoaded) BOOL windowLoaded;
- (void)windowWillLoad;
- (void)windowDidLoad;
- (void)loadWindow;
- (void)close;
- (void)showWindow:(nullable id)sender;
- (void)dismissController:(nullable id)sender;
@property(nullable, strong) NSViewController* contentViewController;
- (void)synchronizeWindowTitleWithDocumentName;
@end

// -- NSVisualEffectView.h ----------------------------------------------------

typedef NS_ENUM(NSInteger, NSVisualEffectMaterial){
    NSVisualEffectMaterialTitlebar = 3,
    NSVisualEffectMaterialSelection = 4,
    NSVisualEffectMaterialMenu = 5,
    NSVisualEffectMaterialPopover = 6,
    NSVisualEffectMaterialSidebar = 7,
    NSVisualEffectMaterialHeaderView = 10,
    NSVisualEffectMaterialSheet = 11,
    NSVisualEffectMaterialWindowBackground = 12,
    NSVisualEffectMaterialHUDWindow = 13,
    NSVisualEffectMaterialFullScreenUI = 15,
    NSVisualEffectMaterialToolTip = 17,
    NSVisualEffectMaterialContentBackground = 18,
    NSVisualEffectMaterialUnderWindowBackground = 21,
    NSVisualEffectMaterialUnderPageBackground = 22,
    NSVisualEffectMaterialAppearanceBased = 0,
    NSVisualEffectMaterialLight = 1,
    NSVisualEffectMaterialDark = 2,
    NSVisualEffectMaterialMediumLight = 8,
    NSVisualEffectMaterialUltraDark = 9,
};

typedef NS_ENUM(NSInteger, NSVisualEffectBlendingMode){
    NSVisualEffectBlendingModeBehindWindow,
    NSVisualEffectBlendingModeWithinWindow,
};

typedef NS_ENUM(NSInteger, NSVisualEffectState){
    NSVisualEffectStateFollowsWindowActiveState,
    NSVisualEffectStateActive,
    NSVisualEffectStateInactive,
};

typedef NS_ENUM(NSInteger, NSBackgroundStyle){
    NSBackgroundStyleNormal = 0,
    NSBackgroundStyleEmphasized,
    NSBackgroundStyleRaised,
    NSBackgroundStyleLowered,
};

@interface NSVisualEffectView : NSView
@property NSVisualEffectMaterial material;
@property(readonly) NSBackgroundStyle interiorBackgroundStyle;
@property NSVisualEffectBlendingMode blendingMode;
@property NSVisualEffectState state;
@property(nullable, retain) NSImage* maskImage;
@property(getter=isEmphasized) BOOL emphasized;
- (void)viewDidMoveToWindow NS_REQUIRES_SUPER;
- (void)viewWillMoveToWindow:(nullable NSWindow*)newWindow NS_REQUIRES_SUPER;
@end

// -- NSGlassEffectView.h (macOS 26) ------------------------------------------

API_AVAILABLE(macos(26.0))
@interface NSGlassEffectView : NSView
@property(nullable, strong) NSView* contentView;
@property CGFloat cornerRadius;
@property(nullable, copy) NSColor* tintColor;
@end

// -- NSCell.h (control states) -----------------------------------------------

typedef NSInteger NSControlStateValue;
static const NSControlStateValue NSControlStateValueMixed = -1;
static const NSControlStateValue NSControlStateValueOff = 0;
static const NSControlStateValue NSControlStateValueOn = 1;

// -- NSMenu.h ----------------------------------------------------------------

@class NSMenuItem;
@protocol NSMenuDelegate;

typedef NS_ENUM(NSInteger, NSMenuPresentationStyle){
    NSMenuPresentationStyleRegular,
    NSMenuPresentationStylePalette,
};

typedef NS_ENUM(NSInteger, NSMenuSelectionMode){
    NSMenuSelectionModeAutomatic,
    NSMenuSelectionModeSelectOne,
    NSMenuSelectionModeSelectAny,
};

typedef NS_OPTIONS(NSUInteger, NSMenuProperties){
    NSMenuPropertyItemTitle = 1 << 0,
    NSMenuPropertyItemAttributedTitle = 1 << 1,
    NSMenuPropertyItemKeyEquivalent = 1 << 2,
    NSMenuPropertyItemImage = 1 << 3,
    NSMenuPropertyItemEnabled = 1 << 4,
    NSMenuPropertyItemAccessibilityDescription = 1 << 5,
};

@interface NSMenu
    : NSObject <NSCopying, NSCoding, NSUserInterfaceItemIdentification,
                NSAppearanceCustomization>
- (instancetype)initWithTitle:(NSString*)title NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithCoder:(NSCoder*)coder NS_DESIGNATED_INITIALIZER;
@property(copy) NSString* title;
+ (void)popUpContextMenu:(NSMenu*)menu
               withEvent:(NSEvent*)event
                 forView:(NSView*)view;
+ (void)popUpContextMenu:(NSMenu*)menu
               withEvent:(NSEvent*)event
                 forView:(nullable NSView*)view
                withFont:(nullable NSFont*)font;
- (BOOL)popUpMenuPositioningItem:(nullable NSMenuItem*)item
                      atLocation:(NSPoint)location
                          inView:(nullable NSView*)view;
+ (void)setMenuBarVisible:(BOOL)visible;
+ (BOOL)menuBarVisible;
@property(nullable, readonly, assign) NSMenu* supermenu;
- (void)insertItem:(NSMenuItem*)newItem atIndex:(NSInteger)index;
- (void)addItem:(NSMenuItem*)newItem;
- (NSMenuItem*)insertItemWithTitle:(NSString*)string
                            action:(nullable SEL)selector
                     keyEquivalent:(NSString*)charCode
                           atIndex:(NSInteger)index;
- (NSMenuItem*)addItemWithTitle:(NSString*)string
                         action:(nullable SEL)selector
                  keyEquivalent:(NSString*)charCode;
- (void)removeItemAtIndex:(NSInteger)index;
- (void)removeItem:(NSMenuItem*)item;
- (void)setSubmenu:(nullable NSMenu*)menu forItem:(NSMenuItem*)item;
- (void)removeAllItems;
@property(copy) NSArray<NSMenuItem*>* itemArray;
@property(readonly) NSInteger numberOfItems;
- (nullable NSMenuItem*)itemAtIndex:(NSInteger)index;
- (NSInteger)indexOfItem:(NSMenuItem*)item;
- (NSInteger)indexOfItemWithTitle:(NSString*)title;
- (NSInteger)indexOfItemWithTag:(NSInteger)tag;
- (NSInteger)indexOfItemWithRepresentedObject:(nullable id)object;
- (NSInteger)indexOfItemWithSubmenu:(nullable NSMenu*)submenu;
- (NSInteger)indexOfItemWithTarget:(nullable id)target
                         andAction:(nullable SEL)actionSelector;
- (nullable NSMenuItem*)itemWithTitle:(NSString*)title;
- (nullable NSMenuItem*)itemWithTag:(NSInteger)tag;
@property BOOL autoenablesItems;
- (void)update;
- (BOOL)performKeyEquivalent:(NSEvent*)event;
- (void)itemChanged:(NSMenuItem*)item;
- (void)performActionForItemAtIndex:(NSInteger)index;
@property(nullable, weak) id<NSMenuDelegate> delegate;
@property(readonly) CGFloat menuBarHeight;
- (void)cancelTracking;
- (void)cancelTrackingWithoutAnimation;
@property(nullable, readonly, strong) NSMenuItem* highlightedItem;
@property CGFloat minimumWidth;
@property(readonly) NSSize size;
@property(null_resettable, strong) NSFont* font;
@property BOOL allowsContextMenuPlugIns;
@property BOOL showsStateColumn;
@property NSUserInterfaceLayoutDirection userInterfaceLayoutDirection;
@property NSMenuPresentationStyle presentationStyle;
@property NSMenuSelectionMode selectionMode;
@property(copy) NSArray<NSMenuItem*>* selectedItems;
@end

@protocol NSMenuItemValidation <NSObject>
- (BOOL)validateMenuItem:(NSMenuItem*)menuItem;
@end

@protocol NSMenuDelegate <NSObject>
@optional
- (void)menuNeedsUpdate:(NSMenu*)menu;
- (NSInteger)numberOfItemsInMenu:(NSMenu*)menu;
- (BOOL)menu:(NSMenu*)menu
        updateItem:(NSMenuItem*)item
           atIndex:(NSInteger)index
      shouldCancel:(BOOL)shouldCancel;
- (BOOL)menuHasKeyEquivalent:(NSMenu*)menu
                    forEvent:(NSEvent*)event
                      target:(id _Nullable* _Nonnull)target
                      action:(SEL _Nullable* _Nonnull)action;
- (void)menuWillOpen:(NSMenu*)menu;
- (void)menuDidClose:(NSMenu*)menu;
- (void)menu:(NSMenu*)menu willHighlightItem:(nullable NSMenuItem*)item;
- (NSRect)confinementRectForMenu:(NSMenu*)menu
                        onScreen:(nullable NSScreen*)screen;
@end

APPKIT_EXTERN NSNotificationName const NSMenuWillSendActionNotification;
APPKIT_EXTERN NSNotificationName const NSMenuDidSendActionNotification;
APPKIT_EXTERN NSNotificationName const NSMenuDidAddItemNotification;
APPKIT_EXTERN NSNotificationName const NSMenuDidRemoveItemNotification;
APPKIT_EXTERN NSNotificationName const NSMenuDidChangeItemNotification;
APPKIT_EXTERN NSNotificationName const NSMenuDidBeginTrackingNotification;
APPKIT_EXTERN NSNotificationName const NSMenuDidEndTrackingNotification;

// -- NSMenuItem.h ------------------------------------------------------------

@interface NSMenuItem
    : NSObject <NSCopying, NSCoding, NSValidatedUserInterfaceItem,
                NSUserInterfaceItemIdentification>
@property(class) BOOL usesUserKeyEquivalents;
+ (NSMenuItem*)separatorItem;
+ (NSMenuItem*)sectionHeaderWithTitle:(NSString*)title;
- (instancetype)initWithTitle:(NSString*)string
                       action:(nullable SEL)selector
                keyEquivalent:(NSString*)charCode NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithCoder:(NSCoder*)coder NS_DESIGNATED_INITIALIZER;
@property(nullable, assign) NSMenu* menu;
@property(readonly) BOOL hasSubmenu;
@property(nullable, strong) NSMenu* submenu;
@property(nullable, readonly, assign) NSMenuItem* parentItem;
@property(copy) NSString* title;
@property(nullable, copy) NSAttributedString* attributedTitle;
@property(nullable, copy) NSString* subtitle;
@property(getter=isSeparatorItem, readonly) BOOL separatorItem;
@property(getter=isSectionHeader, readonly) BOOL sectionHeader;
@property(copy) NSString* keyEquivalent;
@property NSEventModifierFlags keyEquivalentModifierMask;
@property(readonly) NSString* userKeyEquivalent;
@property BOOL allowsKeyEquivalentWhenHidden;
@property BOOL allowsAutomaticKeyEquivalentLocalization;
@property BOOL allowsAutomaticKeyEquivalentMirroring;
@property(nullable, strong) NSImage* image;
@property NSControlStateValue state;
@property(null_resettable, strong) NSImage* onStateImage;
@property(nullable, strong) NSImage* offStateImage;
@property(null_resettable, strong) NSImage* mixedStateImage;
@property(getter=isEnabled) BOOL enabled;
@property(getter=isAlternate) BOOL alternate;
@property NSInteger indentationLevel;
@property(nullable, weak) id target;
@property(nullable) SEL action;
@property NSInteger tag;
@property(nullable, strong) id representedObject;
@property(nullable, strong) NSView* view;
@property(getter=isHighlighted, readonly) BOOL highlighted;
@property(getter=isHidden) BOOL hidden;
@property(getter=isHiddenOrHasHiddenAncestor, readonly)
    BOOL hiddenOrHasHiddenAncestor;
@property(nullable, copy) NSString* toolTip;
@end

// -- NSRunningApplication.h --------------------------------------------------

typedef NS_OPTIONS(NSUInteger, NSApplicationActivationOptions){
    NSApplicationActivateAllWindows = 1 << 0,
    NSApplicationActivateIgnoringOtherApps = 1 << 1,
};

typedef NS_ENUM(NSInteger, NSApplicationActivationPolicy){
    NSApplicationActivationPolicyRegular,
    NSApplicationActivationPolicyAccessory,
    NSApplicationActivationPolicyProhibited,
};

@interface NSRunningApplication : NSObject
@property(readonly, getter=isTerminated) BOOL terminated;
@property(readonly, getter=isFinishedLaunching) BOOL finishedLaunching;
@property(readonly, getter=isHidden) BOOL hidden;
@property(readonly, getter=isActive) BOOL active;
@property(readonly) BOOL ownsMenuBar;
@property(readonly) NSApplicationActivationPolicy activationPolicy;
@property(nullable, readonly, copy) NSString* localizedName;
@property(nullable, readonly, copy) NSString* bundleIdentifier;
@property(nullable, readonly, copy) NSURL* bundleURL;
@property(nullable, readonly, copy) NSURL* executableURL;
@property(readonly) pid_t processIdentifier;
@property(nullable, readonly, copy) NSDate* launchDate;
@property(nullable, readonly, strong) NSImage* icon;
@property(readonly) NSInteger executableArchitecture;
- (BOOL)hide;
- (BOOL)unhide;
- (BOOL)activateFromApplication:(NSRunningApplication*)application
                        options:(NSApplicationActivationOptions)options;
- (BOOL)activateWithOptions:(NSApplicationActivationOptions)options;
- (BOOL)terminate;
- (BOOL)forceTerminate;
+ (NSArray<NSRunningApplication*>*)runningApplicationsWithBundleIdentifier:
    (NSString*)bundleIdentifier;
+ (nullable instancetype)runningApplicationWithProcessIdentifier:(pid_t)pid;
@property(class, readonly, strong) __kindof NSRunningApplication*
    currentApplication;
+ (void)terminateAutomaticallyTerminableApplications;
@end

// -- NSWorkspace.h -----------------------------------------------------------

typedef NSString* NSWorkspaceLaunchConfigurationKey NS_TYPED_ENUM;

@interface NSWorkspaceOpenConfiguration : NSObject <NSCopying>
+ (instancetype)configuration;
@property BOOL promptsUserIfNeeded;
@property BOOL addsToRecentItems;
@property BOOL activates;
@property BOOL hides;
@property BOOL hidesOthers;
@property BOOL createsNewApplicationInstance;
@property BOOL allowsRunningApplicationSubstitution;
@property(copy) NSArray<NSString*>* arguments;
@property(copy) NSDictionary<NSString*, NSString*>* environment;
@end

@interface NSWorkspace : NSObject
@property(class, readonly, strong) NSWorkspace* sharedWorkspace;
@property(readonly, strong) NSNotificationCenter* notificationCenter;
- (BOOL)openURL:(NSURL*)url;
- (void)openURL:(NSURL*)url
        configuration:(NSWorkspaceOpenConfiguration*)configuration
    completionHandler:
        (void (^_Nullable)(NSRunningApplication* _Nullable app,
                           NSError* _Nullable error))completionHandler;
- (void)openURLs:(NSArray<NSURL*>*)urls
    withApplicationAtURL:(NSURL*)applicationURL
           configuration:(NSWorkspaceOpenConfiguration*)configuration
       completionHandler:
           (void (^_Nullable)(NSRunningApplication* _Nullable app,
                              NSError* _Nullable error))completionHandler;
- (void)openApplicationAtURL:(NSURL*)applicationURL
               configuration:(NSWorkspaceOpenConfiguration*)configuration
           completionHandler:
               (void (^_Nullable)(NSRunningApplication* _Nullable app,
                                  NSError* _Nullable error))completionHandler;
- (BOOL)selectFile:(nullable NSString*)fullPath
    inFileViewerRootedAtPath:(NSString*)rootFullPath;
- (void)activateFileViewerSelectingURLs:(NSArray<NSURL*>*)fileURLs;
- (BOOL)isFilePackageAtPath:(NSString*)fullPath;
- (NSImage*)iconForFile:(NSString*)fullPath;
- (nullable NSURL*)URLForApplicationWithBundleIdentifier:
    (NSString*)bundleIdentifier;
- (nullable NSURL*)URLForApplicationToOpenURL:(NSURL*)url;
- (void)hideOtherApplications;
@property(readonly, copy) NSArray<NSRunningApplication*>* runningApplications;
@property(nullable, readonly, strong) NSRunningApplication* frontmostApplication;
@property(nullable, readonly, strong) NSRunningApplication* menuBarOwningApplication;
@property(readonly) BOOL accessibilityDisplayShouldIncreaseContrast;
@property(readonly) BOOL accessibilityDisplayShouldDifferentiateWithoutColor;
@property(readonly) BOOL accessibilityDisplayShouldReduceTransparency;
@property(readonly) BOOL accessibilityDisplayShouldReduceMotion;
@property(readonly) BOOL accessibilityDisplayShouldInvertColors;
@property(readonly, getter=isVoiceOverEnabled) BOOL voiceOverEnabled;
@property(readonly, getter=isSwitchControlEnabled) BOOL switchControlEnabled;
@end

APPKIT_EXTERN NSString* const NSWorkspaceApplicationKey;
APPKIT_EXTERN NSNotificationName const NSWorkspaceWillLaunchApplicationNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceDidLaunchApplicationNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceDidTerminateApplicationNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceDidHideApplicationNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceDidUnhideApplicationNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceDidActivateApplicationNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceDidDeactivateApplicationNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceWillPowerOffNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceWillSleepNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceDidWakeNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceScreensDidSleepNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceScreensDidWakeNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceSessionDidBecomeActiveNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceSessionDidResignActiveNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceActiveSpaceDidChangeNotification;
APPKIT_EXTERN NSNotificationName const NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification;

// -- NSApplication.h ---------------------------------------------------------

typedef NS_ENUM(NSUInteger, NSApplicationTerminateReply){
    NSTerminateCancel = 0,
    NSTerminateNow = 1,
    NSTerminateLater = 2,
};

typedef NS_ENUM(NSUInteger, NSApplicationPrintReply){
    NSPrintingCancelled = 0,
    NSPrintingSuccess = 1,
    NSPrintingReplyLater = 2,
    NSPrintingFailure = 3,
};

typedef NS_ENUM(NSUInteger, NSApplicationDelegateReply){
    NSApplicationDelegateReplySuccess = 0,
    NSApplicationDelegateReplyCancel = 1,
    NSApplicationDelegateReplyFailure = 2,
};

typedef NS_ENUM(NSUInteger, NSRequestUserAttentionType){
    NSCriticalRequest = 0,
    NSInformationalRequest = 10,
};

typedef NS_OPTIONS(NSUInteger, NSApplicationOcclusionState){
    NSApplicationOcclusionStateVisible = 1UL << 1,
};

typedef NS_OPTIONS(NSInteger, NSWindowListOptions){
    NSWindowListOrderedFrontToBack = (1 << 0),
};

typedef struct _NSModalSession* NSModalSession;

static const NSModalResponse NSModalResponseStop = (-1000);
static const NSModalResponse NSModalResponseAbort = (-1001);
static const NSModalResponse NSModalResponseContinue = (-1002);

APPKIT_EXTERN NSRunLoopMode const NSModalPanelRunLoopMode;
APPKIT_EXTERN NSRunLoopMode const NSEventTrackingRunLoopMode;

@protocol NSApplicationDelegate, NSUserActivityRestoring;
@class NSApplication;

APPKIT_EXTERN __kindof NSApplication* _Null_unspecified NSApp;

@interface NSApplication
    : NSResponder <NSUserInterfaceValidations, NSAppearanceCustomization>
@property(class, readonly, strong) __kindof NSApplication* sharedApplication;
@property(nullable, weak) id<NSApplicationDelegate> delegate;
- (void)hide:(nullable id)sender;
- (void)unhide:(nullable id)sender;
- (void)unhideWithoutActivation;
- (nullable NSWindow*)windowWithWindowNumber:(NSInteger)windowNum;
@property(nullable, readonly, weak) NSWindow* mainWindow;
@property(nullable, readonly, weak) NSWindow* keyWindow;
@property(readonly, getter=isActive) BOOL active;
@property(readonly, getter=isHidden) BOOL hidden;
@property(readonly, getter=isRunning) BOOL running;
- (void)deactivate;
- (void)activateIgnoringOtherApps:(BOOL)ignoreOtherApps;
- (void)activate;
- (void)yieldActivationToApplication:(NSRunningApplication*)application;
- (void)hideOtherApplications:(nullable id)sender;
- (void)unhideAllApplications:(nullable id)sender;
- (void)finishLaunching;
- (void)run;
- (NSModalResponse)runModalForWindow:(NSWindow*)window;
- (void)stop:(nullable id)sender;
- (void)stopModal;
- (void)stopModalWithCode:(NSModalResponse)returnCode;
- (void)abortModal;
@property(nullable, readonly, strong) NSWindow* modalWindow;
- (NSModalSession)beginModalSessionForWindow:(NSWindow*)window;
- (NSModalResponse)runModalSession:(NSModalSession)session;
- (void)endModalSession:(NSModalSession)session;
- (void)terminate:(nullable id)sender;
- (NSInteger)requestUserAttention:(NSRequestUserAttentionType)requestType;
- (void)cancelUserAttentionRequest:(NSInteger)request;
- (void)enumerateWindowsWithOptions:(NSWindowListOptions)options
                         usingBlock:(void(NS_NOESCAPE ^)(NSWindow* window,
                                                         BOOL* stop))block;
- (void)preventWindowOrdering;
@property(readonly, copy) NSArray<NSWindow*>* windows;
- (void)setWindowsNeedUpdate:(BOOL)needUpdate;
- (void)updateWindows;
@property(nullable, strong) NSMenu* mainMenu;
@property(nullable, strong) NSMenu* helpMenu;
@property(null_resettable, strong) NSImage* applicationIconImage;
- (NSApplicationActivationPolicy)activationPolicy;
- (BOOL)setActivationPolicy:(NSApplicationActivationPolicy)activationPolicy;
@property(readonly, strong) NSDockTile* dockTile;
- (void)reportException:(NSException*)exception;
+ (void)detachDrawingThread:(SEL)selector
                   toTarget:(id)target
                 withObject:(nullable id)argument;
- (void)replyToApplicationShouldTerminate:(BOOL)shouldTerminate;
- (void)replyToOpenOrPrint:(NSApplicationDelegateReply)reply;
- (void)orderFrontCharacterPalette:(nullable id)sender;
@property NSApplicationPresentationOptions presentationOptions;
@property(readonly) NSApplicationPresentationOptions currentSystemPresentationOptions;
@property(readonly) NSApplicationOcclusionState occlusionState;
@property(readonly, getter=isProtectedDataAvailable) BOOL protectedDataAvailable;
- (void)sendEvent:(NSEvent*)event;
- (void)postEvent:(NSEvent*)event atStart:(BOOL)atStart;
@property(nullable, readonly, strong) NSEvent* currentEvent;
- (nullable NSEvent*)nextEventMatchingMask:(NSEventMask)mask
                                 untilDate:(nullable NSDate*)expiration
                                    inMode:(NSRunLoopMode)mode
                                   dequeue:(BOOL)deqFlag;
- (void)discardEventsMatchingMask:(NSEventMask)mask
                      beforeEvent:(nullable NSEvent*)lastEvent;
- (BOOL)sendAction:(SEL)action to:(nullable id)target from:(nullable id)sender;
- (nullable id)targetForAction:(SEL)action;
- (nullable id)targetForAction:(SEL)action
                            to:(nullable id)target
                          from:(nullable id)sender;
- (BOOL)tryToPerform:(SEL)action with:(nullable id)object;
- (nullable id)validRequestorForSendType:(NSPasteboardType)sendType
                              returnType:(NSPasteboardType)returnType;
@property(nullable, strong) NSMenu* windowsMenu;
- (void)arrangeInFront:(nullable id)sender;
- (void)removeWindowsItem:(NSWindow*)win;
- (void)addWindowsItem:(NSWindow*)win
                 title:(NSString*)string
              filename:(BOOL)isFilename;
- (void)changeWindowsItem:(NSWindow*)win
                    title:(NSString*)string
                 filename:(BOOL)isFilename;
- (void)updateWindowsItem:(NSWindow*)win;
- (void)miniaturizeAll:(nullable id)sender;
@property(nullable, strong) NSMenu* servicesMenu;
- (void)registerServicesMenuSendTypes:(NSArray<NSPasteboardType>*)sendTypes
                          returnTypes:(NSArray<NSPasteboardType>*)returnTypes;
@property(nullable, strong) id servicesProvider;
- (void)orderFrontStandardAboutPanel:(nullable id)sender;
- (void)orderFrontStandardAboutPanelWithOptions:
    (NSDictionary<NSString*, id>*)optionsDictionary;
@property(readonly) NSUserInterfaceLayoutDirection userInterfaceLayoutDirection;
- (void)disableRelaunchOnLogin;
- (void)enableRelaunchOnLogin;
- (void)registerForRemoteNotifications;
- (void)unregisterForRemoteNotifications;
@property(readonly, getter=isRegisteredForRemoteNotifications)
    BOOL registeredForRemoteNotifications;
@property BOOL automaticCustomizeTouchBarMenuItemEnabled;
- (void)toggleTouchBarCustomizationPalette:(nullable id)sender;
@property(readonly, getter=isFullKeyboardAccessEnabled)
    BOOL fullKeyboardAccessEnabled;
@end

@interface NSApplication (NSScripting)
@property(readonly, copy) NSArray<NSWindow*>* orderedWindows;
@end

@interface NSApplication (NSDeprecated)
- (void)endSheet:(NSWindow*)sheet
    API_DEPRECATED("Use -[NSWindow endSheet:] instead", macos(10.0, 10.10));
- (void)endSheet:(NSWindow*)sheet
      returnCode:(NSInteger)returnCode
    API_DEPRECATED("Use -[NSWindow endSheet:returnCode:] instead",
                   macos(10.0, 10.10));
@end

@protocol NSServicesMenuRequestor <NSObject>
@optional
- (BOOL)writeSelectionToPasteboard:(NSPasteboard*)pboard
                             types:(NSArray<NSPasteboardType>*)types;
- (BOOL)readSelectionFromPasteboard:(NSPasteboard*)pboard;
@end

@protocol NSApplicationDelegate <NSObject>
@optional
- (NSApplicationTerminateReply)applicationShouldTerminate:
    (NSApplication*)sender;
- (void)application:(NSApplication*)application
           openURLs:(NSArray<NSURL*>*)urls;
- (BOOL)application:(NSApplication*)sender openFile:(NSString*)filename;
- (void)application:(NSApplication*)sender
          openFiles:(NSArray<NSString*>*)filenames;
- (BOOL)application:(NSApplication*)sender openTempFile:(NSString*)filename;
- (BOOL)applicationShouldOpenUntitledFile:(NSApplication*)sender;
- (BOOL)applicationOpenUntitledFile:(NSApplication*)sender;
- (BOOL)application:(id)sender
    openFileWithoutUI:(NSString*)filename;
- (BOOL)application:(NSApplication*)sender printFile:(NSString*)filename;
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:
    (NSApplication*)sender;
- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender
                    hasVisibleWindows:(BOOL)flag;
- (nullable NSMenu*)applicationDockMenu:(NSApplication*)sender;
- (NSError*)application:(NSApplication*)application
       willPresentError:(NSError*)error;
- (void)application:(NSApplication*)application
    didRegisterForRemoteNotificationsWithDeviceToken:(NSData*)deviceToken;
- (void)application:(NSApplication*)application
    didFailToRegisterForRemoteNotificationsWithError:(NSError*)error;
- (void)application:(NSApplication*)application
    didReceiveRemoteNotification:(NSDictionary<NSString*, id>*)userInfo;
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app;
- (void)application:(NSApplication*)app
    willEncodeRestorableState:(NSCoder*)coder;
- (void)application:(NSApplication*)app
    didDecodeRestorableState:(NSCoder*)coder;
- (BOOL)application:(NSApplication*)application
    willContinueUserActivityWithType:(NSString*)userActivityType;
- (BOOL)application:(NSApplication*)application
    continueUserActivity:(NSUserActivity*)userActivity
      restorationHandler:
          (void (^)(NSArray<id<NSUserActivityRestoring>>* restorableObjects))
              restorationHandler;
- (void)application:(NSApplication*)application
    didFailToContinueUserActivityWithType:(NSString*)userActivityType
                                    error:(NSError*)error;
- (void)application:(NSApplication*)application
    didUpdateUserActivity:(NSUserActivity*)userActivity;
- (BOOL)application:(NSApplication*)sender
    delegateHandlesKey:(NSString*)key;
- (BOOL)applicationShouldAutomaticallyLocalizeKeyEquivalents:
    (NSApplication*)application;
- (void)applicationWillFinishLaunching:(NSNotification*)notification;
- (void)applicationDidFinishLaunching:(NSNotification*)notification;
- (void)applicationWillHide:(NSNotification*)notification;
- (void)applicationDidHide:(NSNotification*)notification;
- (void)applicationWillUnhide:(NSNotification*)notification;
- (void)applicationDidUnhide:(NSNotification*)notification;
- (void)applicationWillBecomeActive:(NSNotification*)notification;
- (void)applicationDidBecomeActive:(NSNotification*)notification;
- (void)applicationWillResignActive:(NSNotification*)notification;
- (void)applicationDidResignActive:(NSNotification*)notification;
- (void)applicationWillUpdate:(NSNotification*)notification;
- (void)applicationDidUpdate:(NSNotification*)notification;
- (void)applicationWillTerminate:(NSNotification*)notification;
- (void)applicationDidChangeScreenParameters:(NSNotification*)notification;
- (void)applicationDidChangeOcclusionState:(NSNotification*)notification;
- (void)applicationProtectedDataWillBecomeUnavailable:
    (NSNotification*)notification;
- (void)applicationProtectedDataDidBecomeAvailable:
    (NSNotification*)notification;
@end

APPKIT_EXTERN NSNotificationName const NSApplicationDidBecomeActiveNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationDidHideNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationDidFinishLaunchingNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationDidResignActiveNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationDidUnhideNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationDidUpdateNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationWillBecomeActiveNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationWillHideNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationWillFinishLaunchingNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationWillResignActiveNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationWillUnhideNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationWillUpdateNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationWillTerminateNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationDidChangeScreenParametersNotification;
APPKIT_EXTERN NSNotificationName const NSApplicationDidChangeOcclusionStateNotification;
APPKIT_EXTERN NSNotificationName const NSSystemColorsDidChangeNotification;
APPKIT_EXTERN NSNotificationName const NSPreferredScrollerStyleDidChangeNotification;

APPKIT_EXTERN int NSApplicationMain(int argc, const char* _Nonnull argv[_Nonnull]);

// -- NSUserActivity.h (AppKit) -----------------------------------------------

@protocol NSUserActivityRestoring <NSObject>
- (void)restoreUserActivityState:(NSUserActivity*)userActivity;
@end

@interface NSResponder (NSUserActivity) <NSUserActivityRestoring>
@end

// -- NSDockTile.h ------------------------------------------------------------

@interface NSDockTile : NSObject
@property(readonly) NSSize size;
@property(nullable, strong) NSView* contentView;
- (void)display;
@property BOOL showsApplicationBadge;
@property(nullable, copy) NSString* badgeLabel;
@property(nullable, readonly, weak) id owner;
@end

// -- NSAlert.h ---------------------------------------------------------------

typedef NS_ENUM(NSUInteger, NSAlertStyle){
    NSAlertStyleWarning = 0,
    NSAlertStyleInformational = 1,
    NSAlertStyleCritical = 2,
};

static const NSModalResponse NSAlertFirstButtonReturn = 1000;
static const NSModalResponse NSAlertSecondButtonReturn = 1001;
static const NSModalResponse NSAlertThirdButtonReturn = 1002;

@protocol NSAlertDelegate;

@interface NSAlert : NSObject
+ (NSAlert*)alertWithError:(NSError*)error;
@property(copy) NSString* messageText;
@property(copy) NSString* informativeText;
@property(null_resettable, strong) NSImage* icon;
- (NSButton*)addButtonWithTitle:(NSString*)title;
@property(readonly, copy) NSArray<NSButton*>* buttons;
@property NSAlertStyle alertStyle;
@property BOOL showsHelp;
@property(nullable, copy) NSString* helpAnchor;
@property(nullable, weak) id<NSAlertDelegate> delegate;
@property(nullable, strong) NSView* accessoryView;
- (void)layout;
@property BOOL showsSuppressionButton;
@property(nullable, readonly, strong) NSButton* suppressionButton;
@property(readonly, strong) NSWindow* window;
- (NSModalResponse)runModal;
- (void)beginSheetModalForWindow:(NSWindow*)sheetWindow
               completionHandler:
                   (void (^_Nullable)(NSModalResponse returnCode))handler;
@end

@protocol NSAlertDelegate <NSObject>
@optional
- (BOOL)alertShowHelp:(NSAlert*)alert;
@end

// -- NSPopover.h -------------------------------------------------------------

typedef NS_ENUM(NSInteger, NSPopoverAppearance){
    NSPopoverAppearanceMinimal = 0,
    NSPopoverAppearanceHUD = 1,
};

typedef NS_ENUM(NSInteger, NSPopoverBehavior){
    NSPopoverBehaviorApplicationDefined = 0,
    NSPopoverBehaviorTransient = 1,
    NSPopoverBehaviorSemitransient = 2,
};

@protocol NSPopoverDelegate;

@interface NSPopover : NSResponder <NSAppearanceCustomization>
- (instancetype)init NS_DESIGNATED_INITIALIZER;
@property(nullable, weak) id<NSPopoverDelegate> delegate;
@property NSPopoverBehavior behavior;
@property BOOL animates;
@property(nullable, strong) NSViewController* contentViewController;
@property NSSize contentSize;
@property(readonly, getter=isShown) BOOL shown;
@property(readonly, getter=isDetached) BOOL detached;
@property NSRect positioningRect;
- (void)showRelativeToRect:(NSRect)positioningRect
                    ofView:(NSView*)positioningView
             preferredEdge:(NSRectEdge)preferredEdge;
- (void)performClose:(nullable id)sender;
- (void)close;
@end

@protocol NSPopoverDelegate <NSObject>
@optional
- (BOOL)popoverShouldClose:(NSPopover*)popover;
- (void)popoverWillShow:(NSNotification*)notification;
- (void)popoverDidShow:(NSNotification*)notification;
- (void)popoverWillClose:(NSNotification*)notification;
- (void)popoverDidClose:(NSNotification*)notification;
@end

APPKIT_EXTERN NSNotificationName const NSPopoverWillShowNotification;
APPKIT_EXTERN NSNotificationName const NSPopoverDidShowNotification;
APPKIT_EXTERN NSNotificationName const NSPopoverWillCloseNotification;
APPKIT_EXTERN NSNotificationName const NSPopoverDidCloseNotification;

// -- NSToolbar.h -------------------------------------------------------------

typedef NSString* NSToolbarIdentifier NS_SWIFT_BRIDGED_TYPEDEF;
typedef NSString* NSToolbarItemIdentifier NS_TYPED_EXTENSIBLE_ENUM;

typedef NS_ENUM(NSUInteger, NSToolbarDisplayMode){
    NSToolbarDisplayModeDefault,
    NSToolbarDisplayModeIconAndLabel,
    NSToolbarDisplayModeIconOnly,
    NSToolbarDisplayModeLabelOnly,
};

@interface NSToolbar : NSObject
- (instancetype)initWithIdentifier:(NSToolbarIdentifier)identifier
    NS_DESIGNATED_INITIALIZER;
- (instancetype)init;
@property(readonly, copy) NSToolbarIdentifier identifier;
@property(nullable, weak) id delegate;
@property(getter=isVisible) BOOL visible;
@property NSToolbarDisplayMode displayMode;
@property BOOL showsBaselineSeparator;
@property BOOL allowsUserCustomization;
@property BOOL autosavesConfiguration;
@property(readonly, copy) NSArray* items;
@property(readonly, copy) NSArray* visibleItems;
- (void)validateVisibleItems;
@end

// -- NSSavePanel.h, NSOpenPanel.h --------------------------------------------

@class UTType;
@protocol NSOpenSavePanelDelegate;

@interface NSSavePanel : NSPanel
+ (NSSavePanel*)savePanel;
@property(nullable, readonly, copy) NSURL* URL;
@property(nullable, copy) NSURL* directoryURL;
@property(copy) NSArray<UTType*>* allowedContentTypes;
@property(nullable, copy) NSArray<NSString*>* allowedFileTypes;
@property BOOL allowsOtherFileTypes;
@property(nullable, strong) NSView* accessoryView;
@property(nullable, weak) id<NSOpenSavePanelDelegate> delegate;
@property(getter=isExpanded, readonly) BOOL expanded;
@property BOOL canCreateDirectories;
@property BOOL canSelectHiddenExtension;
@property(getter=isExtensionHidden) BOOL extensionHidden;
@property BOOL treatsFilePackagesAsDirectories;
@property(null_resettable, copy) NSString* prompt;
@property(null_resettable, copy) NSString* title;
@property(copy) NSString* nameFieldLabel;
@property(copy) NSString* nameFieldStringValue;
@property(copy) NSString* message;
@property BOOL showsHiddenFiles;
@property BOOL showsTagField;
@property(nullable, copy) NSArray<NSString*>* tagNames;
- (void)validateVisibleColumns;
- (void)ok:(nullable id)sender;
- (void)cancel:(nullable id)sender;
- (void)beginSheetModalForWindow:(NSWindow*)window
               completionHandler:(void (^)(NSModalResponse result))handler;
- (void)beginWithCompletionHandler:(void (^)(NSModalResponse result))handler;
- (NSModalResponse)runModal;
@end

@protocol NSOpenSavePanelDelegate <NSObject>
@optional
- (BOOL)panel:(id)sender shouldEnableURL:(NSURL*)url;
- (BOOL)panel:(id)sender
    validateURL:(NSURL*)url
          error:(NSError**)outError;
- (void)panel:(id)sender didChangeToDirectoryURL:(nullable NSURL*)url;
- (nullable NSString*)panel:(id)sender
    userEnteredFilename:(NSString*)filename
              confirmed:(BOOL)okFlag;
- (void)panel:(id)sender willExpand:(BOOL)expanding;
- (void)panelSelectionDidChange:(nullable id)sender;
@end

@interface NSOpenPanel : NSSavePanel
+ (NSOpenPanel*)openPanel;
@property(readonly, copy) NSArray<NSURL*>* URLs;
@property BOOL resolvesAliases;
@property BOOL canChooseDirectories;
@property BOOL allowsMultipleSelection;
@property BOOL canChooseFiles;
@property BOOL canResolveUbiquitousConflicts;
@property BOOL canDownloadUbiquitousContents;
@property(getter=isAccessoryViewDisclosed) BOOL accessoryViewDisclosed;
@end

// -- NSSharingService.h ------------------------------------------------------

typedef NSString* NSSharingServiceName NS_TYPED_EXTENSIBLE_ENUM;

typedef NS_ENUM(NSInteger, NSSharingContentScope){
    NSSharingContentScopeItem,
    NSSharingContentScopePartial,
    NSSharingContentScopeFull,
};

@protocol NSSharingServiceDelegate;

@interface NSSharingService : NSObject
@property(nullable, weak) id<NSSharingServiceDelegate> delegate;
@property(readonly, copy) NSString* title;
@property(readonly, strong) NSImage* image;
@property(nullable, readonly, strong) NSImage* alternateImage;
@property(copy) NSString* menuItemTitle;
@property(nullable, copy) NSArray<NSString*>* recipients;
@property(nullable, copy) NSString* subject;
@property(nullable, readonly, copy) NSString* messageBody;
@property(nullable, readonly, copy) NSURL* permanentLink;
@property(nullable, readonly, copy) NSString* accountName;
@property(nullable, readonly, copy) NSArray<NSURL*>* attachmentFileURLs;
+ (NSArray<NSSharingService*>*)sharingServicesForItems:(NSArray*)items;
+ (nullable NSSharingService*)sharingServiceNamed:
    (NSSharingServiceName)serviceName;
- (instancetype)initWithTitle:(NSString*)title
                        image:(NSImage*)image
               alternateImage:(nullable NSImage*)alternateImage
                      handler:(void (^)(void))block NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
- (BOOL)canPerformWithItems:(nullable NSArray*)items;
- (void)performWithItems:(NSArray*)items;
@end

@protocol NSSharingServiceDelegate <NSObject>
@optional
- (void)sharingService:(NSSharingService*)sharingService
        willShareItems:(NSArray*)items;
- (void)sharingService:(NSSharingService*)sharingService
    didFailToShareItems:(NSArray*)items
                  error:(NSError*)error;
- (void)sharingService:(NSSharingService*)sharingService
         didShareItems:(NSArray*)items;
- (NSRect)sharingService:(NSSharingService*)sharingService
    sourceFrameOnScreenForShareItem:(id)item;
- (nullable NSImage*)sharingService:(NSSharingService*)sharingService
        transitionImageForShareItem:(id)item
                        contentRect:(NSRect*)contentRect;
- (nullable NSWindow*)sharingService:(NSSharingService*)sharingService
           sourceWindowForShareItems:(NSArray*)items
                 sharingContentScope:(NSSharingContentScope*)sharingContentScope;
- (nullable NSView*)anchoringViewForSharingService:
                        (NSSharingService*)sharingService
                                showRelativeToRect:(NSRect*)positioningRect
                                     preferredEdge:(NSRectEdge*)preferredEdge;
@end

APPKIT_EXTERN NSSharingServiceName const NSSharingServiceNameComposeEmail;
APPKIT_EXTERN NSSharingServiceName const NSSharingServiceNameComposeMessage;
APPKIT_EXTERN NSSharingServiceName const NSSharingServiceNameSendViaAirDrop;
APPKIT_EXTERN NSSharingServiceName const NSSharingServiceNameAddToSafariReadingList;
APPKIT_EXTERN NSSharingServiceName const NSSharingServiceNameUseAsDesktopPicture;
APPKIT_EXTERN NSSharingServiceName const NSSharingServiceNameCloudSharing;


NS_ASSUME_NONNULL_END

#endif  // __OBJC__

#endif  // COMPILE_CHECK_STAND_IN_APPKIT_H_
