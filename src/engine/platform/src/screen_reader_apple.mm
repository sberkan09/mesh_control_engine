// mce_platform: the macOS side of platform/screen_reader.h. VoiceOver speaks an announcement request
// posted on the application; NSWorkspace says whether VoiceOver is on. Built without ARC (the
// dictionary is autoreleased).

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <AppKit/AppKit.h>

#include "mce/fixed_string.h"

namespace mce::platform::screen_reader {

bool apple_running() { return [[NSWorkspace sharedWorkspace] isVoiceOverEnabled]; }

bool apple_announce(StringView text) {
    const FixedString<512> z(text);
    NSString* s = [NSString stringWithUTF8String:z.c_str()];
    if (!s || !NSApp) return false;
    @autoreleasepool {
        NSDictionary* info = @{
            NSAccessibilityAnnouncementKey : s,
            NSAccessibilityPriorityKey : @(NSAccessibilityPriorityHigh),
        };
        id element = [NSApp mainWindow] ? (id)[NSApp mainWindow] : (id)NSApp;
        NSAccessibilityPostNotificationWithUserInfo(element, NSAccessibilityAnnouncementRequestedNotification, info);
    }
    return true;
}

}  // namespace mce::platform::screen_reader

#endif  // TARGET_OS_OSX
