// compile_check STAND-IN for the macOS SDK's CoreGraphics framework (see
// ../README.md): the declarations Chromium's Mac code and the checked
// files use, with the SDK's names, types and signatures.

#ifndef COMPILE_CHECK_STAND_IN_COREGRAPHICS_H_
#define COMPILE_CHECK_STAND_IN_COREGRAPHICS_H_

#include <CoreFoundation/CoreFoundation.h>
#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CG_EXTERN extern __attribute__((visibility("default")))
#define CG_INLINE static inline
#define CG_AVAILABLE_STARTING(...)
#define CG_AVAILABLE_BUT_DEPRECATED(...)

CF_EXTERN_C_BEGIN

// -- CGBase.h, CGGeometry.h, CGAffineTransform.h ----------------------------

typedef double CGFloat;
#define CGFLOAT_IS_DOUBLE 1
#define CGFLOAT_MIN DBL_MIN
#define CGFLOAT_MAX DBL_MAX
#define CGFLOAT_DEFINED 1

struct CGPoint {
  CGFloat x;
  CGFloat y;
};
typedef struct CGPoint CGPoint;
struct CGSize {
  CGFloat width;
  CGFloat height;
};
typedef struct CGSize CGSize;
struct CGVector {
  CGFloat dx;
  CGFloat dy;
};
typedef struct CGVector CGVector;
struct CGRect {
  CGPoint origin;
  CGSize size;
};
typedef struct CGRect CGRect;

CG_EXTERN const CGPoint CGPointZero;
CG_EXTERN const CGSize CGSizeZero;
CG_EXTERN const CGRect CGRectZero;
CG_EXTERN const CGRect CGRectNull;
CG_EXTERN const CGRect CGRectInfinite;

CG_INLINE CGPoint CGPointMake(CGFloat x, CGFloat y) {
  CGPoint p;
  p.x = x;
  p.y = y;
  return p;
}
CG_INLINE CGSize CGSizeMake(CGFloat width, CGFloat height) {
  CGSize s;
  s.width = width;
  s.height = height;
  return s;
}
CG_INLINE CGRect CGRectMake(CGFloat x, CGFloat y, CGFloat w, CGFloat h) {
  CGRect r;
  r.origin.x = x;
  r.origin.y = y;
  r.size.width = w;
  r.size.height = h;
  return r;
}
CG_EXTERN CGFloat CGRectGetMinX(CGRect rect);
CG_EXTERN CGFloat CGRectGetMidX(CGRect rect);
CG_EXTERN CGFloat CGRectGetMaxX(CGRect rect);
CG_EXTERN CGFloat CGRectGetMinY(CGRect rect);
CG_EXTERN CGFloat CGRectGetMidY(CGRect rect);
CG_EXTERN CGFloat CGRectGetMaxY(CGRect rect);
CG_EXTERN CGFloat CGRectGetWidth(CGRect rect);
CG_EXTERN CGFloat CGRectGetHeight(CGRect rect);
CG_EXTERN bool CGPointEqualToPoint(CGPoint point1, CGPoint point2);
CG_EXTERN bool CGSizeEqualToSize(CGSize size1, CGSize size2);
CG_EXTERN bool CGRectEqualToRect(CGRect rect1, CGRect rect2);
CG_EXTERN bool CGRectIsEmpty(CGRect rect);
CG_EXTERN bool CGRectIsNull(CGRect rect);
CG_EXTERN CGRect CGRectInset(CGRect rect, CGFloat dx, CGFloat dy);
CG_EXTERN CGRect CGRectOffset(CGRect rect, CGFloat dx, CGFloat dy);
CG_EXTERN CGRect CGRectUnion(CGRect r1, CGRect r2);
CG_EXTERN CGRect CGRectIntersection(CGRect r1, CGRect r2);
CG_EXTERN bool CGRectContainsPoint(CGRect rect, CGPoint point);
CG_EXTERN bool CGRectIntersectsRect(CGRect rect1, CGRect rect2);

struct CGAffineTransform {
  CGFloat a, b, c, d;
  CGFloat tx, ty;
};
typedef struct CGAffineTransform CGAffineTransform;
CG_EXTERN const CGAffineTransform CGAffineTransformIdentity;
CG_EXTERN CGAffineTransform CGAffineTransformMake(CGFloat a,
                                                  CGFloat b,
                                                  CGFloat c,
                                                  CGFloat d,
                                                  CGFloat tx,
                                                  CGFloat ty);
CG_EXTERN CGAffineTransform CGAffineTransformMakeScale(CGFloat sx, CGFloat sy);
CG_EXTERN CGAffineTransform CGAffineTransformMakeTranslation(CGFloat tx,
                                                             CGFloat ty);
CG_EXTERN CGAffineTransform CGAffineTransformMakeRotation(CGFloat angle);
CG_EXTERN CGAffineTransform CGAffineTransformConcat(CGAffineTransform t1,
                                                    CGAffineTransform t2);
CG_EXTERN CGPoint CGPointApplyAffineTransform(CGPoint point,
                                              CGAffineTransform t);
CG_EXTERN CGRect CGRectApplyAffineTransform(CGRect rect, CGAffineTransform t);
CG_EXTERN bool CGAffineTransformIsIdentity(CGAffineTransform t);

// -- Opaque types -------------------------------------------------------------

typedef unsigned short CGGlyph;
typedef unsigned short CGFontIndex;
typedef int32_t CGError;
typedef uint32_t CGDirectDisplayID;
typedef uint32_t CGWindowID;
typedef struct CGFont* CGFontRef;
typedef struct CF_BRIDGED_TYPE(id) CGColor* CGColorRef;
typedef struct CF_BRIDGED_TYPE(id) CGColorSpace* CGColorSpaceRef;
typedef struct CF_BRIDGED_TYPE(id) CGContext* CGContextRef;
typedef struct CF_BRIDGED_TYPE(id) CGImage* CGImageRef;
typedef struct CF_BRIDGED_TYPE(id) CGPath* CGMutablePathRef;
typedef const struct CF_BRIDGED_TYPE(id) CGPath* CGPathRef;
typedef struct CGDataProvider* CGDataProviderRef;
typedef struct CF_BRIDGED_TYPE(id) CGEvent* CGEventRef;

CG_EXTERN CFTypeID CGFontGetTypeID(void);
CG_EXTERN CGFontRef CGFontRetain(CGFontRef font);
CG_EXTERN void CGFontRelease(CGFontRef font);
CG_EXTERN CFStringRef CGFontCopyPostScriptName(CGFontRef font);
CG_EXTERN CFStringRef CGFontCopyFullName(CGFontRef font);
CG_EXTERN int CGFontGetUnitsPerEm(CGFontRef font);
CG_EXTERN size_t CGFontGetNumberOfGlyphs(CGFontRef font);
CG_EXTERN CFDataRef CGFontCopyTableForTag(CGFontRef font, uint32_t tag);
CG_EXTERN CFArrayRef CGFontCopyTableTags(CGFontRef font);
CG_EXTERN CGFontRef CGFontCreateWithDataProvider(CGDataProviderRef provider);
CG_EXTERN CGFontRef CGFontCreateWithFontName(CFStringRef name);
CG_EXTERN CFArrayRef CGFontCopyVariationAxes(CGFontRef font);
CG_EXTERN CFDictionaryRef CGFontCopyVariations(CGFontRef font);

CG_EXTERN CFTypeID CGColorGetTypeID(void);
CG_EXTERN CGColorRef CGColorRetain(CGColorRef color);
CG_EXTERN void CGColorRelease(CGColorRef color);
CG_EXTERN CGColorRef CGColorCreateGenericRGB(CGFloat red,
                                             CGFloat green,
                                             CGFloat blue,
                                             CGFloat alpha);
CG_EXTERN size_t CGColorGetNumberOfComponents(CGColorRef color);
CG_EXTERN const CGFloat* CGColorGetComponents(CGColorRef color);
CG_EXTERN CGFloat CGColorGetAlpha(CGColorRef color);
CG_EXTERN CGColorSpaceRef CGColorSpaceCreateDeviceRGB(void);
CG_EXTERN CGColorSpaceRef CGColorSpaceCreateWithName(CFStringRef name);
CG_EXTERN void CGColorSpaceRelease(CGColorSpaceRef space);
CG_EXTERN const CFStringRef kCGColorSpaceSRGB;
CG_EXTERN const CFStringRef kCGColorSpaceDisplayP3;

CG_EXTERN CGDataProviderRef CGDataProviderCreateWithCFData(CFDataRef data);
CG_EXTERN void CGDataProviderRelease(CGDataProviderRef provider);

CG_EXTERN CGDirectDisplayID CGMainDisplayID(void);

CF_EXTERN_C_END

#endif  // COMPILE_CHECK_STAND_IN_COREGRAPHICS_H_
