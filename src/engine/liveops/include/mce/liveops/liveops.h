#pragma once
// mce_liveops (L2, M-LIVEOPS): services for shipped games (crash reports, analytics events; game
// services in game_services.h and remote config in remote_config.h). Off unless the project's host turns it on
// (player::PlayerDesc::liveops), never in the sim or the determinism hash, and the game stays
// fully playable offline: a service that cannot reach its vendor only waits.
//
// Consent gates everything that leaves the device. Until it is known (Unknown) analytics events
// wait in memory and crash reports wait on disk; Denied drops the events and keeps the reports
// local; Granted sends both. The player feeds it from the ad network's consent (M-MONETIZE: UMP and
// ATT) unless the game sets it from its own privacy screen (set_consent), or -liveops.consent.
//
// Step 1 (crash reports and analytics):
//
//   liveops::Service svc;
//   liveops::ServiceDesc d;
//   d.analytics = &my_backend;        // mce/liveops/analytics.h; null: events are dropped
//   d.crash_uploader = &my_uploader;  // mce/liveops/crash_upload.h; null: reports stay on disk
//   d.crash_dir = crash::dump_dir();
//   svc.init(d);
//   ... once per host tick: svc.update();
//
// Game code anywhere:  liveops::analytics::event("level_end", liveops::analytics::Params().add("level", 3));

#include "mce/fixed_string.h"
#include "mce/liveops/analytics.h"
#include "mce/liveops/crash_upload.h"

namespace mce::liveops {

struct ServiceDesc {
    analytics::IBackend* analytics = nullptr;
    ICrashUploader* crash_uploader = nullptr;
    FixedString<512> crash_dir;  // the crash reports' directory (crash::dump_dir()); empty: none
};

// What the host owns: installs the analytics backend, lists the crash reports left by earlier runs
// and sends what consent allows, a little each update.
class Service {
public:
    Service() = default;
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    void init(const ServiceDesc& d);
    // Main thread, once per host tick (paused or not).
    void update();
    void shutdown();

    CrashReports& crashes() { return crashes_; }
    const CrashReports& crashes() const { return crashes_; }

private:
    CrashReports crashes_;
    analytics::IBackend* analytics_ = nullptr;
    bool started_ = false;
};

}  // namespace mce::liveops
