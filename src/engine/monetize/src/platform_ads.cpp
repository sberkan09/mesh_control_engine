#include "mce/monetize/platform_ads.h"

#include "mce/convar.h"
#include "mce/log.h"
#include "mce/platform/ads.h"

namespace mce::monetize {

namespace {

MCE_CVAR(cv_ads_test, "monetize.ads.test", "auto",
         "Platform ads: Google's test units (on), the catalog's (off) or test units in debug builds only (auto)",
         CVarFlags::None);
MCE_CVAR(cv_ads_test_device, "monetize.ads.test_device", false,
         "Platform ads: the consent form acts as in the EEA (testing the form)", CVarFlags::None);

platform::ads::Format format_of(AdFormat f) {
    switch (f) {
        case AdFormat::Rewarded: return platform::ads::Format::Rewarded;
        case AdFormat::Interstitial: return platform::ads::Format::Interstitial;
        case AdFormat::Banner: return platform::ads::Format::Banner;
    }
    return platform::ads::Format::Interstitial;
}

AdEventKind kind_of(platform::ads::AnswerKind k) {
    using K = platform::ads::AnswerKind;
    switch (k) {
        case K::Loaded: return AdEventKind::Loaded;
        case K::NoFill: return AdEventKind::NoFill;
        case K::Shown: return AdEventKind::Shown;
        case K::Rewarded: return AdEventKind::Rewarded;
        case K::Closed: return AdEventKind::Closed;
        case K::Failed: return AdEventKind::Failed;
    }
    return AdEventKind::Failed;
}

void forward(void* self, const platform::ads::Answer& a) { static_cast<PlatformAds*>(self)->answer(a); }

bool debug_build() {
#if defined(NDEBUG)
    return false;
#else
    return true;
#endif
}

}  // namespace

PlatformAds::~PlatformAds() { stop(); }

const char* PlatformAds::name() const { return platform::ads::backend_name(platform::ads::backend()); }

bool PlatformAds::available() const { return platform::ads::backend() != platform::ads::Backend::None; }

StringView PlatformAds::unit(u16 placement) const {
    if (!catalog_ || placement >= catalog_->ads().size()) return {};
    const AdPlacementInfo& p = catalog_->ads()[placement];
#if defined(__APPLE__)
    const StringView own = p.ios_unit.view();
#else
    const StringView own = p.android_unit.view();
#endif
    return test_units_ || own.empty() ? platform::ads::test_unit(format_of(p.format)) : own;
}

void PlatformAds::start(const Catalog& catalog, Sink& sink) {
    stop();
    catalog_ = &catalog;
    sink_ = &sink;
    started_ = true;
    const StringView mode = cv_ads_test.get_string();
    if (mode == "on") test_units_ = true;
    else if (mode == "off") test_units_ = false;
    else {
        if (mode != "auto") MCE_LOG_WARN("monetize: -monetize.ads.test wants auto, on or off (got '{}'); auto", mode);
        test_units_ = debug_build();
    }
    platform::ads::Options opt;
    opt.test_device = cv_ads_test_device.get_bool();
    if (platform::ads::start(&forward, this, opt))
        MCE_LOG_INFO("monetize: ads through '{}' with {} units", name(), test_units_ ? "Google's test" : "the catalog's");
    else
        MCE_LOG_INFO("monetize: no ad network here ('{}'); every ad is NoFill", name());
}

void PlatformAds::stop() {
    if (!started_) return;
    platform::ads::stop();
    started_ = false;
}

void PlatformAds::remember(u32 request, u16 placement) {
    std::lock_guard<std::mutex> lock(sent_mutex_);
    sent_[next_sent_ % kSent] = Sent{request, placement};
    ++next_sent_;
}

void PlatformAds::refuse(AdEventKind kind, u16 placement, u32 request) {
    if (!sink_) return;
    AdAnswer a;
    a.kind = kind;
    a.placement = placement;
    a.request = request;
    sink_->post(a);
}

void PlatformAds::load(u16 placement, u32 request) {
    if (!catalog_ || placement >= catalog_->ads().size()) return refuse(AdEventKind::NoFill, placement, request);
    remember(request, placement);
    platform::ads::load(format_of(catalog_->ads()[placement].format), unit(placement), request);
}

void PlatformAds::show(u16 placement, u32 request) {
    if (!catalog_ || placement >= catalog_->ads().size()) return refuse(AdEventKind::Failed, placement, request);
    remember(request, placement);
    platform::ads::show(format_of(catalog_->ads()[placement].format), unit(placement), request);
}

void PlatformAds::show_banner(u16 placement, u32 request, BannerEdge edge, const SafeInsets& insets) {
    if (!catalog_ || placement >= catalog_->ads().size()) return refuse(AdEventKind::NoFill, placement, request);
    remember(request, placement);
    platform::ads::show_banner(unit(placement), request, edge == BannerEdge::Top, insets.left, insets.top,
                               insets.right, insets.bottom);
}

void PlatformAds::hide_banner() { platform::ads::hide_banner(); }

bool PlatformAds::privacy_options_required() const { return platform::ads::privacy_options_required(); }

void PlatformAds::show_privacy_options() { platform::ads::show_privacy_options(); }

void PlatformAds::answer(const platform::ads::Answer& a) {
    if (!sink_ || !catalog_) return;
    u16 placement = kNoIndex;
    {
        std::lock_guard<std::mutex> lock(sent_mutex_);
        for (const Sent& s : sent_)
            if (s.placement != kNoIndex && s.request == a.request) placement = s.placement;
    }
    if (placement == kNoIndex) {
        MCE_LOG_WARN("monetize: the ad network answered request {} for '{}', which was not asked; ignored", a.request,
                     a.unit);
        return;
    }
    AdAnswer out;
    out.kind = kind_of(a.kind);
    out.placement = placement;
    out.request = a.request;
    out.reward_amount = out.kind == AdEventKind::Rewarded ? a.amount : 0;
    sink_->post(out);
}

}  // namespace mce::monetize
