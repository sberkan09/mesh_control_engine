#pragma once
// Wanted level and police (M-OPENWORLD C3): crimes raise heat on an offender, police units see it, chase it,
// shoot it and give up when they lose it. kit::install_stock installs it.
//
//   Wanted3D   on the offender (the player): heat, the level made from it (0..max_level), and how crimes count
//   Police3D   on a police unit: when it responds, what it sees, and a small state machine
//              (Idle, Pursue, Attack, Search)
//
// Crimes. `Damage3D` and `Died3D` (combat3d.h) turn into heat when the instigator (or the entity it hangs
// under) has a Wanted3D and the victim's Health3D `team` is `civilian_team` or `police_team` (hits and kills
// each have a heat value). A hit on a police unit is always known; a civilian hit counts only when a living
// Police3D within `witness_range` of the victim has a clear line to it (`require_witness` 0: always). Scripts
// and other systems call `report_crime`. Each counted crime pushes `Crime3D`.
//
// Level. `level` is heat / `heat_per_level` rounded up, at most `max_level`. A change pushes
// `WantedChanged3D`. Heat falls by `decay` per second once no police unit has seen the offender for
// `cooldown_delay` seconds (a new crime restarts that wait); at level 0 every unit goes back to Idle.
//
// Police3D. A unit whose `min_level` is above the offender's level ignores it. Otherwise it looks for the
// nearest offender within `sight_range` with a clear line (eye `eye_height` above the entity): seen, it is
// Pursue (further than `attack_range`, or below `attack_level`) or Attack; lost, it is Search for
// `search_time` seconds at the place it last saw the offender, then Idle. In Attack a unit with a Weapon3D
// on its own entity turns to face the target (`aim`) and sets `fire`. With `drive_nav` a NavAgent3D follows
// the target (Pursue), walks to where it was last seen (Search) and stands still (Attack, Idle). A unit with
// an AiBrain also gets the blackboard keys "wanted" (number), "police_state" (number), "target" (entity,
// cleared when none) and "last_known" (vec3), so a behavior tree can take over the movement.
//
// Arrest (M-ARREST). A unit that sees the offender within `arrest_range` of it, at a wanted level of at most
// `arrest_max_level`, with the offender moving no faster than `arrest_speed`, builds `arrest_progress` for
// `arrest_time` seconds (it falls back at the same rate when any of that stops being true); the offender is then
// arrested: `Arrested3D` is pushed and the wanted level cleared (`clear_wanted`). What an arrest means for the
// game (losing weapons, a respawn at the station) is the game's, on that event. `arrest_range` 0 turns it off.
//
// Dispatch (M-ARREST 2). An idle unit with `dispatch_range` above 0 whose `min_level` the offender has reached,
// and which is within that range of `Wanted3D::last_known`, takes the offender as its target and Searches there
// (reinforcements: put units with a higher `min_level` and a long range around the map). It does not need to see it.
//
// Police cars (M-ARREST 3). `drive_car` 1 on a unit that has a Vehicle3D (with `use_input` 0) writes the car's
// `throttle`, `steer`, `brake` and `handbrake`: Pursue and Attack drive at the target, Search at the last known place,
// Idle stops. It steers at the point, slows for tight turns and for the last 12 m, and brakes to a stop inside 4 m
// (so a car beside a standing offender is still a unit that can arrest it). A car is not aimed: `aim` does not turn it.
// A roadblock is a car with `drive_car` 0 placed across the road.
//
// "kit3d_wanted" runs in FixedUpdate_Pre after "kit3d_health"; "kit3d_respawn" (M-WASTED) after it. Everything is in the components, so it is
// deterministic, hashed and saved.

#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
class Frame;
}

namespace mce::kit {

struct Wanted3D {
    f32 heat = 0.0f;
    f32 heat_per_level = 20.0f;
    f32 hit_civilian = 5.0f;        // heat per hit on a civilian_team victim
    f32 kill_civilian = 15.0f;
    f32 hit_police = 15.0f;         // heat per hit on a police_team victim
    f32 kill_police = 30.0f;
    f32 witness_range = 40.0f;      // m: police farther from a civilian victim saw nothing
    f32 cooldown_delay = 10.0f;     // s unseen before heat decays
    f32 decay = 2.0f;               // heat/s
    f32 unseen_time = 0.0f;         // engine-written: s since a unit saw it (or a crime)
    math::vec3 last_known;          // engine-written: where a unit last saw it
    u32 seen_by = 0;                // engine-written: units that see it now
    u32 crimes = 0;                 // engine-written: crimes counted
    u8 max_level = 5;
    u8 level = 0;                   // engine-written
    u8 police_team = 2;             // Health3D::team of police
    u8 civilian_team = 3;           // Health3D::team of civilians
    u8 require_witness = 1;
    u8 pad_[3] = {};
};
MCE_COMPONENT(Wanted3D, (heat, Save, Range{0, 1000}), (heat_per_level, Range{1, 1000}), (hit_civilian, Range{0, 1000}),
              (kill_civilian, Range{0, 1000}), (hit_police, Range{0, 1000}), (kill_police, Range{0, 1000}),
              (witness_range, Range{0, 1000}, Help{"m"}), (cooldown_delay, Range{0, 3600}, Help{"s unseen before heat decays"}),
              (decay, Range{0, 1000}, Help{"heat/s"}), (unseen_time, Save, ReadOnly), (last_known, Save, ReadOnly),
              (seen_by, ReadOnly, Transient), (crimes, Save, ReadOnly), (max_level, Range{1, 10}), (level, ReadOnly, Transient),
              (police_team, Help{"Health3D::team of police units"}), (civilian_team, Help{"Health3D::team of civilians"}),
              (require_witness, Help{"1: a civilian hit counts only when a police unit sees the victim"}))

enum class PoliceState : u8 { Idle, Pursue, Attack, Search };
MCE_ENUM(PoliceState, Idle, Pursue, Attack, Search)

struct Police3D {
    u8 min_level = 1;               // responds from this wanted level
    u8 attack_level = 2;            // shoots from this wanted level (below it, it only follows)
    PoliceState state = PoliceState::Idle;  // engine-written
    u8 drive_nav = 1;               // 1: drives the entity's NavAgent3D
    u8 aim = 1;                     // 1: Attack turns the entity to the target and sets its Weapon3D's `fire`
    u8 arrest_max_level = 1;        // arrests up to this wanted level (above it, it only chases and shoots)
    u8 drive_car = 0;               // 1: drives the entity's Vehicle3D toward the target (M-ARREST 3)
    u8 pad_[1] = {};
    f32 sight_range = 30.0f;        // m
    f32 attack_range = 20.0f;       // m
    f32 search_time = 8.0f;         // s at the last known place before giving up
    f32 eye_height = 1.6f;          // m
    f32 state_time = 0.0f;          // engine-written: s in the state
    Entity target;                  // engine-written: the offender it follows
    math::vec3 last_known;          // engine-written
    f32 arrest_range = 2.5f;        // m from the offender at which it can cuff it; 0 = never arrests
    f32 arrest_time = 2.0f;         // s the offender must stand still within range
    f32 arrest_speed = 1.5f;        // m/s: faster than this it is not standing still
    f32 arrest_progress = 0.0f;     // engine-written: s toward the arrest
    f32 dispatch_range = 0.0f;      // m: an idle unit this close to the offender's last known place is sent there (0 = off)
};
MCE_COMPONENT(Police3D, (min_level, Range{1, 10}, Help{"responds from this wanted level"}),
              (attack_level, Range{1, 10}, Help{"shoots from this wanted level"}), (state, ReadOnly, Transient),
              (drive_nav, Help{"1: drives the NavAgent3D"}), (aim, Help{"1: Attack faces the target and fires the Weapon3D"}),
              (sight_range, Range{0, 1000}, Help{"m"}), (attack_range, Range{0, 1000}, Help{"m"}),
              (search_time, Range{0, 600}, Help{"s at the last known place"}), (eye_height, Range{0, 10}),
              (state_time, ReadOnly, Transient), (target, Save, ReadOnly), (last_known, Save, ReadOnly),
              (drive_car, Help{"1: drives the Vehicle3D (throttle, steer, brake) toward the target"}),
              (arrest_max_level, Range{0, 10}, Help{"arrests up to this wanted level; 0 = never"}),
              (arrest_range, Range{0, 50}, Help{"m from the offender at which it can arrest; 0 = never"}),
              (arrest_time, Range{0, 60}, Help{"s the offender must stand still within range"}),
              (arrest_speed, Range{0, 20}, Help{"m/s: faster than this it is not standing still"}),
              (arrest_progress, ReadOnly, Transient),
              (dispatch_range, Range{0, 2000}, Help{"m: an idle unit this close to the last known place is sent there; 0 = off"}))

// Events (producer FixedUpdate_Pre).
struct Crime3D {
    Entity offender;
    Entity victim;
    f32 heat = 0.0f;                // heat added
    math::vec3 position;
};
struct WantedChanged3D {
    Entity offender;
    u32 from = 0;
    u32 to = 0;
};
// An offender arrested by `by` (M-ARREST); its wanted level is already cleared.
struct Arrested3D {
    Entity offender;
    Entity by;
    math::vec3 position;
};
MCE_EVENT(Crime3D, offender, victim, heat, position)
MCE_EVENT(Arrested3D, offender, by, position)
MCE_EVENT(WantedChanged3D, offender, from, to)

// Adds `heat` to `offender`'s Wanted3D as a crime at `position` (`victim` may be null) and pushes Crime3D. The
// caller decides whether anyone saw it: a crime with `witnessed` false counts nothing. False when it did not
// count (no Wanted3D, no heat, not witnessed). From a FixedUpdate_Pre system or between ticks; the level is
// recomputed at once and WantedChanged3D pushed.
bool report_crime(World& world, Entity offender, f32 heat, math::vec3 position, Entity victim = {}, bool witnessed = true);

// Whether a crime of `offender` at `at` counts as seen: always when its Wanted3D has `require_witness` 0, otherwise
// when a living Police3D unit within the offender's `witness_range` has a clear line to `at`. False without a Wanted3D.
bool crime_seen(Frame& frame, Entity offender, math::vec3 at);

// Busted and wasted (M-WASTED). `Respawn3D` on the player (the entity with a Health3D): after it dies (Died3D) or is
// arrested (Arrested3D) it waits `delay` seconds, then wakes up at the nearest `RespawnPoint3D` (a Station after an
// arrest, a Hospital after a death; any point when the wanted kind is missing; where it is when there is none) with full
// health and no wanted level, and `Respawned3D` is pushed. With `caption` 1 and captions installed the wait shows
// "WASTED" or "BUSTED" (a `ShowCaption` for `delay` seconds). What it costs (money, weapons) is the game's, on that event.
enum class RespawnKind : u8 { Hospital, Station };
MCE_ENUM(RespawnKind, Hospital, Station)

struct RespawnPoint3D {
    RespawnKind kind = RespawnKind::Hospital;
    u8 pad_[3] = {};
};
MCE_COMPONENT(RespawnPoint3D, (kind, Help{"Hospital: where a dead player wakes up; Station: where an arrested one does"}))

struct Respawn3D {
    f32 delay = 3.0f;               // s between dying or being arrested and waking up
    f32 timer = 0.0f;               // engine-written: s left (0 when idle)
    u8 reason = 0;                  // engine-written: 0 none, 1 busted, 2 wasted
    u8 caption = 1;                 // 1: shows "BUSTED" / "WASTED" through captions.h while it waits
    u8 pad_[2] = {};
    u16 fine_busted = 100;          // cash taken from its Wallet3D (never below zero) when it wakes up busted
    u16 fine_wasted = 200;          // ... and wasted
};
MCE_COMPONENT(Respawn3D, (caption, Help{"1: shows BUSTED or WASTED as a caption while the player waits (needs install_captions)"}), (delay, Range{0, 600}, Help{"s between dying or being arrested and waking up"}),
              (fine_busted, Help{"cash taken from its Wallet3D when it wakes up busted (never below zero)"}),
              (fine_wasted, Help{"cash taken from its Wallet3D when it wakes up wasted (never below zero)"}),
              (timer, ReadOnly, Transient), (reason, ReadOnly, Transient, Help{"0 none, 1 busted, 2 wasted"}))

// An entity woke up again (M-WASTED). `reason` 1: busted, 2: wasted.
struct Respawned3D {
    Entity entity;
    u32 reason = 0;
    math::vec3 position;
};
MCE_EVENT(Respawned3D, entity, reason, position)

// Money (M-CASH). `Wallet3D` on whoever carries cash; `CashDrop3D` on an entity with a Health3D credits its killer (when
// the killer has a wallet) a random amount in [min, max] when it dies. Every credit and debit pushes CashChanged3D.
struct Wallet3D {
    i32 cash = 0;
};
MCE_COMPONENT(Wallet3D, (cash, Save, Range{0, 1000000000}, Help{"cash carried; the systems never take it below zero"}))

struct CashDrop3D {
    i32 min = 5;
    i32 max = 25;
};
MCE_COMPONENT(CashDrop3D, (min, Range{0, 1000000}, Help{"least cash the killer gets"}), (max, Range{0, 1000000}, Help{"most cash the killer gets"}))

struct CashChanged3D {
    Entity entity;
    i32 delta = 0;  // what was actually added (+) or taken (-)
    i32 cash = 0;   // the wallet after the change
};
MCE_EVENT(CashChanged3D, entity, delta, cash)

// Mission pay (M-MISSIONPAY). `MissionReward3D` on a Mission entity (missions.h): when the mission completes, its
// actor's Wallet3D is credited `cash` (nothing when the actor has no wallet, or the mission fails). Needs
// install_wanted3d for the component to exist.
struct MissionReward3D {
    i32 cash = 100;
};
MCE_COMPONENT(MissionReward3D, (cash, Range{0, 1000000}, Help{"cash the mission's actor is paid when it completes"}))

// Car theft (M-CARJACK). `CarOwner3D` on a vehicle (one with a VehicleSeat3D, seats3d.h) that belongs to someone: the
// first time a rider with a Wanted3D boards it, `report_crime` adds `heat` to that rider and `stolen` is set, so the same
// car counts once. With `needs_witness` 0 the car's owner calls it in; with 1 the theft counts only when `crime_seen`
// (a police unit sees the car), and an unseen theft is still marked stolen. Needs install_wanted3d for the component to exist.
struct CarOwner3D {
    f32 heat = 8.0f;
    u8 stolen = 0;  // engine-written
    u8 needs_witness = 0;
    u8 pad_[2] = {};
};
MCE_COMPONENT(CarOwner3D, (heat, Range{0, 1000}, Help{"wanted heat the first boarding adds to the rider"}), (stolen, Save, ReadOnly),
              (needs_witness, Help{"1: the theft counts only when a police unit sees the car"}))

// Adds `amount` (> 0) to `e`'s wallet, saturating, and pushes CashChanged3D. False without a Wallet3D or when nothing changed.
bool give_cash(World& world, Entity e, i32 amount);
// Takes up to `amount` (> 0) from `e`'s wallet, never below zero, and pushes CashChanged3D. Returns what was taken (0 without a Wallet3D).
i32 take_cash(World& world, Entity e, i32 amount);

// Pay-and-spray (M-PAYSPRAY). `PaySpray3D` on a placed entity (a garage): an offender with a Wanted3D at level 1 or
// more that stands within `radius` of it, and whose Wallet3D holds `price_per_level` x its level, pays that and has
// its wanted level cleared (`clear_wanted`); `Sprayed3D` is pushed. Too poor: nothing happens. Whether the police may
// be in sight is the game's rule; the garage does not check it. System "kit3d_payspray" (after "kit3d_cash").
struct PaySpray3D {
    f32 radius = 3.0f;           // m, from the entity
    u16 price_per_level = 100;   // cash per wanted level
    u8 enabled = 1;
    u8 pad_ = 0;
};
MCE_COMPONENT(PaySpray3D, (radius, Range{0.1f, 100}, Help{"m: an offender this close pays to clear its level"}),
              (price_per_level, Help{"cash per wanted level"}), enabled)

// An offender paid a PaySpray3D and lost its wanted level.
struct Sprayed3D {
    Entity entity;
    Entity garage;
    i32 paid = 0;
    u32 level = 0;  // the level it had
};
MCE_EVENT(Sprayed3D, entity, garage, paid, level)

// Ammo shop (M-AMMOSHOP). `AmmoShop3D` on a placed entity: a buyer with a Weapon3D and a Wallet3D on the same entity that
// stands within `radius` has the weapon's `reserve` topped up to `stock_to` rounds at `price_per_round` each. A buyer that
// cannot afford the whole top-up buys as many rounds as it can pay for. Weapons that never run dry (`magazine` 0 or
// `infinite_ammo`) are not sold to. `AmmoBought3D` is pushed per sale. System "kit3d_ammoshop" (after "kit3d_payspray").
struct AmmoShop3D {
    f32 radius = 3.0f;            // m, from the entity
    u16 price_per_round = 2;      // cash per round
    u16 stock_to = 120;           // the reserve it fills up to
    u8 enabled = 1;
    u8 pad_[3] = {};
};
MCE_COMPONENT(AmmoShop3D, (radius, Range{0.1f, 100}, Help{"m: a buyer this close is sold ammo"}), (price_per_round, Help{"cash per round"}),
              (stock_to, Help{"the reserve a sale fills up to"}), enabled)

// A buyer was sold rounds by an AmmoShop3D.
struct AmmoBought3D {
    Entity entity;
    Entity shop;
    u32 rounds = 0;
    i32 paid = 0;
};
MCE_EVENT(AmmoBought3D, entity, shop, rounds, paid)

// Medic (M-HEALSHOP). `HealShop3D` on a placed entity: a living buyer with a Health3D below its `max` and a Wallet3D that
// stands within `radius` is healed `1` hp per `price_per_hp` cash, up to its `max`; with too little cash it buys the whole
// hit points it can pay for. `HealBought3D` is pushed per sale. System "kit3d_healshop" (after "kit3d_ammoshop").
struct HealShop3D {
    f32 radius = 3.0f;          // m, from the entity
    u16 price_per_hp = 1;       // cash per hit point
    u8 enabled = 1;
    u8 pad_ = 0;
};
MCE_COMPONENT(HealShop3D, (radius, Range{0.1f, 100}, Help{"m: a hurt buyer this close is healed for cash"}), (price_per_hp, Help{"cash per hit point"}),
              enabled)

// A buyer was healed by a HealShop3D.
struct HealBought3D {
    Entity entity;
    Entity shop;
    f32 hp = 0.0f;
    i32 paid = 0;
};
MCE_EVENT(HealBought3D, entity, shop, hp, paid)

// Cash pickup (M-CASHPICKUP). `CashPickup3D` on a placed entity: a holder of a Wallet3D that comes within `radius` is paid
// `cash` (`give_cash`) and `CashPickedUp3D` is pushed. With `respawn` 0 the pickup entity is then destroyed; otherwise it
// waits `respawn` seconds (engine-written `left`) before it can be taken again, and a game hides it meanwhile from the
// event. A holder whose wallet is full takes nothing. System "kit3d_cashpickup" (after "kit3d_healshop").
struct CashPickup3D {
    i32 cash = 50;
    f32 radius = 1.5f;   // m, from the entity
    f32 respawn = 0.0f;  // s until it can be taken again; 0: destroyed when taken
    f32 left = 0.0f;     // engine-written: s until it can be taken
};
MCE_COMPONENT(CashPickup3D, (cash, Range{1, 1000000}, Help{"cash a Wallet3D holder is paid for touching it"}),
              (radius, Range{0.1f, 100}, Help{"m"}), (respawn, Range{0, 100000}, Help{"s before it can be taken again; 0: destroyed when taken"}),
              (left, ReadOnly, Transient))

// A holder took a CashPickup3D.
struct CashPickedUp3D {
    Entity entity;
    Entity pickup;
    i32 cash = 0;
};
MCE_EVENT(CashPickedUp3D, entity, pickup, cash)

// Chop shop (M-CHOPSHOP). `ChopShop3D` on a placed entity (a scrapyard): a stolen car (`CarOwner3D.stolen` 1, with a
// VehicleSeat3D) whose driver holds a Wallet3D and that stands within `radius` is bought for `price`: the driver is paid
// (`give_cash`), the car is destroyed (the driver steps out as for any vanished vehicle) and `CarSold3D` is pushed. Needs the
// seats of install_seats3d. System "kit3d_chopshop" (after "kit3d_cashpickup").
struct ChopShop3D {
    f32 radius = 4.0f;  // m
    i32 price = 300;
    u8 enabled = 1;
    u8 pad_[3] = {};
};
MCE_COMPONENT(ChopShop3D, (radius, Save, Range{0.1f, 1000}, Help{"m: a stolen car this close is bought"}),
              (price, Save, Range{1, 1000000}, Help{"cash paid to the driver for a stolen car"}), (enabled, Save))

// A driver sold a stolen car to a ChopShop3D.
struct CarSold3D {
    Entity seller;
    Entity shop;
    Entity car;
    i32 paid = 0;
};
MCE_EVENT(CarSold3D, seller, shop, car, paid)

// Clears the heat and level of `offender` (a pay-and-spray, a mission end). Units go Idle on the next tick.
void clear_wanted(World& world, Entity offender);

// Registers the components and events and adds "kit3d_wanted" (after install_combat3d, which it calls). Idempotent.
void install_wanted3d(World& world);

}  // namespace mce::kit
