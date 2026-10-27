// compile_check STAND-IN for the macOS SDK's QuartzCore framework (Core
// Animation; see ../README.md): the layer and transaction API Chromium's Mac
// code and the checked files use, with the SDK's names, types and
// signatures.
//
// Source: the SDK's QuartzCore.framework headers named in each section
// below, as Apple's documentation gives them
// (developer.apple.com/documentation/quartzcore).

#ifndef COMPILE_CHECK_STAND_IN_QUARTZCORE_H_
#define COMPILE_CHECK_STAND_IN_QUARTZCORE_H_

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurfaceRef.h>

#if defined(__OBJC__)
#import <Foundation/Foundation.h>
#endif

// -- CABase.h ----------------------------------------------------------------

#define CA_EXTERN FOUNDATION_EXTERN
#define CA_EXTERN_C_BEGIN CF_EXTERN_C_BEGIN
#define CA_EXTERN_C_END CF_EXTERN_C_END

CA_EXTERN_C_BEGIN
CA_EXTERN CFTimeInterval CACurrentMediaTime(void);
CA_EXTERN_C_END

// -- CATransform3D.h ---------------------------------------------------------

struct CATransform3D {
  CGFloat m11, m12, m13, m14;
  CGFloat m21, m22, m23, m24;
  CGFloat m31, m32, m33, m34;
  CGFloat m41, m42, m43, m44;
};
typedef struct CATransform3D CATransform3D;

CA_EXTERN_C_BEGIN
CA_EXTERN const CATransform3D CATransform3DIdentity;
CA_EXTERN bool CATransform3DIsIdentity(CATransform3D t);
CA_EXTERN CATransform3D CATransform3DMakeTranslation(CGFloat tx, CGFloat ty,
                                                     CGFloat tz);
CA_EXTERN CATransform3D CATransform3DMakeScale(CGFloat sx, CGFloat sy,
                                               CGFloat sz);
CA_EXTERN CATransform3D CATransform3DConcat(CATransform3D a, CATransform3D b);
CA_EXTERN CATransform3D CATransform3DMakeAffineTransform(CGAffineTransform m);
CA_EXTERN_C_END

#if defined(__OBJC__)

NS_ASSUME_NONNULL_BEGIN

// -- CAMediaTiming.h ---------------------------------------------------------

typedef NSString* CAMediaTimingFillMode NS_TYPED_ENUM;

@protocol CAMediaTiming
@property CFTimeInterval beginTime;
@property CFTimeInterval duration;
@property float speed;
@property CFTimeInterval timeOffset;
@property float repeatCount;
@property CFTimeInterval repeatDuration;
@property BOOL autoreverses;
@property(copy) CAMediaTimingFillMode fillMode;
@end

// -- CATransaction.h ---------------------------------------------------------

@interface CATransaction : NSObject
+ (void)begin;
+ (void)commit;
+ (void)flush;
+ (void)lock;
+ (void)unlock;
+ (CFTimeInterval)animationDuration;
+ (void)setAnimationDuration:(CFTimeInterval)dur;
+ (BOOL)disableActions;
+ (void)setDisableActions:(BOOL)flag;
+ (nullable void (^)(void))completionBlock;
+ (void)setCompletionBlock:(nullable void (^)(void))block;
+ (nullable id)valueForKey:(NSString*)key;
+ (void)setValue:(nullable id)anObject forKey:(NSString*)key;
@end

CA_EXTERN NSString* const kCATransactionAnimationDuration;
CA_EXTERN NSString* const kCATransactionDisableActions;
CA_EXTERN NSString* const kCATransactionAnimationTimingFunction;
CA_EXTERN NSString* const kCATransactionCompletionBlock;

// -- CALayer.h ---------------------------------------------------------------

typedef NSString* CALayerContentsGravity NS_TYPED_ENUM;
typedef NSString* CALayerContentsFormat NS_TYPED_ENUM;
typedef NSString* CALayerContentsFilter NS_TYPED_ENUM;
typedef NSString* CALayerCornerCurve NS_TYPED_ENUM;

typedef NS_OPTIONS(unsigned int, CAAutoresizingMask){
    kCALayerNotSizable = 0,     kCALayerMinXMargin = 1U << 0,
    kCALayerWidthSizable = 1U << 1, kCALayerMaxXMargin = 1U << 2,
    kCALayerMinYMargin = 1U << 3, kCALayerHeightSizable = 1U << 4,
    kCALayerMaxYMargin = 1U << 5};

typedef NS_OPTIONS(unsigned int, CAEdgeAntialiasingMask){
    kCALayerLeftEdge = 1U << 0, kCALayerRightEdge = 1U << 1,
    kCALayerBottomEdge = 1U << 2, kCALayerTopEdge = 1U << 3};

typedef NS_OPTIONS(NSUInteger, CACornerMask){
    kCALayerMinXMinYCorner = 1U << 0, kCALayerMaxXMinYCorner = 1U << 1,
    kCALayerMinXMaxYCorner = 1U << 2, kCALayerMaxXMaxYCorner = 1U << 3};

@class CAAnimation, CALayer;

@protocol CAAction
- (void)runActionForKey:(NSString*)event
                 object:(id)anObject
              arguments:(nullable NSDictionary*)dict;
@end

@protocol CALayerDelegate <NSObject>
@optional
- (void)displayLayer:(CALayer*)layer;
- (void)drawLayer:(CALayer*)layer inContext:(CGContextRef)ctx;
- (void)layerWillDraw:(CALayer*)layer;
- (void)layoutSublayersOfLayer:(CALayer*)layer;
- (nullable id<CAAction>)actionForLayer:(CALayer*)layer
                                 forKey:(NSString*)event;
@end

@interface CALayer : NSObject <NSSecureCoding, CAMediaTiming>
+ (instancetype)layer;
- (instancetype)init;
- (instancetype)initWithLayer:(id)layer;
@property CGRect bounds;
@property CGPoint position;
@property CGFloat zPosition;
@property CGPoint anchorPoint;
@property CATransform3D transform;
@property CGRect frame;
@property(getter=isHidden) BOOL hidden;
@property(getter=isDoubleSided) BOOL doubleSided;
@property(getter=isGeometryFlipped) BOOL geometryFlipped;
@property(nullable, readonly) CALayer* superlayer;
- (void)removeFromSuperlayer;
@property(nullable, copy) NSArray<__kindof CALayer*>* sublayers;
- (void)addSublayer:(CALayer*)layer;
- (void)insertSublayer:(CALayer*)layer atIndex:(unsigned)idx;
- (void)insertSublayer:(CALayer*)layer below:(nullable CALayer*)sibling;
- (void)insertSublayer:(CALayer*)layer above:(nullable CALayer*)sibling;
- (void)replaceSublayer:(CALayer*)oldLayer with:(CALayer*)newLayer;
@property(nullable, strong) CALayer* mask;
@property BOOL masksToBounds;
@property(nullable, strong) id contents;
@property CGRect contentsRect;
@property(copy) CALayerContentsGravity contentsGravity;
@property CGFloat contentsScale;
@property CGRect contentsCenter;
@property(copy) CALayerContentsFormat contentsFormat;
@property(copy) CALayerContentsFilter minificationFilter;
@property(copy) CALayerContentsFilter magnificationFilter;
@property(getter=isOpaque) BOOL opaque;
- (void)display;
- (void)setNeedsDisplay;
- (void)setNeedsDisplayInRect:(CGRect)r;
- (BOOL)needsDisplay;
- (void)displayIfNeeded;
@property BOOL needsDisplayOnBoundsChange;
@property BOOL drawsAsynchronously;
- (void)drawInContext:(CGContextRef)ctx;
- (void)renderInContext:(CGContextRef)ctx;
@property CAEdgeAntialiasingMask edgeAntialiasingMask;
@property BOOL allowsEdgeAntialiasing;
@property(nullable) CGColorRef backgroundColor;
@property CGFloat cornerRadius;
@property CACornerMask maskedCorners;
@property(copy) CALayerCornerCurve cornerCurve;
@property CGFloat borderWidth;
@property(nullable) CGColorRef borderColor;
@property float opacity;
@property BOOL allowsGroupOpacity;
@property(nullable, strong) id compositingFilter;
@property(nullable, copy) NSArray* filters;
@property(nullable, copy) NSArray* backgroundFilters;
@property BOOL shouldRasterize;
@property CGFloat rasterizationScale;
@property(nullable) CGColorRef shadowColor;
@property float shadowOpacity;
@property CGSize shadowOffset;
@property CGFloat shadowRadius;
@property(nullable) CGPathRef shadowPath;
@property CAAutoresizingMask autoresizingMask;
- (void)setNeedsLayout;
- (BOOL)needsLayout;
- (void)layoutIfNeeded;
- (void)layoutSublayers;
+ (nullable id<CAAction>)defaultActionForKey:(NSString*)event;
- (nullable id<CAAction>)actionForKey:(NSString*)event;
@property(nullable, copy) NSDictionary<NSString*, id<CAAction>>* actions;
- (void)addAnimation:(CAAnimation*)anim forKey:(nullable NSString*)key;
- (void)removeAllAnimations;
- (void)removeAnimationForKey:(NSString*)key;
- (nullable NSArray<NSString*>*)animationKeys;
- (nullable __kindof CAAnimation*)animationForKey:(NSString*)key;
@property(nullable, copy) NSString* name;
@property(nullable, weak) id<CALayerDelegate> delegate;
@property(nullable, copy) NSDictionary* style;
- (CGPoint)convertPoint:(CGPoint)p fromLayer:(nullable CALayer*)l;
- (CGPoint)convertPoint:(CGPoint)p toLayer:(nullable CALayer*)l;
- (CGRect)convertRect:(CGRect)r fromLayer:(nullable CALayer*)l;
- (CGRect)convertRect:(CGRect)r toLayer:(nullable CALayer*)l;
@end

CA_EXTERN CALayerContentsGravity const kCAGravityCenter;
CA_EXTERN CALayerContentsGravity const kCAGravityTop;
CA_EXTERN CALayerContentsGravity const kCAGravityBottom;
CA_EXTERN CALayerContentsGravity const kCAGravityLeft;
CA_EXTERN CALayerContentsGravity const kCAGravityRight;
CA_EXTERN CALayerContentsGravity const kCAGravityTopLeft;
CA_EXTERN CALayerContentsGravity const kCAGravityTopRight;
CA_EXTERN CALayerContentsGravity const kCAGravityBottomLeft;
CA_EXTERN CALayerContentsGravity const kCAGravityBottomRight;
CA_EXTERN CALayerContentsGravity const kCAGravityResize;
CA_EXTERN CALayerContentsGravity const kCAGravityResizeAspect;
CA_EXTERN CALayerContentsGravity const kCAGravityResizeAspectFill;
CA_EXTERN CALayerContentsFilter const kCAFilterNearest;
CA_EXTERN CALayerContentsFilter const kCAFilterLinear;
CA_EXTERN CALayerContentsFilter const kCAFilterTrilinear;

NS_ASSUME_NONNULL_END

#endif  // __OBJC__

#endif  // COMPILE_CHECK_STAND_IN_QUARTZCORE_H_
