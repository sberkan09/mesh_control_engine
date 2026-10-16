#include "mce/kit/presence.h"

#include "mce/ecs/world.h"

namespace mce::kit {

namespace {

void kit_presence(Frame& f) {
    OverlayRequests* r = f.world().try_resource<OverlayRequests>();
    if (!r) return;
    for (const OpenOverlay& o : f.events().read<OpenOverlay>()) {
        if (r->pending.size() >= kMaxOverlayRequests) r->pending.erase(0);
        r->pending.push_back(o);
    }
}

}  // namespace

void install_presence(World& world) {
    if (world.try_resource<OverlayRequests>()) return;
    world.register_components<RichPresence>();
    world.register_event<OpenOverlay>(Phase::FixedUpdate_Pre);
    world.set_resource(OverlayRequests{});
    world.add_system(Phase::FixedUpdate_Post, "kit_presence", &kit_presence);
}

bool current_presence(World& world, RichPresence& out) {
    if (world.find_component(kTypeHash<RichPresence>) == ecs::kInvalidComponent) return false;
    bool found = false;
    world.query<const RichPresence>().each([&](Entity, const RichPresence& p) {
        if (found || !p.enabled) return;
        out = p;
        found = true;
    });
    return found;
}

void take_overlay_requests(World& world, Array<OpenOverlay>& out) {
    out.clear();
    OverlayRequests* r = world.try_resource<OverlayRequests>();
    if (!r) return;
    for (const OpenOverlay& o : r->pending) out.push_back(o);
    r->pending.clear();
}

const char* overlay_page_name(OverlayPage p) {
    switch (p) {
        case OverlayPage::Friends: return "friends";
        case OverlayPage::Achievements: return "achievements";
        case OverlayPage::Stats: return "stats";
        case OverlayPage::Community: return "community";
        case OverlayPage::Settings: return "settings";
        case OverlayPage::Store: return "store";
        case OverlayPage::Web: return "web";
    }
    return "friends";
}

}  // namespace mce::kit
