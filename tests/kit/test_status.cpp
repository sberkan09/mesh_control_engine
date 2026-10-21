// Timed status effects (M-STATUS 1): regen, poison pulses through apply_damage (Died3D, instigator), armor and max-health
// bonuses applied as a remembered change and given back, refresh instead of stacking, a full slot table, clearing, and a
// bounded run that is the same twice.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/status.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Rig {
    World w;
    u64 t = 0;
    Entity hero, other;
    Rig() {
        install_status(w, nullptr);
        Health3D hp;
        hp.max = 100.0f;
        hp.current = 50.0f;
        hp.armor = 0.25f;
        hp.regen = 0.0f;
        hero = w.spawn_with(hp, StatusEffects{});
        other = w.spawn_with(Health3D{});
    }
    void tick(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    Health3D& hp() { return *w.try_get_mut<Health3D>(hero); }
    StatusEffects& st() { return *w.try_get_mut<StatusEffects>(hero); }
    void apply(const char* id, StatusKind kind, f32 amount, f32 seconds, Entity source = Entity()) {
        ApplyStatus a;
        a.target = hero;
        a.id = id;
        a.kind = static_cast<u32>(kind);
        a.amount = amount;
        a.seconds = seconds;
        a.source = source;
        EventAccess(w).push(a);
    }
    void clear(const char* id = "") {
        ClearStatus c;
        c.target = hero;
        c.id = id;
        EventAccess(w).push(c);
    }
    u32 active() {
        u32 n = 0;
        for (u32 i = 0; i < kStatusSlots; ++i) n += st().key[i] != 0;
        return n;
    }
};

}  // namespace

TEST_CASE("status: regen heals per second, caps at max and ends on time") {
    Rig r;
    r.apply("tonic", StatusKind::Regen, 10.0f, 2.0f);
    r.tick(3);
    CHECK(r.active() == 1);
    r.tick(60);  // about one second of regen: ~10 hp
    CHECK(r.hp().current == doctest::Approx(60.0f).epsilon(0.08));
    r.tick(90);  // past the two seconds
    CHECK(r.active() == 0);
    CHECK(r.hp().current == doctest::Approx(70.0f).epsilon(0.05));  // 10 hp/s for 2 s
    r.apply("big", StatusKind::Regen, 1000.0f, 1.0f);
    r.tick(70);
    CHECK(r.hp().current == 100.0f);  // capped
}

TEST_CASE("status: poison pulses through apply_damage, credits the source and can kill") {
    Rig r;
    r.hp().armor = 0.0f;
    r.apply("venom", StatusKind::Poison, 10.0f, 3.0f, r.other);
    r.tick(3);
    r.tick(60);  // one second: two pulses of 5
    CHECK(r.hp().current == doctest::Approx(40.0f).epsilon(0.1));
    CHECK(r.hp().last_attacker == r.other);
    CHECK(r.hp().hits >= 1);
    // a weak victim dies from it and Died3D carries the source
    r.hp().current = 6.0f;
    u32 died = 0;
    Entity killer;
    for (u32 i = 0; i < 90; ++i) {
        r.tick();
        for (const Died3D& d : EventAccess(r.w).read<Died3D>()) {
            ++died;
            killer = d.killer;
        }
    }
    CHECK(died == 1);
    CHECK(killer == r.other);
    CHECK(r.hp().dead == 1);
}

TEST_CASE("status: armor and max-health bonuses are applied as a change and given back") {
    Rig r;
    r.apply("shield", StatusKind::Armor, 0.5f, 1.0f);
    r.apply("vigor", StatusKind::MaxHealth, 40.0f, 1.0f);
    r.tick(3);
    CHECK(r.hp().armor == doctest::Approx(0.75f));
    CHECK(r.hp().max == doctest::Approx(140.0f));
    r.tick(20);  // nothing accumulates
    CHECK(r.hp().armor == doctest::Approx(0.75f));
    CHECK(r.hp().max == doctest::Approx(140.0f));
    r.hp().max += 10.0f;  // a level-up while the effect lasts stays
    r.tick(100);
    CHECK(r.active() == 0);
    CHECK(r.hp().armor == doctest::Approx(0.25f));
    CHECK(r.hp().max == doctest::Approx(110.0f));
    // the clamp is not remembered as added
    r.apply("huge", StatusKind::Armor, 2.0f, 1.0f);
    r.tick(5);
    CHECK(r.hp().armor == doctest::Approx(1.0f));
    r.clear("huge");
    r.tick(3);
    CHECK(r.hp().armor == doctest::Approx(0.25f));
}

TEST_CASE("status: max-health loss clamps current, and re-applying an id refreshes it") {
    Rig r;
    r.hp().current = 100.0f;
    r.apply("vigor", StatusKind::MaxHealth, 50.0f, 1.0f);
    r.tick(3);
    r.hp().current = 150.0f;
    r.tick(1);
    r.apply("vigor", StatusKind::MaxHealth, 50.0f, 5.0f);  // refresh: still one slot, longer
    r.tick(3);
    CHECK(r.active() == 1);
    CHECK(r.hp().max == doctest::Approx(150.0f));  // not 200: nothing stacked
    r.tick(80);
    CHECK(r.active() == 1);  // the first second passed, the refresh keeps it alive
    r.tick(300);
    CHECK(r.active() == 0);
    CHECK(r.hp().max == doctest::Approx(100.0f));
    CHECK(r.hp().current == doctest::Approx(100.0f));
}

TEST_CASE("status: a full table refuses a new id, bad input is refused, clear sends StatusEnded") {
    Rig r;
    for (u32 i = 0; i < kStatusSlots; ++i) {
        char id[8] = {'e', static_cast<char>('0' + i), 0};
        r.apply(id, StatusKind::Regen, 1.0f, 10.0f);
    }
    r.tick(3);
    CHECK(r.active() == kStatusSlots);
    r.apply("ninth", StatusKind::Regen, 1.0f, 10.0f);
    r.tick(2);
    u32 refused = 0;
    for (const StatusRefused& e : EventAccess(r.w).read<StatusRefused>()) refused += e.id.view() == "ninth";
    CHECK(refused == 1);
    CHECK(r.active() == kStatusSlots);
    r.apply("bad", StatusKind::Regen, 1.0f, 0.0f);  // zero seconds
    r.tick(2);
    refused = 0;
    for (const StatusRefused& e : EventAccess(r.w).read<StatusRefused>()) refused += e.id.view() == "bad";
    CHECK(refused == 1);
    r.clear("e3");
    r.tick(2);
    u32 ended = 0;
    for (const StatusEnded& e : EventAccess(r.w).read<StatusEnded>()) ended += e.id.view() == "e3" && e.expired == 0;
    CHECK(ended == 1);
    CHECK(r.active() == kStatusSlots - 1);
    r.clear();
    r.tick(2);
    CHECK(r.active() == 0);
}

TEST_CASE("status: expiry sends StatusEnded with the id, and the same run twice has the same hash") {
    auto run = []() {
        Rig r;
        r.apply("tonic", StatusKind::Regen, 7.0f, 0.5f);
        r.apply("venom", StatusKind::Poison, 3.0f, 1.0f, r.other);
        r.apply("shield", StatusKind::Armor, 0.1f, 0.7f);
        u32 expired = 0;
        for (u32 i = 0; i < 120; ++i) {
            r.tick();
            for (const StatusEnded& e : EventAccess(r.w).read<StatusEnded>()) expired += e.expired;
        }
        CHECK(expired == 3);
        return r.w.state_hash();
    };
    CHECK(run() == run());
}

TEST_CASE("status: a refresh that changes the kind gives back the old bonus, and clears run before applies") {
    Rig r;
    r.apply("x", StatusKind::Armor, 0.5f, 5.0f);
    r.tick(3);
    CHECK(r.hp().armor == doctest::Approx(0.75f));
    r.apply("x", StatusKind::MaxHealth, 40.0f, 5.0f);
    r.tick(3);
    CHECK(r.hp().armor == doctest::Approx(0.25f));
    CHECK(r.hp().max == doctest::Approx(140.0f));
    // cleanse then re-apply in one tick: the effect ends up on
    r.clear();
    r.apply("y", StatusKind::Regen, 1.0f, 5.0f);
    r.tick(3);
    CHECK(r.active() == 1);
    CHECK(r.hp().max == doctest::Approx(100.0f));
}

TEST_CASE("status: kill credit is per effect, and bad amounts and corrupt saved slots are refused or dropped") {
    Rig r;
    r.hp().armor = 0.0f;
    r.hp().current = 3.0f;
    r.apply("venom", StatusKind::Poison, 20.0f, 3.0f, r.other);
    r.apply("heal", StatusKind::Regen, 0.5f, 3.0f, r.hero);
    Entity killer;
    for (u32 i = 0; i < 90; ++i) {
        r.tick();
        for (const Died3D& d : EventAccess(r.w).read<Died3D>()) killer = d.killer;
    }
    CHECK(killer == r.other);
    Rig q;
    q.apply("neg", StatusKind::Poison, -5.0f, 1.0f);
    q.apply("nan", StatusKind::Armor, std::numeric_limits<f32>::quiet_NaN(), 1.0f);
    q.apply("huge", StatusKind::MaxHealth, 1.0e9f, 1.0f);
    q.tick(3);
    CHECK(q.active() == 0);
    // a corrupt saved slot (NaN amount, kind past the enum) is dropped, Health3D stays finite
    q.apply("ok", StatusKind::Regen, 1.0f, 10.0f);
    q.tick(3);
    q.st().amount[0] = std::numeric_limits<f32>::quiet_NaN();
    q.tick(2);
    CHECK(q.active() == 0);
    CHECK(std::isfinite(q.hp().current));
    q.apply("odd", StatusKind::Regen, 1.0f, 10.0f);
    q.tick(3);
    q.st().kind[0] = 9;
    q.st().pulse_us[0] = 4000000000u;
    q.tick(2);
    CHECK(q.active() == 0);
}
