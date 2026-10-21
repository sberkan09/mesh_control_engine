#include "marble_garden.h"

#include <cmath>
#include <cstdio>

#include "mce/ecs/hierarchy.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/effects.h"
#include "mce/log.h"
#include "mce/scene/scene.h"

namespace marble_garden {

using namespace mce;
using math::vec2;
using math::vec3;

namespace {

// ---- helpers -------------------------------------------------------------------------------------

vec3 entity_position(World& w, Entity e) {
    if (const WorldTransform* wt = w.try_get<WorldTransform>(e)) return wt->position;
    if (const LocalTransform* lt = w.try_get<LocalTransform>(e)) return lt->position;
    return {};
}

void set_label(World& w, Entity e, const char* text) {
    if (e.is_null() || !w.alive(e)) return;
    if (kit::TextLabel* label = w.try_get_mut<kit::TextLabel>(e)) {
        if (!(label->text == StringView(text))) label->text.assign(StringView(text));
    }
}

void fire_sfx(World& w, Entity e) {
    if (e.is_null() || !w.alive(e)) return;
    if (kit::SfxTrigger* sfx = w.try_get_mut<kit::SfxTrigger>(e)) sfx->fire = true;
}

// The first GardenGame of the scene (there is one), or null.
GardenGame* find_game(Frame& f, Entity* out = nullptr) {
    GardenGame* game = nullptr;
    f.query<GardenGame&>().each([&](Entity e, GardenGame& g) {
        if (!game) {
            game = &g;
            if (out) *out = e;
        }
    });
    return game;
}

// The horizontal forward and right of the primary 3D camera (forward -Z when there is none).
void camera_basis(Frame& f, vec3& forward, vec3& right) {
    forward = {0, 0, -1};
    bool found = false;
    f.query<const kit::Camera3DComponent&, const LocalTransform&>().each(
        [&](Entity e, const kit::Camera3DComponent& cam, const LocalTransform& lt) {
            if (found || cam.primary == 0) return;
            found = true;
            math::quat rot = lt.rotation;
            if (const WorldTransform* wt = f.world().try_get<WorldTransform>(e)) rot = wt->rotation;
            vec3 fw = math::rotate(rot, vec3{0, 0, -1});
            fw.y = 0;
            if (math::length(fw) < 1e-3f) {  // looking straight down: use the camera's up instead
                fw = math::rotate(rot, vec3{0, 1, 0});
                fw.y = 0;
            }
            if (math::length(fw) > 1e-3f) forward = math::normalize(fw);
        });
    right = {-forward.z, 0, forward.x};  // forward (0,0,-1) -> right (1,0,0)
}

// ---- the marble ----------------------------------------------------------------------------------

// FixedUpdate_Pre, after kit3d_bodies created the body: push the marble by the Move action.
void marble_drive(Frame& f) {
    kit::Physics3D* physics = f.try_resource<kit::Physics3D>();
    const input::InputState* in = f.try_resource<input::InputState>();
    if (!physics) return;
    const GardenGame* game = find_game(f);
    const bool playing = !game || game->state == 0;

    vec3 forward, right;
    camera_basis(f, forward, right);

    f.query<MarbleController&, const kit::RigidBody3D&, const LocalTransform&>().each(
        [&](Entity e, MarbleController& m, const kit::RigidBody3D& rb, const LocalTransform& lt) {
            if (!m.started) {
                m.started = true;
                m.spawn_position = lt.position;
            }
            if (m.respawn_cooldown > 0) m.respawn_cooldown--;
            if (!physics->world.has_body(e)) return;
            physics::BodyState3D s;
            if (!physics->world.body_state(e, s)) return;

            const f32 r = rb.radius * std::fmax(lt.scale.x, std::fmax(lt.scale.y, lt.scale.z));
            const f32 mass = rb.density * (4.0f / 3.0f) * math::kPi * r * r * r;

            vec2 move = (in && playing) ? in->move : vec2{0, 0};
            const f32 len = math::length(move);
            if (len > 1.0f) move = move * (1.0f / len);

            const vec3 vh{s.linear_velocity.x, 0, s.linear_velocity.z};
            vec3 dv{0, 0, 0};
            if (len > 0.05f) {
                const vec3 dir = right * move.x + forward * move.y;
                const vec3 dir_n = math::normalize(dir);
                const f32 along = math::dot(vh, dir_n);
                if (along < m.max_speed) dv = dir * (m.acceleration * f.dt);
                // Steer: bleed off sideways speed so turns feel responsive.
                const vec3 side = vh - dir_n * along;
                const f32 side_len = math::length(side);
                if (side_len > 1e-3f) {
                    const f32 cut = std::fmin(side_len, m.brake * f.dt);
                    dv = dv - side * (cut / side_len);
                }
            } else {
                const f32 speed = math::length(vh);
                if (speed > 1e-3f) {
                    const f32 cut = std::fmin(speed, m.brake * f.dt);
                    dv = vh * (-cut / speed);
                }
            }
            if (math::length_sq(dv) > 0.0f) physics->world.apply_impulse(e, dv * mass);
        });
}

// Puts the marble back at its respawn point with no speed.
void respawn_marble(Frame& f, Entity marble, MarbleController& m) {
    vec3 at = m.spawn_position;
    if (!m.respawn.is_null() && f.world().alive(m.respawn)) at = entity_position(f.world(), m.respawn);
    if (LocalTransform* lt = f.world().try_get_mut<LocalTransform>(marble)) {
        lt->position = at;
        lt->rotation = math::quat{};
    }
    if (kit::Physics3D* physics = f.try_resource<kit::Physics3D>()) {
        if (physics->world.has_body(marble)) {
            physics->world.teleport(marble, at, math::quat{});
            physics->world.set_linear_velocity(marble, {0, 0, 0});
        }
    }
    if (kit::RigidBody3D* rb = f.world().try_get_mut<kit::RigidBody3D>(marble)) {
        rb->velocity = {0, 0, 0};
        rb->set_velocity = true;
        rb->impulse = {0, 0, 0};
    }
    m.respawns++;
    m.respawn_cooldown = 10;
}

// FixedUpdate_Post: the pond (and falling off the world) sends the marble back.
void hazards(Frame& f) {
    World& w = f.world();
    Entity game_e;
    GardenGame* game = find_game(f, &game_e);
    auto respawn = [&](Entity marble, Entity hazard) {
        MarbleController* m = w.try_get_mut<MarbleController>(marble);
        if (!m || m->respawn_cooldown > 0) return;
        respawn_marble(f, marble, *m);
        f.events().push(MarbleRespawned{marble, hazard});
        if (game) {
            game->falls++;
            fire_sfx(w, game->fall_sound);
        }
    };
    for (const kit::TriggerEnter3D& ev : f.events().read<kit::TriggerEnter3D>()) {
        Hazard* h = w.try_get_mut<Hazard>(ev.trigger);
        if (!h || !w.has<MarbleController>(ev.other)) continue;
        h->hits++;
        respawn(ev.other, ev.trigger);
    }
    f.query<MarbleController&, const LocalTransform&>().each([&](Entity e, MarbleController& m, const LocalTransform& lt) {
        if (m.started && lt.position.y < m.kill_y) respawn(e, Entity{});
    });
}

// ---- gems ----------------------------------------------------------------------------------------

// FixedUpdate_Pre: gems turn about +Y and bob.
void gem_motion(Frame& f) {
    f.query<Gem&, LocalTransform&>().each([&](Gem& g, LocalTransform& t) {
        if (!g.started) {
            g.started = true;
            g.base_y = t.position.y;
        }
        g.time += f.dt;
        const f32 radians = g.spin_dps * (math::kPi / 180.0f) * f.dt;
        t.rotation = math::normalize(math::quat_from_axis_angle({0, 1, 0}, radians) * t.rotation);
        const f32 period = g.bob_period > 0.0f ? g.bob_period : 1.0f;
        t.position.y = g.base_y + g.bob_height * std::sin(g.time * (2.0f * math::kPi / period));
    });
}

// FixedUpdate_Post: the marble touching a gem collects it.
void gem_pickup(Frame& f) {
    World& w = f.world();
    GardenGame* game = find_game(f);
    if (game && game->state != 0) return;
    for (const kit::TriggerEnter3D& ev : f.events().read<kit::TriggerEnter3D>()) {
        Gem* gem = w.try_get_mut<Gem>(ev.trigger);
        if (!gem || gem->value == 0 || !w.has<MarbleController>(ev.other)) continue;
        const u32 value = gem->value;
        gem->value = 0;  // collected: ignore any further overlap this tick
        f.commands().destroy(ev.trigger);
        u32 collected = value, total = 0;
        if (game) {
            game->gems_collected += value;
            collected = game->gems_collected;
            total = game->gems_total;
            fire_sfx(w, game->chime);
        }
        f.events().push(GemCollected{ev.trigger, collected, total});
    }
}

// ---- the round: timer, HUD, win and lose -----------------------------------------------------------

void round_rules(Frame& f) {
    World& w = f.world();
    GardenGame* game = find_game(f);
    if (!game) return;
    GardenGame& g = *game;
    if (!g.started) {
        g.started = true;
        g.time_left = g.time_limit;
        g.state = 0;
        g.end_timer = 0;
        if (g.gems_total == 0) {
            u32 n = 0;
            f.query<const Gem&>().each([&](const Gem& gem) { n += gem.value; });
            g.gems_total = n;
        }
    }

    if (g.state == 0) {
        g.time_left -= f.dt;
        if (g.gems_total > 0 && g.gems_collected >= g.gems_total) {
            g.state = 1;
            f.events().push(RoundOver{1, g.gems_collected, g.gems_total, g.time_limit - g.time_left});
        } else if (g.time_left <= 0.0f) {
            g.time_left = 0.0f;
            g.state = 2;
            f.events().push(RoundOver{0, g.gems_collected, g.gems_total, g.time_limit});
        }
        if (g.state != 0) {
            GardenResult& r = f.resource<GardenResult>();
            r.rounds++;
            r.won = g.state == 1 ? 1u : 0u;
            r.gems = g.gems_collected;
            r.total = g.gems_total;
            r.time_used = g.time_limit - g.time_left;
            r.falls = g.falls;
            if (r.won && (r.best_time <= 0.0f || r.time_used < r.best_time)) r.best_time = r.time_used;
            MCE_LOG_INFO("marble_garden: round over, {}, {}/{} gems in {:.1f} s", r.won ? "won" : "lost", r.gems, r.total,
                         static_cast<double>(r.time_used));
        }
    } else {
        g.end_timer += f.dt;
        if (g.end_timer >= g.end_delay) {
            const StringView target = g.state == 1 ? g.win_scene.view() : g.lose_scene.view();
            if (!target.empty()) scene::request_change(w, target);
        }
    }

    // HUD
    char buf[64];
    const int secs = static_cast<int>(std::ceil(g.time_left));
    std::snprintf(buf, sizeof buf, "%d:%02d", secs / 60, secs % 60);
    set_label(w, g.hud_timer, buf);
    std::snprintf(buf, sizeof buf, "Gems %u / %u", g.gems_collected, g.gems_total);
    set_label(w, g.hud_gems, buf);
}

// ---- menus ---------------------------------------------------------------------------------------

void menus(Frame& f) {
    const input::InputState* in = f.try_resource<input::InputState>();
    f.query<MenuScreen&>().each([&](MenuScreen& m) {
        m.time += f.dt;
        if (m.done || m.time < m.min_time || !in) return;
        const bool go = in->confirm_pressed || in->jump_pressed || in->fire_pressed || !in->text().empty();
        if (!go || m.next_scene.empty()) return;
        m.done = 1;
        scene::request_change(f.world(), m.next_scene.view());
    });
}

void result_labels(Frame& f) {
    const GardenResult* r = f.try_resource<GardenResult>();
    if (!r || r->rounds == 0) return;
    f.query<const ResultLabel&, kit::TextLabel&>().each([&](const ResultLabel& rl, kit::TextLabel& label) {
        char buf[64];
        if (r->won && rl.show_time)
            std::snprintf(buf, sizeof buf, "All %u gems in %.1f s  (best %.1f s)", r->gems,
                          static_cast<double>(r->time_used), static_cast<double>(r->best_time));
        else if (r->won)
            std::snprintf(buf, sizeof buf, "All %u gems collected", r->gems);
        else
            std::snprintf(buf, sizeof buf, "%u of %u gems before time ran out", r->gems, r->total);
        if (!(label.text == StringView(buf))) label.text.assign(StringView(buf));
    });
}

// ---- template systems ------------------------------------------------------------------------------

void spin(Frame& f) {
    f.query<const Spin&, LocalTransform&>().each([&](const Spin& s, LocalTransform& t) {
        const f32 radians = s.degrees_per_second * (math::kPi / 180.0f) * f.dt;
        t.rotation = math::quat_from_axis_angle({0, 0, 1}, radians) * t.rotation;
    });
}

void bob(Frame& f) {
    f.query<Bob&, LocalTransform&>().each([&](Bob& b, LocalTransform& t) {
        if (!b.started) {
            b.base_y = t.position.y;
            b.started = true;
        }
        b.time += f.dt;
        const f32 period = b.period > 0.0f ? b.period : 1.0f;
        t.position.y = b.base_y + b.height * std::sin(b.time * (2.0f * math::kPi / period));
    });
}

}  // namespace

ResultV install(World& world) {
    if (!world.try_resource<input::InputState>()) input::install(world);
    world.register_components<Spin, Bob, MarbleController, Gem, Hazard, GardenGame, MenuScreen, ResultLabel>();
    world.register_event<GemCollected>(Phase::FixedUpdate_Post);
    world.register_event<MarbleRespawned>(Phase::FixedUpdate_Post);
    world.register_event<RoundOver>(Phase::FixedUpdate_Post);
    if (!world.try_resource<GardenResult>()) world.set_resource(GardenResult{});

    world.add_system(Phase::FixedUpdate_Pre, "marble_garden_spin", &spin);
    world.add_system(Phase::FixedUpdate_Pre, "marble_garden_bob", &bob, "marble_garden_spin");
    world.add_system(Phase::FixedUpdate_Pre, "marble_garden_menus", &menus, "marble_garden_bob");
    world.add_system(Phase::FixedUpdate_Pre, "marble_garden_marble", &marble_drive, "marble_garden_menus");
    world.add_system(Phase::FixedUpdate_Pre, "marble_garden_gem_motion", &gem_motion, "marble_garden_marble");
    world.add_system(Phase::FixedUpdate_Pre, "marble_garden_results", &result_labels, "marble_garden_gem_motion");
    world.add_system(Phase::FixedUpdate_Post, "marble_garden_gem_pickup", &gem_pickup);
    world.add_system(Phase::FixedUpdate_Post, "marble_garden_hazards", &hazards, "marble_garden_gem_pickup");
    world.add_system(Phase::FixedUpdate_Post, "marble_garden_round", &round_rules, "marble_garden_hazards");
    MCE_LOG_INFO("marble_garden: game systems installed");  // the module logs into the host's log
    return ok();
}

}  // namespace marble_garden
