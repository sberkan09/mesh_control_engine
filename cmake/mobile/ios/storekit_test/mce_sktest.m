// StoreKit testing in the iOS Simulator without Xcode's Run button (M-MONETIZE, SBA-26).
//
// Xcode applies a StoreKit configuration (.storekit) only to apps it launches from a scheme. This
// library does the same for any app: injected into it at launch, it opens an SKTestSession over the
// configuration file, so StoreKit 2 answers from that file (local products, purchases, restore) and
// no App Store account is needed. Test-only: never linked into an app, the engine knows nothing of it.
//
//   python3 cmake/mobile/ios/storekit_test/run.py <app id> <file.storekit> [-- <app args>]
//
// Environment (simctl passes SIMCTL_CHILD_<name> to the app as <name>):
//   MCE_SKTEST_CONFIG       the .storekit file (a host path; Simulator apps read the host's files)
//   MCE_SKTEST_RESET=1      forget every earlier test transaction first (a fresh install)
//   MCE_SKTEST_DIALOGS=0    no purchase sheet: every purchase succeeds without a tap
//   MCE_SKTEST_ASK_TO_BUY=1 purchases wait for approval (Pending)
//   MCE_SKTEST_INTERRUPT=1  purchases are interrupted (finished later, outside purchase())

#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

@protocol MceSKTestSession
- (instancetype)initWithContentsOfURL:(NSURL*)url error:(NSError**)error;
- (void)setDisableDialogs:(BOOL)v;
- (void)setAskToBuyEnabled:(BOOL)v;
- (void)setInterruptedPurchasesEnabled:(BOOL)v;
- (void)resetToDefaultState;
- (void)clearTransactions;
@end

static id<MceSKTestSession> g_session;  // the session lives as long as the app

// SKTestSession asks XCTest which app it tests (XCTestConfiguration); outside a test run XCTest is not
// there and the soft link aborts. The app under test is this one.
static NSString* mce_sktest_bundle_id(id self, SEL cmd) {
    (void)self;
    (void)cmd;
    return [[NSBundle mainBundle] bundleIdentifier];
}

static int flag(const char* name) {
    const char* v = getenv(name);
    return v && strcmp(v, "1") == 0;
}

__attribute__((constructor)) static void mce_sktest_start(void) {
    const char* config = getenv("MCE_SKTEST_CONFIG");
    if (!config || !*config) return;
    if (!dlopen("/Developer/Library/Frameworks/StoreKitTest.framework/StoreKitTest", RTLD_NOW)) {
        NSLog(@"mce_sktest: cannot load StoreKitTest: %s", dlerror());
        return;
    }
    Class cls = NSClassFromString(@"SKTestSession");
    Method bundle_id = cls ? class_getInstanceMethod(cls, NSSelectorFromString(@"bundleID")) : NULL;
    if (!bundle_id) {
        NSLog(@"mce_sktest: this StoreKitTest has no SKTestSession.bundleID to replace");
        return;
    }
    method_setImplementation(bundle_id, (IMP)mce_sktest_bundle_id);
    NSError* error = nil;
    id<MceSKTestSession> s = [(id<MceSKTestSession>)[cls alloc] initWithContentsOfURL:[NSURL fileURLWithPath:@(config)]
                                                                                 error:&error];
    if (!s) {
        NSLog(@"mce_sktest: cannot open %s: %@", config, error);
        return;
    }
    if (flag("MCE_SKTEST_RESET")) {
        [s resetToDefaultState];
        [s clearTransactions];
    }
    const char* dialogs = getenv("MCE_SKTEST_DIALOGS");
    [s setDisableDialogs:(dialogs && strcmp(dialogs, "0") == 0)];
    [s setAskToBuyEnabled:flag("MCE_SKTEST_ASK_TO_BUY")];
    [s setInterruptedPurchasesEnabled:flag("MCE_SKTEST_INTERRUPT")];
    g_session = s;
    NSLog(@"mce_sktest: StoreKit test session over %s", config);
}
