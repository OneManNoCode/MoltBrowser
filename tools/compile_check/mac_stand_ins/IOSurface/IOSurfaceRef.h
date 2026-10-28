// compile_check STAND-IN for the macOS SDK's IOSurface framework (see
// ../README.md): the IOSurfaceRef C API Chromium's Mac code uses
// (ui/gfx/mac/io_surface.h and its includers), with the SDK's names, types
// and signatures.
//
// Source: the SDK's IOSurface.framework headers IOSurfaceTypes.h,
// IOSurfaceBase.h and IOSurfaceRef.h, as Apple's documentation gives them
// (developer.apple.com/documentation/iosurface).

#ifndef COMPILE_CHECK_STAND_IN_IOSURFACEREF_H_
#define COMPILE_CHECK_STAND_IN_IOSURFACEREF_H_

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <stddef.h>
#include <stdint.h>

// -- IOSurfaceBase.h ---------------------------------------------------------

#define IOSFC_AVAILABLE_STARTING(_mac, _iphone)
#define IOSFC_DEPRECATED_MSG(...)

// -- IOSurfaceTypes.h --------------------------------------------------------

typedef uint32_t IOSurfaceID;

typedef CF_OPTIONS(uint32_t, IOSurfaceLockOptions){
    kIOSurfaceLockReadOnly = 0x00000001,
    kIOSurfaceLockAvoidSync = 0x00000002,
};

typedef CF_OPTIONS(uint32_t, IOSurfacePurgeabilityState){
    kIOSurfacePurgeableNonVolatile = 0,
    kIOSurfacePurgeableVolatile = 1,
    kIOSurfacePurgeableEmpty = 2,
    kIOSurfacePurgeableKeepCurrent = 3,
};

enum {
  kIOSurfaceDefaultCache = 0,
  kIOSurfaceInhibitCache = 1,
  kIOSurfaceWriteThruCache = 2,
  kIOSurfaceCopybackCache = 3,
  kIOSurfaceWriteCombineCache = 4,
  kIOSurfaceCopybackInnerCache = 5,
};

enum {
  kIOSurfaceMapCacheShift = 8,
  kIOSurfaceMapDefaultCache = kIOSurfaceDefaultCache << kIOSurfaceMapCacheShift,
  kIOSurfaceMapInhibitCache = kIOSurfaceInhibitCache << kIOSurfaceMapCacheShift,
  kIOSurfaceMapWriteThruCache = kIOSurfaceWriteThruCache
                                << kIOSurfaceMapCacheShift,
  kIOSurfaceMapCopybackCache = kIOSurfaceCopybackCache
                               << kIOSurfaceMapCacheShift,
  kIOSurfaceMapWriteCombineCache = kIOSurfaceWriteCombineCache
                                   << kIOSurfaceMapCacheShift,
  kIOSurfaceMapCopybackInnerCache = kIOSurfaceCopybackInnerCache
                                    << kIOSurfaceMapCacheShift,
};

typedef CF_ENUM(int32_t, IOSurfaceComponentName){
    kIOSurfaceComponentNameUnknown = 0,
    kIOSurfaceComponentNameAlpha = 1,
    kIOSurfaceComponentNameRed = 2,
    kIOSurfaceComponentNameGreen = 3,
    kIOSurfaceComponentNameBlue = 4,
    kIOSurfaceComponentNameLuma = 5,
    kIOSurfaceComponentNameChromaRed = 6,
    kIOSurfaceComponentNameChromaBlue = 7,
};

typedef CF_ENUM(int32_t, IOSurfaceSubsampling){
    kIOSurfaceSubsamplingUnknown = 0,
    kIOSurfaceSubsamplingNone = 1,
    kIOSurfaceSubsampling422 = 2,
    kIOSurfaceSubsampling420 = 3,
    kIOSurfaceSubsampling411 = 4,
};

// -- IOSurfaceRef.h ----------------------------------------------------------

CF_EXTERN_C_BEGIN

typedef struct CF_BRIDGED_TYPE(id) __IOSurface* IOSurfaceRef;

CF_EXPORT const CFStringRef kIOSurfaceAllocSize;
CF_EXPORT const CFStringRef kIOSurfaceWidth;
CF_EXPORT const CFStringRef kIOSurfaceHeight;
CF_EXPORT const CFStringRef kIOSurfaceBytesPerRow;
CF_EXPORT const CFStringRef kIOSurfaceBytesPerElement;
CF_EXPORT const CFStringRef kIOSurfaceElementWidth;
CF_EXPORT const CFStringRef kIOSurfaceElementHeight;
CF_EXPORT const CFStringRef kIOSurfaceOffset;
CF_EXPORT const CFStringRef kIOSurfacePlaneInfo;
CF_EXPORT const CFStringRef kIOSurfacePlaneWidth;
CF_EXPORT const CFStringRef kIOSurfacePlaneHeight;
CF_EXPORT const CFStringRef kIOSurfacePlaneBytesPerRow;
CF_EXPORT const CFStringRef kIOSurfacePlaneOffset;
CF_EXPORT const CFStringRef kIOSurfacePlaneSize;
CF_EXPORT const CFStringRef kIOSurfacePlaneBase;
CF_EXPORT const CFStringRef kIOSurfacePlaneBitsPerElement;
CF_EXPORT const CFStringRef kIOSurfacePlaneBytesPerElement;
CF_EXPORT const CFStringRef kIOSurfacePlaneElementWidth;
CF_EXPORT const CFStringRef kIOSurfacePlaneElementHeight;
CF_EXPORT const CFStringRef kIOSurfaceCacheMode;
CF_EXPORT const CFStringRef kIOSurfaceIsGlobal;
CF_EXPORT const CFStringRef kIOSurfacePixelFormat;
CF_EXPORT const CFStringRef kIOSurfacePixelSizeCastingAllowed;
CF_EXPORT const CFStringRef kIOSurfacePlaneComponentBitDepths;
CF_EXPORT const CFStringRef kIOSurfacePlaneComponentBitOffsets;
CF_EXPORT const CFStringRef kIOSurfaceName;

CF_EXPORT CFTypeID IOSurfaceGetTypeID(void);
CF_EXPORT IOSurfaceRef _Nullable IOSurfaceCreate(CFDictionaryRef properties)
    CF_RETURNS_RETAINED;
CF_EXPORT IOSurfaceRef _Nullable IOSurfaceLookup(IOSurfaceID csid)
    CF_RETURNS_RETAINED;
CF_EXPORT IOSurfaceID IOSurfaceGetID(IOSurfaceRef buffer);
CF_EXPORT kern_return_t IOSurfaceLock(IOSurfaceRef buffer,
                                      IOSurfaceLockOptions options,
                                      uint32_t* _Nullable seed);
CF_EXPORT kern_return_t IOSurfaceUnlock(IOSurfaceRef buffer,
                                        IOSurfaceLockOptions options,
                                        uint32_t* _Nullable seed);
CF_EXPORT size_t IOSurfaceGetAllocSize(IOSurfaceRef buffer);
CF_EXPORT size_t IOSurfaceGetWidth(IOSurfaceRef buffer);
CF_EXPORT size_t IOSurfaceGetHeight(IOSurfaceRef buffer);
CF_EXPORT size_t IOSurfaceGetBytesPerElement(IOSurfaceRef buffer);
CF_EXPORT size_t IOSurfaceGetBytesPerRow(IOSurfaceRef buffer);
CF_EXPORT void* IOSurfaceGetBaseAddress(IOSurfaceRef buffer);
CF_EXPORT size_t IOSurfaceGetElementWidth(IOSurfaceRef buffer);
CF_EXPORT size_t IOSurfaceGetElementHeight(IOSurfaceRef buffer);
CF_EXPORT OSType IOSurfaceGetPixelFormat(IOSurfaceRef buffer);
CF_EXPORT uint32_t IOSurfaceGetSeed(IOSurfaceRef buffer);
CF_EXPORT size_t IOSurfaceGetPlaneCount(IOSurfaceRef buffer);
CF_EXPORT size_t IOSurfaceGetWidthOfPlane(IOSurfaceRef buffer,
                                          size_t planeIndex);
CF_EXPORT size_t IOSurfaceGetHeightOfPlane(IOSurfaceRef buffer,
                                           size_t planeIndex);
CF_EXPORT size_t IOSurfaceGetBytesPerElementOfPlane(IOSurfaceRef buffer,
                                                    size_t planeIndex);
CF_EXPORT size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef buffer,
                                                size_t planeIndex);
CF_EXPORT void* IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef buffer,
                                               size_t planeIndex);
CF_EXPORT size_t IOSurfaceGetElementWidthOfPlane(IOSurfaceRef buffer,
                                                 size_t planeIndex);
CF_EXPORT size_t IOSurfaceGetElementHeightOfPlane(IOSurfaceRef buffer,
                                                  size_t planeIndex);
CF_EXPORT size_t IOSurfaceGetNumberOfComponentsOfPlane(IOSurfaceRef buffer,
                                                       size_t planeIndex);
CF_EXPORT IOSurfaceComponentName IOSurfaceGetNameOfComponentOfPlane(
    IOSurfaceRef buffer, size_t planeIndex, size_t componentIndex);
CF_EXPORT size_t IOSurfaceGetBitDepthOfComponentOfPlane(IOSurfaceRef buffer,
                                                        size_t planeIndex,
                                                        size_t componentIndex);
CF_EXPORT size_t IOSurfaceGetBitOffsetOfComponentOfPlane(
    IOSurfaceRef buffer, size_t planeIndex, size_t componentIndex);
CF_EXPORT IOSurfaceSubsampling IOSurfaceGetSubsampling(IOSurfaceRef buffer);
CF_EXPORT void IOSurfaceSetValue(IOSurfaceRef buffer, CFStringRef key,
                                 CFTypeRef value);
CF_EXPORT CFTypeRef _Nullable IOSurfaceCopyValue(IOSurfaceRef buffer,
                                                 CFStringRef key)
    CF_RETURNS_RETAINED;
CF_EXPORT void IOSurfaceRemoveValue(IOSurfaceRef buffer, CFStringRef key);
CF_EXPORT void IOSurfaceSetValues(IOSurfaceRef buffer, CFDictionaryRef keysAndValues);
CF_EXPORT CFDictionaryRef _Nullable IOSurfaceCopyAllValues(
    IOSurfaceRef buffer) CF_RETURNS_RETAINED;
CF_EXPORT void IOSurfaceRemoveAllValues(IOSurfaceRef buffer);
CF_EXPORT mach_port_t IOSurfaceCreateMachPort(IOSurfaceRef buffer);
CF_EXPORT IOSurfaceRef _Nullable IOSurfaceLookupFromMachPort(mach_port_t port)
    CF_RETURNS_RETAINED;
CF_EXPORT size_t IOSurfaceGetPropertyMaximum(CFStringRef property);
CF_EXPORT size_t IOSurfaceGetPropertyAlignment(CFStringRef property);
CF_EXPORT size_t IOSurfaceAlignProperty(CFStringRef property, size_t value);
CF_EXPORT void IOSurfaceIncrementUseCount(IOSurfaceRef buffer);
CF_EXPORT void IOSurfaceDecrementUseCount(IOSurfaceRef buffer);
CF_EXPORT int32_t IOSurfaceGetUseCount(IOSurfaceRef buffer);
CF_EXPORT Boolean IOSurfaceIsInUse(IOSurfaceRef buffer);
CF_EXPORT Boolean IOSurfaceAllowsPixelSizeCasting(IOSurfaceRef buffer);
CF_EXPORT kern_return_t IOSurfaceSetPurgeable(
    IOSurfaceRef buffer, uint32_t newState, uint32_t* _Nullable oldState);

CF_EXTERN_C_END

#endif  // COMPILE_CHECK_STAND_IN_IOSURFACEREF_H_
