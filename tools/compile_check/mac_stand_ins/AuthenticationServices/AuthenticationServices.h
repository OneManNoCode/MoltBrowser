// compile_check STAND-IN for the macOS SDK's AuthenticationServices
// framework (see ../README.md): the web authentication session API a
// browser implements (Chromium's AppController handles the system's
// ASWebAuthenticationSession requests), with the SDK's names, types and
// signatures.
//
// Source: the SDK headers named in each section below, as Apple's
// documentation gives them (developer.apple.com/documentation/
// authenticationservices: ASWebAuthenticationSession,
// ASWebAuthenticationSessionRequest, ASWebAuthenticationSessionCallback,
// ASWebAuthenticationSessionWebBrowserSessionManager and
// ASWebAuthenticationSessionWebBrowserSessionHandling).

#ifndef COMPILE_CHECK_STAND_IN_AUTHENTICATIONSERVICES_H_
#define COMPILE_CHECK_STAND_IN_AUTHENTICATIONSERVICES_H_

#import <Foundation/Foundation.h>

// -- ASFoundation.h ----------------------------------------------------------

#if defined(__cplusplus)
#define AS_EXTERN extern "C" __attribute__((visibility("default")))
#else
#define AS_EXTERN extern __attribute__((visibility("default")))
#endif

#if defined(__OBJC__)

NS_ASSUME_NONNULL_BEGIN

// -- ASWebAuthenticationSession.h (macOS 10.15) ------------------------------

AS_EXTERN NSErrorDomain const ASWebAuthenticationSessionErrorDomain
    API_AVAILABLE(macos(10.15));

typedef NS_ERROR_ENUM(ASWebAuthenticationSessionErrorDomain,
                      ASWebAuthenticationSessionErrorCode){
    ASWebAuthenticationSessionErrorCodeCanceledLogin = 1,
    ASWebAuthenticationSessionErrorCodePresentationContextNotProvided = 2,
    ASWebAuthenticationSessionErrorCodePresentationContextInvalid = 3,
} API_AVAILABLE(macos(10.15));

typedef void (^ASWebAuthenticationSessionCompletionHandler)(
    NSURL* _Nullable callbackURL, NSError* _Nullable error);

@class ASWebAuthenticationSessionCallback;

API_AVAILABLE(macos(10.15))
@interface ASWebAuthenticationSession : NSObject
- (instancetype)initWithURL:(NSURL*)URL
          callbackURLScheme:(nullable NSString*)callbackURLScheme
          completionHandler:
              (ASWebAuthenticationSessionCompletionHandler)completionHandler;
- (instancetype)initWithURL:(NSURL*)URL
                   callback:(ASWebAuthenticationSessionCallback*)callback
          completionHandler:
              (ASWebAuthenticationSessionCompletionHandler)completionHandler
    API_AVAILABLE(macos(14.4));
@property(nonatomic) BOOL prefersEphemeralWebBrowserSession;
@property(nonatomic, nullable, copy)
    NSDictionary<NSString*, NSString*>* additionalHeaderFields
        API_AVAILABLE(macos(14.4));
@property(nonatomic, readonly) BOOL canStart;
- (BOOL)start;
- (void)cancel;
+ (instancetype)new NS_UNAVAILABLE;
- (instancetype)init NS_UNAVAILABLE;
@end

// -- ASWebAuthenticationSessionCallback.h (macOS 14.4) -----------------------

API_AVAILABLE(macos(14.4))
@interface ASWebAuthenticationSessionCallback : NSObject
+ (instancetype)callbackWithCustomScheme:(NSString*)customScheme;
+ (instancetype)callbackWithHTTPSHost:(NSString*)host path:(NSString*)path;
- (BOOL)matchesURL:(NSURL*)url;
+ (instancetype)new NS_UNAVAILABLE;
- (instancetype)init NS_UNAVAILABLE;
@end

// -- ASWebAuthenticationSessionRequest.h (macOS 10.15) -----------------------

@class ASWebAuthenticationSessionRequest;

API_AVAILABLE(macos(10.15))
@protocol ASWebAuthenticationSessionRequestDelegate <NSObject>
@optional
- (void)authenticationSessionRequest:
            (ASWebAuthenticationSessionRequest*)authenticationSessionRequest
          didCompleteWithCallbackURL:(NSURL*)callbackURL;
- (void)authenticationSessionRequest:
            (ASWebAuthenticationSessionRequest*)authenticationSessionRequest
                  didCancelWithError:(NSError*)error;
@end

API_AVAILABLE(macos(10.15))
@interface ASWebAuthenticationSessionRequest
    : NSObject <NSSecureCoding, NSCopying>
@property(readonly, nonatomic) NSUUID* UUID;
@property(readonly, nonatomic) NSURL* URL;
@property(readonly, nonatomic, nullable) NSString* callbackURLScheme;
@property(readonly, nonatomic) BOOL shouldUseEphemeralSession;
@property(nonatomic, readonly, nullable)
    ASWebAuthenticationSessionCallback* callback API_AVAILABLE(macos(14.4));
@property(nonatomic, readonly, nullable)
    NSDictionary<NSString*, NSString*>* additionalHeaderFields
        API_AVAILABLE(macos(14.4));
@property(nonatomic, weak, nullable)
    id<ASWebAuthenticationSessionRequestDelegate> delegate;
- (void)cancelWithError:(NSError*)error;
- (void)completeWithCallbackURL:(NSURL*)url;
+ (instancetype)new NS_UNAVAILABLE;
- (instancetype)init NS_UNAVAILABLE;
@end

// -- ASWebAuthenticationSessionWebBrowserSessionHandling.h (macOS 10.15) -----

API_AVAILABLE(macos(10.15))
@protocol ASWebAuthenticationSessionWebBrowserSessionHandling
- (void)beginHandlingWebAuthenticationSessionRequest:
    (nullable ASWebAuthenticationSessionRequest*)request;
- (void)cancelWebAuthenticationSessionRequest:
    (nullable ASWebAuthenticationSessionRequest*)request;
@end

// -- ASWebAuthenticationSessionWebBrowserSessionManager.h (macOS 10.15) ------

API_AVAILABLE(macos(10.15))
@interface ASWebAuthenticationSessionWebBrowserSessionManager : NSObject
@property(class, readonly, strong)
    ASWebAuthenticationSessionWebBrowserSessionManager* sharedManager;
@property(nonatomic, weak)
    id<ASWebAuthenticationSessionWebBrowserSessionHandling> sessionHandler;
@property(nonatomic, readonly) BOOL wasLaunchedByAuthenticationServices;
@end

NS_ASSUME_NONNULL_END

#endif  // __OBJC__

#endif  // COMPILE_CHECK_STAND_IN_AUTHENTICATIONSERVICES_H_
