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
include use, with Apple's real names, types and signatures (as the SDK
documents them). A file checked against them is reported as "partly on
stand-ins": a call with a wrong argument type is caught, but an API this
directory has and the real SDK lacks (or declares differently) is not.
The build machine's Xcode stays the authority.

`usr_include/` holds the few libSystem headers the Zig toolchain's set
leaves out (`mach/mach_vm.h`, MIG-generated in the SDK), linked into the
SDK's `usr/include`.

What is here: CoreFoundation, Foundation, CoreGraphics, CoreText, AppKit
(fonts), Security (certificates, the keychain item API, code signing
requirements), Accelerate (vDSP's biquad), ApplicationServices and the
Cocoa umbrella. A Mac file that needs more of the SDK (most of AppKit,
AuthenticationServices, IOSurface, QuartzCore, ...) is reported as not
checked, with the declarations or framework it needs.

When a check fails with an undeclared Apple identifier, add its real
declaration here, in the framework the SDK has it in.
