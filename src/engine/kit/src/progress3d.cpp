#include "mce/kit/progress3d.h"

#include <algorithm>
#include <cstdio>

#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/kit/captions.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/melee3d.h"
#include "mce/kit/missions.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/wanted3d.h"

namespace mce::kit {

static_assert(sizeof(Progress3D) == 24);
static_assert(sizeof(Goal3D) == 60);

namespace {

struct Progress3DInstalled {};

u32 counter(const Progress3D& p, GoalStat s) {
    switch (s) {
        case GoalStat::Kills: return p.kills;
        case GoalStat::Missions: return p.missions;
        case GoalStat::Rides: return p.rides;
        case GoalStat::MeleeHits: return p.melee_hits;
    }
    return 0;
}

void kit_progress(Frame& f) {
    World& w = f.world();
    EventAccess ev(w);
    f.query<Progress3D>().each([&](Entity e, Progress3D& p) {
        for (const Died3D& d : ev.read<Died3D>())
            if (d.killer == e && !(d.entity == e)) ++p.kills;
        for (const MissionCompleted& m : ev.read<MissionCompleted>()) {
            const Mission* ms = w.alive(m.mission) ? w.try_get<Mission>(m.mission) : nullptr;
            if (ms && ms->actor == e) ++p.missions;
        }
        for (const VehicleEntered3D& v : ev.read<VehicleEntered3D>())
            if (v.rider == e) ++p.rides;
        for (const MeleeSwung3D& s : ev.read<MeleeSwung3D>())
            if (s.entity == e) p.melee_hits += s.hits;
    });
    Array<Entity> goals;
    f.query<Goal3D>().each([&](Entity g, Goal3D& gl) {
        if (!gl.done) goals.push_back(g);
    });
    std::sort(goals.begin(), goals.end(), [](Entity a, Entity b) { return a.index < b.index; });
    for (Entity g : goals) {
        Goal3D* gl = w.try_get_mut<Goal3D>(g);
        if (!gl || gl->done || gl->owner.is_null() || !w.alive(gl->owner)) continue;
        Progress3D* p = w.try_get_mut<Progress3D>(gl->owner);
        if (!p || counter(*p, gl->stat) < gl->target) continue;
        gl->done = 1;
        ++p->goals_done;
        if (gl->reward > 0) give_cash(w, gl->owner, static_cast<i32>(gl->reward));
        if (w.try_resource<Captions>()) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "Goal: %s", gl->name.c_str());
            ShowCaption c;
            c.text = buf;
            c.seconds = 3.0f;
            ev.push(c);
        }
        ev.push(GoalDone3D{g, gl->owner, gl->reward, 0});
    }
}

}  // namespace

void install_progress3d(World& world) {
    install_missions(world, nullptr);
    install_seats3d(world, nullptr);
    install_melee3d(world);
    install_wanted3d(world);
    if (world.try_resource<Progress3DInstalled>()) return;
    world.set_resource(Progress3DInstalled{});
    world.register_components<Progress3D, Goal3D>();
    world.register_event<GoalDone3D>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_progress", &kit_progress, "kit_melee");
}

}  // namespace mce::kit
