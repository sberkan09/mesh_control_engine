#include "mce/liveops/liveops.h"

#include <atomic>

#include "mce/log.h"

namespace mce::liveops {
namespace {

std::atomic<u8> g_consent{static_cast<u8>(Consent::Unknown)};

}  // namespace

void set_consent(Consent c) {
    const u8 old = g_consent.exchange(static_cast<u8>(c), std::memory_order_acq_rel);
    if (old != static_cast<u8>(c)) MCE_LOG_INFO("liveops: consent {}", consent_name(c));
}

Consent consent() { return static_cast<Consent>(g_consent.load(std::memory_order_acquire)); }

const char* consent_name(Consent c) {
    switch (c) {
        case Consent::Unknown:
            return "unknown";
        case Consent::Granted:
            return "granted";
        case Consent::Denied:
            return "denied";
    }
    return "unknown";
}

Service::~Service() { shutdown(); }

void Service::init(const ServiceDesc& d) {
    shutdown();
    analytics_ = d.analytics;
    analytics::set_backend(analytics_);
    crashes_.set_uploader(d.crash_uploader);
    const u32 waiting = d.crash_dir.empty() ? 0 : crashes_.scan(d.crash_dir.view());
    MCE_LOG_INFO("liveops: analytics '{}', crash reports '{}' ({} waiting), consent {}",
                 analytics_ ? analytics_->name() : "none", d.crash_uploader ? d.crash_uploader->name() : "none",
                 waiting, consent_name(consent()));
    started_ = true;
}

void Service::update() {
    if (!started_) return;
    analytics::update();
    crashes_.update();
}

void Service::shutdown() {
    if (!started_) return;
    analytics::update();  // what consent already allows goes out before the backend does
    if (analytics::backend() == analytics_) analytics::set_backend(nullptr);
    crashes_.set_uploader(nullptr);
    started_ = false;
}

}  // namespace mce::liveops
