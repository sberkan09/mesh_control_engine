#pragma once
// The phone's ad network as a provider (M-MONETIZE step 4): Google Mobile Ads with Google's consent
// form (UMP) and, on iOS, App Tracking Transparency, through mce_platform's ads (mce/platform/ads.h),
// the only place those SDKs are seen. Off a phone, or on one built without the SDKs, available() is
// false and every request ends in NoFill (Failed for a show), like the null network.
//
// Which ad units it asks for:
//   -monetize.ads.test auto|on|off   auto (default): Google's test units in a debug build, the
//                                    catalog's units (AdPlacementInfo::android_unit / ios_unit) in a
//                                    release one; on: test units always; off: the catalog's always.
// A placement without a unit for this platform always gets the test unit: real ids come only from
// the project's catalog. -monetize.ads.test_device 1 makes the consent form act as in the EEA.
//
// Answers arrive on the SDK's threads and go straight into the Service's inbox.

#include <mutex>

#include "mce/monetize/monetize.h"

namespace mce::platform::ads {
struct Answer;
}

namespace mce::monetize {

class PlatformAds final : public IAdProvider {
public:
    PlatformAds() = default;
    ~PlatformAds() override;
    const char* name() const override;
    bool available() const override;
    void start(const Catalog& catalog, Sink& sink) override;
    void stop() override;
    void load(u16 placement, u32 request) override;
    void show(u16 placement, u32 request) override;
    void show_banner(u16 placement, u32 request, BannerEdge edge, const SafeInsets& insets) override;
    void hide_banner() override;
    bool privacy_options_required() const override;
    void show_privacy_options() override;

    // The unit `placement` is requested with: the test unit or the catalog's (tests).
    StringView unit(u16 placement) const;
    void set_test_units(bool on) { test_units_ = on; }
    bool test_units() const { return test_units_; }
    // One answer from mce_platform's ads (any thread), mapped to the catalog and posted.
    void answer(const platform::ads::Answer& a);

private:
    void remember(u32 request, u16 placement);
    void refuse(AdEventKind kind, u16 placement, u32 request);

    const Catalog* catalog_ = nullptr;
    Sink* sink_ = nullptr;
    bool started_ = false;
    bool test_units_ = false;
    // The last requests and their placements: answers name a unit, and in test mode every
    // placement of a format shares one.
    struct Sent {
        u32 request = 0;
        u16 placement = kNoIndex;
    };
    static constexpr u32 kSent = 64;
    std::mutex sent_mutex_;
    Sent sent_[kSent];
    u32 next_sent_ = 0;
};

}  // namespace mce::monetize
