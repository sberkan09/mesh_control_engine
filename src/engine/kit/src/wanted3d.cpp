#include "mce/kit/wanted3d.h"

#include <cmath>
#include <cstdio>

#include "kit3d_internal.h"
#include "mce/containers/array.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/hash.h"
#include "mce/kit/ai.h"
#include "mce/kit/captions.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/nav3d.h"
#include "mce/kit/perception.h"
#include "mce/kit/procgen.h"
#include "mce/kit/vehicles3d.h"

namespace mce::kit {

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(Wanted3D) == 68);
static_assert(sizeof(Police3D) == 68);
static_assert(sizeof(RespawnPoint3D) == 4);
static_assert(sizeof(Respawn3D) == 16);
static_assert(sizeof(Wallet3D) == 4);
static_assert(sizeof(CashDrop3D) == 8);
static_assert(sizeof(PaySpray3D) == 8);
static_assert(sizeof(AmmoShop3D) == 12);
static_assert(sizeof(MissionReward3D) == 4);
static_assert(sizeof(CarOwner3D) == 8);

namespace {

struct Wanted3DState {
    u8 installed = 1;
    Array<Entity> moved;       // where each offender was last tick (M-ARREST: how fast it is going)
    Array<math::vec3> before;
};

// e and the entities it hangs under: the first that has a Wanted3D.
Entity wanted_holder(World& w, Entity e) {
    for (u32 i = 0; i < 8 && !e.is_null() && w.alive(e); ++i) {
        if (w.has<Wanted3D>(e)) return e;
        const ChildOf* c = w.try_get<ChildOf>(e);
        e = c ? c->parent : Entity{};
    }
    return Entity{};
}

Entity health_holder(World& w, Entity e) {
    for (u32 i = 0; i < 8 && !e.is_null() && w.alive(e); ++i) {
        if (w.has<Health3D>(e)) return e;
        const ChildOf* c = w.try_get<ChildOf>(e);
        e = c ? c->parent : Entity{};
    }
    return Entity{};
}

math::vec3 position_of(World& w, Entity e) {
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return {};
    math::vec3 pos, scale;
    math::quat rot;
    world_pose(w, e, *t, pos, rot, scale);
    return pos;
}

u8 level_of(const Wanted3D& wd) {
    if (wd.heat <= 0.0f) return 0;
    const f32 raw = std::ceil(wd.heat / math::max(wd.heat_per_level, 1.0e-3f));
    return static_cast<u8>(math::clamp(raw, 0.0f, static_cast<f32>(wd.max_level)));
}

void settle(World& w, Entity e, Wanted3D& wd) {
    const f32 cap = static_cast<f32>(wd.max_level) * wd.heat_per_level;
    wd.heat = math::clamp(wd.heat, 0.0f, cap);
    const u8 now = level_of(wd);
    if (now != wd.level) {
        EventAccess(w).push(WantedChanged3D{e, wd.level, now});
        wd.level = now;
    }
}

struct Unit {
    Entity entity;
    math::vec3 eye;
    bool alive;
};

struct Offender {
    Entity entity;
    math::vec3 pos;
    f32 speed;  // m/s since last tick
};

bool witnessed_by_police(Frame& f, const Array<Unit>& units, math::vec3 at, f32 range) {
    for (const Unit& u : units) {
        if (!u.alive || math::length(u.eye - at) > range) continue;
        if (line_of_sight(f, PerceptionSpace::Space3D, u.eye, at + math::vec3{0.0f, 1.0f, 0.0f}, 0xFFFF, u.entity)) return true;
    }
    return false;
}

void add_heat(World& w, Entity e, f32 heat, math::vec3 at, Entity victim) {
    Wanted3D* wd = w.try_get_mut<Wanted3D>(e);
    if (!wd || heat <= 0.0f) return;
    wd->heat += heat;
    wd->unseen_time = 0.0f;
    ++wd->crimes;
    EventAccess(w).push(Crime3D{e, victim, heat, at});
    settle(w, e, *wd);
}

void set_state(Police3D& p, PoliceState s) {
    if (p.state != s) {
        p.state = s;
        p.state_time = 0.0f;
    }
}

// FixedUpdate_Pre, after "kit3d_health": crimes, then what the units see and do, then the cool-down.
void kit3d_wanted(Frame& f) {
    World& w = f.world();
    Wanted3DState& st = *w.try_resource<Wanted3DState>();
    Array<Unit> units;
    f.query<const Police3D, const LocalTransform>().each([&](Entity e, const Police3D& p, const LocalTransform&) {
        const Health3D* h = w.try_get<Health3D>(e);
        units.push_back(Unit{e, position_of(w, e) + math::vec3{0.0f, p.eye_height, 0.0f}, !(h && h->dead)});
    });

    // Crimes from this tick's combat events.
    auto ev = f.events();
    for (const Damage3D& d : ev.read<Damage3D>()) {
        const Entity off = wanted_holder(w, d.instigator);
        const Entity vic = health_holder(w, d.target);
        if (off.is_null() || vic.is_null() || off == vic) continue;
        const Wanted3D* wd = w.try_get<Wanted3D>(off);
        const u8 team = w.try_get<Health3D>(vic)->team;
        if (team == 0) continue;
        const math::vec3 at = position_of(w, vic);
        if (team == wd->police_team) {
            add_heat(w, off, wd->hit_police, at, vic);
        } else if (team == wd->civilian_team) {
            if (!wd->require_witness || witnessed_by_police(f, units, at, wd->witness_range)) add_heat(w, off, wd->hit_civilian, at, vic);
        }
    }
    for (const Died3D& d : ev.read<Died3D>()) {
        const Entity off = wanted_holder(w, d.killer);
        const Entity vic = health_holder(w, d.entity);
        if (off.is_null() || vic.is_null() || off == vic) continue;
        const Wanted3D* wd = w.try_get<Wanted3D>(off);
        const u8 team = w.try_get<Health3D>(vic)->team;
        if (team == 0) continue;
        const math::vec3 at = position_of(w, vic);
        if (team == wd->police_team) {
            add_heat(w, off, wd->kill_police, at, vic);
        } else if (team == wd->civilian_team) {
            if (!wd->require_witness || witnessed_by_police(f, units, at, wd->witness_range)) add_heat(w, off, wd->kill_civilian, at, vic);
        }
    }

    Array<Offender> offenders;
    f.query<Wanted3D>().each([&](Entity e, Wanted3D& wd) {
        wd.seen_by = 0;
        const math::vec3 pos = position_of(w, e);
        f32 speed = 0.0f;
        for (usize i = 0; i < st.moved.size(); ++i)
            if (st.moved[i] == e) {
                const math::vec3 d = pos - st.before[i];
                speed = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) / math::max(f.dt, 1.0e-6f);
            }
        offenders.push_back(Offender{e, pos, speed});
    });
    st.moved.clear();
    st.before.clear();
    for (const Offender& o : offenders) {
        st.moved.push_back(o.entity);
        st.before.push_back(o.pos);
    }

    // M-ARREST 2: an idle unit within `dispatch_range` of a place an offender it answers to was last seen.
    auto dispatch = [&](Entity e, Police3D& p) {
        const math::vec3 here = position_of(w, e);
        for (const Offender& o : offenders) {
            const Wanted3D& wd = *w.try_get<Wanted3D>(o.entity);
            if (wd.level < p.min_level || wd.crimes == 0) continue;
            if (math::length(wd.last_known - here) > p.dispatch_range) continue;
            if (math::length(wd.last_known - here) < p.arrest_range + 1.0f) continue;
            p.target = o.entity;
            p.last_known = wd.last_known;
            return true;
        }
        return false;
    };

    // The units.
    f.query<Police3D, LocalTransform>().each([&](Entity e, Police3D& p, LocalTransform& t) {
        const Health3D* h = w.try_get<Health3D>(e);
        if (h && h->dead) {
            set_state(p, PoliceState::Idle);
            p.target = {};
            return;
        }
        p.state_time += f.dt;
        const math::vec3 eye = position_of(w, e) + math::vec3{0.0f, p.eye_height, 0.0f};

        // The nearest offender it answers to and sees.
        const Offender* seen = nullptr;
        f32 best = p.sight_range;
        for (const Offender& o : offenders) {
            const Wanted3D& wd = *w.try_get<Wanted3D>(o.entity);
            if (wd.level < p.min_level) continue;
            const f32 d = math::length(o.pos - position_of(w, e));
            if (d > best) continue;
            if (!line_of_sight(f, PerceptionSpace::Space3D, eye, o.pos + math::vec3{0.0f, 1.0f, 0.0f}, 0xFFFF, e, o.entity)) continue;
            best = d;
            seen = &o;
        }

        const PoliceState before = p.state;
        const Wanted3D* tw = p.target.is_null() || !w.alive(p.target) ? nullptr : w.try_get<Wanted3D>(p.target);
        if (seen) {
            Wanted3D& wd = *w.try_get_mut<Wanted3D>(seen->entity);
            ++wd.seen_by;
            wd.last_known = seen->pos;
            p.target = seen->entity;
            p.last_known = seen->pos;
            set_state(p, best <= p.attack_range && wd.level >= p.attack_level ? PoliceState::Attack : PoliceState::Pursue);
        } else if (!tw && p.dispatch_range > 0.0f && dispatch(e, p)) {
            set_state(p, PoliceState::Search);
        } else if (!tw || tw->level < p.min_level) {
            p.target = {};
            set_state(p, PoliceState::Idle);
        } else if (p.state == PoliceState::Pursue || p.state == PoliceState::Attack) {
            set_state(p, PoliceState::Search);
        } else if (p.state == PoliceState::Search && p.state_time >= p.search_time) {
            p.target = {};
            set_state(p, PoliceState::Idle);
        }

        // M-ARREST: standing still within reach of a unit that answers to it, at a level it arrests at.
        {
            bool near_still = false;
            Entity who;
            if (seen && p.arrest_range > 0.0f && p.arrest_max_level > 0) {
                const Wanted3D& wd = *w.try_get<Wanted3D>(seen->entity);
                near_still = best <= p.arrest_range && wd.level <= p.arrest_max_level && seen->speed <= p.arrest_speed;
                who = seen->entity;
            }
            p.arrest_progress = near_still ? p.arrest_progress + f.dt : math::max(p.arrest_progress - f.dt, 0.0f);
            if (near_still && p.arrest_progress >= p.arrest_time) {
                p.arrest_progress = 0.0f;
                EventAccess(w).push(Arrested3D{who, e, seen->pos});
                clear_wanted(w, who);
            }
        }

        if (p.state == PoliceState::Attack && p.aim && !p.drive_car) {
            const math::vec3 d = p.last_known - position_of(w, e);
            if (std::abs(d.x) + std::abs(d.z) > 1.0e-4f)
                t.rotation = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, std::atan2(-d.x, -d.z));
        }
        if (Weapon3D* wp = w.try_get_mut<Weapon3D>(e); wp && p.aim && p.state == PoliceState::Attack) wp->fire = 1;

        if (p.drive_nav) {
            if (NavAgent3D* a = w.try_get_mut<NavAgent3D>(e)) {
                switch (p.state) {
                    case PoliceState::Pursue:
                        a->follow = p.target;
                        a->move_to = 1;
                        break;
                    case PoliceState::Search:
                        a->follow = {};
                        a->target = p.last_known;
                        a->move_to = 1;
                        break;
                    case PoliceState::Attack:
                    case PoliceState::Idle:
                        if (before != p.state) {
                            a->follow = {};
                            a->move_to = 0;
                        }
                        break;
                }
            }
        }
        // M-ARREST 3: a police car steers at the point it answers to.
        if (p.drive_car) {
            if (Vehicle3D* v = w.try_get_mut<Vehicle3D>(e); v && !v->use_input) {
                const bool go = p.state != PoliceState::Idle && !p.target.is_null();
                const math::vec3 d = p.last_known - position_of(w, e);
                const f32 dist = std::sqrt(d.x * d.x + d.z * d.z);
                if (!go || dist < 4.0f) {
                    v->throttle = 0.0f;
                    v->steer = 0.0f;
                    v->brake = 1.0f;
                } else {
                    const math::vec3 fwd = math::rotate(t.rotation, math::vec3{0.0f, 0.0f, -1.0f});
                    // Signed angle from the heading to the target, positive when it is to the right.
                    const f32 ang = std::atan2(fwd.x * d.z - fwd.z * d.x, fwd.x * d.x + fwd.z * d.z);
                    const f32 max_steer = math::max(v->max_steer, 1.0f) * 0.0174533f;
                    v->steer = math::clamp(ang / max_steer, -1.0f, 1.0f);
                    const f32 turn = std::abs(v->steer);
                    v->throttle = math::min(1.0f - 0.6f * turn, dist < 12.0f ? 0.4f : 1.0f);
                    v->brake = 0.0f;
                }
                v->handbrake = 0.0f;
            }
        }
        if (w.has<AiBrain>(e)) {
            const Wanted3D* now = p.target.is_null() ? nullptr : w.try_get<Wanted3D>(p.target);
            bb_set_number(w, e, "wanted", now ? static_cast<f32>(now->level) : 0.0f);
            bb_set_number(w, e, "police_state", static_cast<f32>(static_cast<u8>(p.state)));
            if (p.target.is_null())
                bb_clear(w, e, "target");
            else
                bb_set_entity(w, e, "target", p.target);
            bb_set_vector(w, e, "last_known", p.last_known);
        }
    });

    // The cool-down.
    for (const Offender& o : offenders) {
        Wanted3D& wd = *w.try_get_mut<Wanted3D>(o.entity);
        if (wd.seen_by > 0) {
            wd.unseen_time = 0.0f;
        } else if (wd.heat > 0.0f) {
            wd.unseen_time += f.dt;
            if (wd.unseen_time >= wd.cooldown_delay) wd.heat -= wd.decay * f.dt;
        }
        settle(w, o.entity, wd);
    }
}

// M-WASTED: dying or being arrested starts the timer; at zero the player wakes up at the nearest matching point.
void kit3d_respawn(Frame& f) {
    World& w = f.world();
    EventAccess ev(w);
    auto start = [&](Entity e, u8 reason) {
        Respawn3D* r = e.is_null() || !w.alive(e) ? nullptr : w.try_get_mut<Respawn3D>(e);
        if (!r || (r->reason != 0 && reason == 2)) return;  // a busted wait is not turned into a wasted one
        r->reason = reason;
        r->timer = r->delay;
        if (r->caption && w.try_resource<Captions>()) {
            ShowCaption c;
            c.text = reason == 1 ? "BUSTED" : "WASTED";
            c.seconds = r->delay;
            ev.push(c);
        }
    };
    for (const Died3D& d : ev.read<Died3D>()) start(d.entity, 2);
    for (const Arrested3D& a : ev.read<Arrested3D>()) start(a.offender, 1);

    f.query<Respawn3D>().each([&](Entity e, Respawn3D& r) {
        if (r.reason == 0) return;
        r.timer -= f.dt;
        if (r.timer > 0.0f) return;
        const RespawnKind want = r.reason == 1 ? RespawnKind::Station : RespawnKind::Hospital;
        const math::vec3 here = position_of(w, e);
        bool found = false, found_kind = false;
        f32 best = 0.0f;
        math::vec3 target = here;
        f.query<RespawnPoint3D, LocalTransform>().each([&](Entity pe, RespawnPoint3D& pt, LocalTransform&) {
            const math::vec3 pp = position_of(w, pe);
            const f32 d = math::length(pp - here);
            const bool match = pt.kind == want;
            if (found && ((found_kind && !match) || (found_kind == match && d >= best))) return;
            found = true;
            found_kind = match;
            best = d;
            target = pp;
        });
        if (found)
            if (LocalTransform* t = w.try_get_mut<LocalTransform>(e)) t->position = target;
        if (Health3D* h = w.try_get_mut<Health3D>(e)) {
            h->current = h->max;
            h->dead = 0;
            h->dead_time = 0.0f;
        }
        clear_wanted(w, e);
        const i32 fine = take_cash(w, e, r.reason == 1 ? r.fine_busted : r.fine_wasted);
        if (fine > 0 && r.caption && w.try_resource<Captions>()) {
            char buf[24];
            std::snprintf(buf, sizeof buf, "-$%d", fine);
            ShowCaption c;
            c.text = buf;
            c.seconds = 2.0f;
            ev.push(c);
        }
        ev.push(Respawned3D{e, r.reason, target});
        r.reason = 0;
        r.timer = 0.0f;
    });
}

// M-CASH: a killed CashDrop3D pays its killer's wallet a deterministic amount.
void kit3d_cash(Frame& f) {
    World& w = f.world();
    EventAccess ev(w);
    for (const Died3D& d : ev.read<Died3D>()) {
        if (d.entity.is_null() || d.killer.is_null() || !w.alive(d.entity) || !w.alive(d.killer)) continue;
        const CashDrop3D* drop = w.try_get<CashDrop3D>(d.entity);
        if (!drop || !w.has<Wallet3D>(d.killer)) continue;
        const i32 lo = drop->min < drop->max ? drop->min : drop->max;
        const i32 hi = drop->min < drop->max ? drop->max : drop->min;
        procgen::Rng rng = procgen::Rng::child(hash_combine(fnv1a64("cash"), d.entity.index), "cash", d.killer.index);
        give_cash(w, d.killer, rng.range(lo, hi));
    }
}

// M-PAYSPRAY: an offender standing at a garage with the cash pays for a clean slate.
void kit3d_payspray(Frame& f) {
    World& w = f.world();
    f.query<PaySpray3D, LocalTransform>().each([&](Entity ge, PaySpray3D& g, LocalTransform&) {
        if (!g.enabled) return;
        const math::vec3 gp = position_of(w, ge);
        f.query<Wanted3D, Wallet3D, LocalTransform>().each([&](Entity oe, Wanted3D& wd, Wallet3D& wl, LocalTransform&) {
            if (wd.level == 0 || math::length(position_of(w, oe) - gp) > g.radius) return;
            const i32 cost = static_cast<i32>(g.price_per_level) * static_cast<i32>(wd.level);
            if (wl.cash < cost) return;
            const u32 level = wd.level;
            const i32 paid = take_cash(w, oe, cost);
            clear_wanted(w, oe);
            EventAccess(w).push(Sprayed3D{oe, ge, paid, level});
        });
    });
}

void kit3d_ammoshop(Frame& f) {
    World& w = f.world();
    f.query<AmmoShop3D, LocalTransform>().each([&](Entity se, AmmoShop3D& sh, LocalTransform&) {
        if (!sh.enabled || sh.price_per_round == 0) return;
        const math::vec3 sp = position_of(w, se);
        f.query<Weapon3D, Wallet3D, LocalTransform>().each([&](Entity be, Weapon3D& wp, Wallet3D& wl, LocalTransform&) {
            if (wp.magazine == 0 || wp.infinite_ammo || wp.reserve >= sh.stock_to || math::length(position_of(w, be) - sp) > sh.radius) return;
            const u32 afford = wl.cash > 0 ? static_cast<u32>(wl.cash) / sh.price_per_round : 0u;
            const u32 rounds = math::min(sh.stock_to - wp.reserve, afford);
            if (rounds == 0) return;
            const i32 paid = take_cash(w, be, static_cast<i32>(rounds * sh.price_per_round));
            wp.reserve += rounds;
            EventAccess(w).push(AmmoBought3D{be, se, rounds, paid});
        });
    });
}

}  // namespace

bool give_cash(World& w, Entity e, i32 amount) {
    Wallet3D* wl = e.is_null() || !w.alive(e) || amount <= 0 ? nullptr : w.try_get_mut<Wallet3D>(e);
    if (!wl) return false;
    const i32 room = 2000000000 - wl->cash;
    const i32 add = amount < room ? amount : room;
    if (add <= 0) return false;
    wl->cash += add;
    CashChanged3D c;
    c.entity = e;
    c.delta = add;
    c.cash = wl->cash;
    EventAccess(w).push(c);
    return true;
}

i32 take_cash(World& w, Entity e, i32 amount) {
    Wallet3D* wl = e.is_null() || !w.alive(e) || amount <= 0 ? nullptr : w.try_get_mut<Wallet3D>(e);
    if (!wl || wl->cash <= 0) return 0;
    const i32 take = amount < wl->cash ? amount : wl->cash;
    wl->cash -= take;
    CashChanged3D c;
    c.entity = e;
    c.delta = -take;
    c.cash = wl->cash;
    EventAccess(w).push(c);
    return take;
}

bool report_crime(World& w, Entity offender, f32 heat, math::vec3 position, Entity victim, bool witnessed) {
    if (!witnessed || offender.is_null() || !w.alive(offender) || !w.has<Wanted3D>(offender) || heat <= 0.0f) return false;
    add_heat(w, offender, heat, position, victim);
    return true;
}

void clear_wanted(World& w, Entity offender) {
    Wanted3D* wd = offender.is_null() || !w.alive(offender) ? nullptr : w.try_get_mut<Wanted3D>(offender);
    if (!wd) return;
    wd->heat = 0.0f;
    wd->unseen_time = 0.0f;
    settle(w, offender, *wd);
}

void install_wanted3d(World& world) {
    install_combat3d(world, nullptr);
    if (world.try_resource<Wanted3DState>()) return;
    world.set_resource(Wanted3DState{});
    world.register_components<Wanted3D, Police3D, Respawn3D, RespawnPoint3D, Wallet3D, CashDrop3D, PaySpray3D, MissionReward3D, CarOwner3D, AmmoShop3D>();
    world.register_event<Sprayed3D>(Phase::FixedUpdate_Pre);
    world.register_event<AmmoBought3D>(Phase::FixedUpdate_Pre);
    world.register_event<CashChanged3D>(Phase::FixedUpdate_Pre);
    world.register_event<Respawned3D>(Phase::FixedUpdate_Pre);
    world.register_event<Crime3D>(Phase::FixedUpdate_Pre);
    world.register_event<WantedChanged3D>(Phase::FixedUpdate_Pre);
    world.register_event<Arrested3D>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_wanted", &kit3d_wanted, "kit3d_health");
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_respawn", &kit3d_respawn, "kit3d_wanted");
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_cash", &kit3d_cash, "kit3d_wanted");
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_payspray", &kit3d_payspray, "kit3d_cash");
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_ammoshop", &kit3d_ammoshop, "kit3d_payspray");
}

}  // namespace mce::kit
