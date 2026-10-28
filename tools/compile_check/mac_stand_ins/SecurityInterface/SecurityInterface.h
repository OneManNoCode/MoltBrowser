// compile_check STAND-IN for the macOS SDK's SecurityInterface framework
// (see ../README.md): the certificate panel Chromium's certificate viewer
// opens (components/remote_cocoa), with the SDK's names, types and
// signatures.
//
// Source: the SDK's SecurityInterface.framework headers SFCertificatePanel.h
// and SFCertificateTrustPanel.h, as Apple's documentation gives them
// (developer.apple.com/documentation/securityinterface).

#ifndef COMPILE_CHECK_STAND_IN_SECURITYINTERFACE_H_
#define COMPILE_CHECK_STAND_IN_SECURITYINTERFACE_H_

#import <AppKit/AppKit.h>
#import <Security/Security.h>

#if defined(__OBJC__)

// -- SFCertificatePanel.h ----------------------------------------------------

@interface SFCertificatePanel : NSPanel
+ (SFCertificatePanel*)sharedCertificatePanel;
- (NSInteger)runModalForTrust:(SecTrustRef)trust showGroup:(BOOL)showGroup;
- (NSInteger)runModalForCertificates:(NSArray*)certificates
                           showGroup:(BOOL)showGroup;
- (void)beginSheetForWindow:(NSWindow*)docWindow
              modalDelegate:(id)delegate
             didEndSelector:(SEL)didEndSelector
                contextInfo:(void*)contextInfo
                      trust:(SecTrustRef)trust
                  showGroup:(BOOL)showGroup;
- (void)beginSheetForWindow:(NSWindow*)docWindow
              modalDelegate:(id)delegate
             didEndSelector:(SEL)didEndSelector
                contextInfo:(void*)contextInfo
               certificates:(NSArray*)certificates
                  showGroup:(BOOL)showGroup;
- (void)setPolicies:(id)policies;
- (NSArray*)policies;
- (void)setDefaultButtonTitle:(NSString*)title;
- (void)setAlternateButtonTitle:(NSString*)title;
- (void)setShowsHelp:(BOOL)showsHelp;
- (BOOL)showsHelp;
@end

#endif  // __OBJC__

#endif  // COMPILE_CHECK_STAND_IN_SECURITYINTERFACE_H_
