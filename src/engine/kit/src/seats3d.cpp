#include "mce/kit/seats3d.h"

#include <cmath>

#include "kit3d_internal.h"
#include "mce/containers/array.h"
#include "mce/containers/hash_map.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/effects.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/vehicles3d.h"
#include "mce/kit/wanted3d.h"
#include "mce/string_id.h"

namespace mce::kit {

static_assert(sizeof(VehicleSeat3D) == 44);
static_assert(sizeof(Rider3D) == 20);

namespace {

constexpr StringId kInteract{"Interact"};

enum : u8 { kSavedInput = 1, kSavedDriver = 2 };

struct Seats3DState {
    HashMap<Entity, CharacterController3D> stash;  // a seated rider's controller, back on exit
};

bool pose_of(World& w, Entity e, math::vec3& pos, math::quat& rot) {
    if (e.is_null() || !w.alive(e)) return false;
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return false;
    math::vec3 scale;
    world_pose(w, e, *t, pos, rot, scale);
    return true;
}

void follow(World& w, Entity camera, Entity target) {
    if (camera.is_null() || !w.alive(camera)) return;
    if (CameraFollow3D* c = w.try_get_mut<CameraFollow3D>(camera)) c->target = target;
}

// Gives the car back what boarding took.
void release_seat(World& w, Entity car, VehicleSeat3D& seat) {
    if (Vehicle3D* v = w.try_get_mut<Vehicle3D>(car)) {
        v->use_input = 0;
        v->throttle = v->steer = 0.0f;
        v->brake = 0.0f;
        v->handbrake = 1.0f;
    }
    if ((seat.saved & kSavedDriver) != 0)
        if (VehicleDriver3D* d = w.try_get_mut<VehicleDriver3D>(car)) d->drive = 1;
    seat.saved = 0;
    seat.driver = Entity{};
}

// A rider steps out at `pos` facing `rot`: the controller comes back, the seat is freed.
void step_out(Frame& f, Entity rider, Rider3D& r, LocalTransform& t, math::vec3 pos, math::quat rot, bool dead = false) {
    World& w = f.world();
    Seats3DState& st = f.resource<Seats3DState>();
    const Entity car = r.vehicle;
    if (!car.is_null() && w.alive(car))
        if (VehicleSeat3D* seat = w.try_get_mut<VehicleSeat3D>(car); seat && seat->driver == rider) release_seat(w, car, *seat);
    t.position = pos;
    const math::vec3 fwd = math::rotate(rot, math::vec3{0.0f, 0.0f, -1.0f});
    t.rotation = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, std::atan2(-fwd.x, -fwd.z));
    CharacterController3D ctl;
    if (const CharacterController3D* kept = st.stash.find(rider)) ctl = *kept;
    st.stash.erase(rider);
    // What the stash remembers of the ride's start is stale; a dead rider no longer takes the input.
    ctl.velocity = {};
    ctl.flags = 0;
    ctl.jumping = 0;
    ctl.airborne_ticks = ctl.buffered_ticks = 0;
    ctl.air_jumps_used = 0;
    ctl.crouched = false;
    if (dead) {
        ctl.use_input = false;
        ctl.move = {};
    }
    f.commands().add<CharacterController3D>(rider, ctl);
    follow(w, r.camera, rider);
    f.events().push(VehicleExited3D{rider, car});
    r.seated = 0;
    r.vehicle = Entity{};
}

// The first of the exit points with room for the rider's capsule.
math::vec3 exit_point(World& w, Entity car, const VehicleSeat3D& seat, f32 radius, math::vec3 vp, math::quat vr) {
    Physics3D* phys = w.try_resource<Physics3D>();
    const math::vec3 cands[3] = {vp + math::rotate(vr, seat.exit), vp + math::rotate(vr, math::vec3{-seat.exit.x, seat.exit.y, seat.exit.z}),
                                 vp + math::vec3{0.0f, 2.4f, 0.0f}};
    if (!phys) return cands[0];
    for (const math::vec3& c : cands) {
        Entity skip[1] = {car};
        physics::QueryFilter3D filter;
        filter.include_characters = true;
        filter.exclude = Span<const Entity>(skip, 1);
        Entity out[1];
        if (phys->world.overlap_sphere(c, math::max(radius, 0.1f), Span<Entity>(out, 1), filter) == 0) return c;
    }
    return cands[2];
}

// FixedUpdate_Pre, after the vehicles: boarding, leaving, and freeing seats whose rider or car went.
void kit3d_seats(Frame& f) {
    World& w = f.world();
    Seats3DState& st = f.resource<Seats3DState>();
    const input::InputState* in = w.try_resource<input::InputState>();

    // A seat whose driver is gone, or no longer sits there.
    f.query<VehicleSeat3D>().each([&](Entity car, VehicleSeat3D& seat) {
        if (seat.driver.is_null()) return;
        const Rider3D* r = w.alive(seat.driver) ? w.try_get<Rider3D>(seat.driver) : nullptr;
        if (!r || !r->seated || r->vehicle != car) release_seat(w, car, seat);
    });

    f.query<Rider3D, LocalTransform>().each([&](Entity e, Rider3D& r, LocalTransform& t) {
        // The car went (destroyed, or its seat removed): put the rider back where it sits.
        if (r.seated) {
            const bool gone = r.vehicle.is_null() || !w.alive(r.vehicle) || !w.try_get<VehicleSeat3D>(r.vehicle);
            if (gone) {
                step_out(f, e, r, t, t.position, t.rotation);
                r.enter = 0;
                return;
            }
        }
        // A rider who died in the seat is put out.
        if (r.seated) {
            const Health3D* hp = w.try_get<Health3D>(e);
            if (hp && hp->dead) {
                math::vec3 vp;
                math::quat vr;
                const VehicleSeat3D* seat = w.try_get<VehicleSeat3D>(r.vehicle);
                if (seat && pose_of(w, r.vehicle, vp, vr)) step_out(f, e, r, t, exit_point(w, r.vehicle, *seat, 0.3f, vp, vr), vr, true);
                else step_out(f, e, r, t, t.position, t.rotation, true);
                r.enter = 0;
                return;
            }
        }
        const bool pressed = r.use_input && in && in->has_action(kInteract) && in->pressed(kInteract);
        if (!r.enter && !pressed) return;
        r.enter = 0;
        if (r.seated) {
            VehicleSeat3D* seat = w.try_get_mut<VehicleSeat3D>(r.vehicle);
            const Vehicle3D* v = w.try_get<Vehicle3D>(r.vehicle);
            if (v && std::fabs(v->speed) > seat->max_exit_speed) return;
            math::vec3 vp;
            math::quat vr;
            if (!pose_of(w, r.vehicle, vp, vr)) return;
            const CharacterController3D* kept = st.stash.find(e);
            step_out(f, e, r, t, exit_point(w, r.vehicle, *seat, kept ? kept->radius : 0.3f, vp, vr), vr);
            return;
        }
        // Board the nearest free seat in reach.
        Entity best;
        f32 best_d = 0.0f;
        f.query<VehicleSeat3D, Vehicle3D>().each([&](Entity car, VehicleSeat3D& seat, Vehicle3D&) {
            if (seat.locked || !seat.driver.is_null()) return;
            math::vec3 vp;
            math::quat vr;
            if (!pose_of(w, car, vp, vr)) return;
            const f32 d = math::min(math::length(t.position - (vp + math::rotate(vr, seat.seat))),
                                    math::length(t.position - (vp + math::rotate(vr, seat.exit))));
            if (d > seat.enter_radius || (!best.is_null() && d >= best_d)) return;
            best = car;
            best_d = d;
        });
        if (best.is_null()) return;
        VehicleSeat3D& seat = *w.try_get_mut<VehicleSeat3D>(best);
        Vehicle3D& v = *w.try_get_mut<Vehicle3D>(best);
        seat.saved = 0;
        if (!v.use_input) {
            v.use_input = 1;
            seat.saved |= kSavedInput;
        }
        if (VehicleDriver3D* d = w.try_get_mut<VehicleDriver3D>(best); d && d->drive) {
            d->drive = 0;
            seat.saved |= kSavedDriver;
        }
        v.handbrake = 0.0f;
        seat.driver = e;
        if (const CharacterController3D* ctl = w.try_get<CharacterController3D>(e)) {
            st.stash.insert_or_assign(e, *ctl);
            f.commands().remove<CharacterController3D>(e);
        }
        r.seated = 1;
        r.vehicle = best;
        follow(w, r.camera, best);
        f.events().push(VehicleEntered3D{e, best});
        if (CarOwner3D* own = w.try_get_mut<CarOwner3D>(best); own && !own->stolen) {  // M-CARJACK
            own->stolen = 1;
            const LocalTransform* ct = w.try_get<LocalTransform>(best);
            const math::vec3 at = ct ? ct->position : math::vec3{};
            report_crime(w, e, own->heat, at, best, !own->needs_witness || crime_seen(f, e, at));
        }
    });
}

// FixedUpdate_Physics, after the vehicle state: a seated rider rides at the seat.
void kit3d_seat_pose(Frame& f) {
    World& w = f.world();
    f.query<Rider3D, LocalTransform>().each([&](Entity, Rider3D& r, LocalTransform& t) {
        if (!r.seated) return;
        const VehicleSeat3D* seat = r.vehicle.is_null() || !w.alive(r.vehicle) ? nullptr : w.try_get<VehicleSeat3D>(r.vehicle);
        math::vec3 vp;
        math::quat vr;
        if (!seat || !pose_of(w, r.vehicle, vp, vr)) return;
        t.position = vp + math::rotate(vr, seat->seat);
        t.rotation = vr;
    });
}

void on_rider_removed(World& w, Entity e) {
    Seats3DState* st = w.try_resource<Seats3DState>();
    if (st) st->stash.erase(e);
}

}  // namespace

void install_seats3d(World& world, asset::AssetSystem* assets) {
    install_vehicles3d(world, assets);
    if (world.try_resource<Seats3DState>()) return;
    world.set_resource(Seats3DState{});
    world.register_components<VehicleSeat3D, Rider3D>();
    world.register_event<VehicleEntered3D>(Phase::FixedUpdate_Pre);
    world.register_event<VehicleExited3D>(Phase::FixedUpdate_Pre);
    world.set_hooks<Rider3D>(nullptr, &on_rider_removed);
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_seats", &kit3d_seats, "kit3d_vehicles");
    world.add_system(Phase::FixedUpdate_Physics, "kit3d_seat_pose", &kit3d_seat_pose, "kit3d_vehicle_state");
}

}  // namespace mce::kit
