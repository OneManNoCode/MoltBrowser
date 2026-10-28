# Apple framework stand-ins

The macOS check (`tools/compile_check`, platform `mac`) compiles against
Darwin's real libc and libSystem headers (`mach/`, `os/`, `dispatch/`,
`objc/`, `sys/`, from the Zig toolchain's PyPI wheel, see `ccheck/sdks.py`)
but the Apple frameworks' headers (CoreFoundation, Foundation, AppKit,
CoreText, CoreGraphics, Security, ...) are part of the macOS SDK, which is
only distributed with Xcode and can't be fetched here. These directories
stand in for them: one per framework, laid out as its `Headers/`
directory (`<CoreText/CoreText.h>` is `CoreText/CoreText.h`).

Each declares only what the checked files and the Chromium headers they
include use, with Apple's real names, types, values and signatures, and
says in its first lines which SDK headers (and which of Apple's
documentation pages) its declarations come from; inside a file, each
section is headed by the SDK header it stands in for (`// -- NSWindow.h`).
A file checked against them is reported as "partly on stand-ins": a call
with a wrong argument type, a missing method or an enum constant that
doesn't exist is caught, but an API this directory has and the real SDK
lacks (or declares differently) is not, and availability (`API_AVAILABLE`)
is not modeled. The build machine's Xcode stays the authority.

`usr_include/` holds the few libSystem headers the Zig toolchain's set
leaves out (`mach/mach_vm.h`, MIG-generated in the SDK), linked into the
SDK's `usr/include`.

What is here: CoreFoundation, Foundation (with NSGeometry, keyed
archiving, key-value observing and NSUserActivity), CoreGraphics (with
window levels), CoreText, AppKit (the application and its delegate,
windows, panels, views, responders, events, tracking areas, cursors,
menus, the workspace and running applications, colors, images,
appearances, animation, drag and drop, the pasteboard, alerts, popovers,
toolbars, open and save panels, sharing services, visual effect and glass
views, fonts), QuartzCore (layers and transactions), IOSurface (the
IOSurfaceRef C API), AuthenticationServices (the web authentication
session a browser handles), Security (certificates, the keychain item API,
code signing requirements), SecurityInterface (the certificate panel),
CoreServices (its umbrella), Accelerate (vDSP's biquad),
ApplicationServices and the Cocoa umbrella. A Mac file that needs more of
the SDK (Metal, AVFoundation, UserNotifications, ...) is reported as not
checked, with the declarations or framework it needs.

When a check fails with an undeclared Apple identifier, add its real
declaration here, in the framework and section the SDK has it in.

## Why not real headers

Apple's SDK licence restricts redistributing the SDK and using it on
non-Apple hardware, so no Xcode SDK is downloaded. What can be had legally
and reproducibly doesn't cover the frameworks: the Zig toolchain's Darwin
headers (used above) and LLVM's are libc and libSystem only; Apple's
open-source releases of CoreFoundation (CF-Lite, and
swift-corelibs-foundation's fork of it) and Security are older or diverged
versions of the SDK's headers, and AppKit, Foundation, QuartzCore,
IOSurface and AuthenticationServices are not published at all. Every Mac
file reaches Foundation or AppKit, so swapping one framework for an older
real copy would not make any file "checked", and would mix two versions
of the same API. GNUstep's headers are a different implementation of the
API, not Apple's declarations.
