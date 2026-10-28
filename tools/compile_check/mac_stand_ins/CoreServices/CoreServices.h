// compile_check STAND-IN for the macOS SDK's CoreServices umbrella framework
// (see ../README.md): CoreFoundation and the parts of its subframeworks
// (LaunchServices, ...) Chromium's Mac code and the checked files use, with
// the SDK's names, types and signatures.
//
// Source: the SDK's CoreServices.framework umbrella and the subframework
// headers named in each section below, as Apple's documentation gives them
// (developer.apple.com/documentation/coreservices).

#ifndef COMPILE_CHECK_STAND_IN_CORESERVICES_H_
#define COMPILE_CHECK_STAND_IN_CORESERVICES_H_

#include <CoreFoundation/CoreFoundation.h>

#endif  // COMPILE_CHECK_STAND_IN_CORESERVICES_H_
