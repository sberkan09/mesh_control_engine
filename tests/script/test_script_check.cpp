// M-SCRIPT 4: script::check and mce.d.luau. Names a script spells out are checked against the
// reflection registry (with a "did you mean"), removed globals say what to use instead, and Luau's
// type checker runs against the generated definitions; every problem comes back with its position.

#include <doctest/doctest.h>

#include <string>

#include "mce/refl/macros.h"
#include "mce/script/compiler.h"

using namespace mce;

namespace sccheck {

struct Mover {
    math::vec3 position;
    math::vec3 velocity;
    f32 bounce = 1.0f;
};
MCE_COMPONENT(Mover, position, velocity, bounce)

struct Bumped {
    Entity who;
    f32 force = 0.0f;
};
MCE_EVENT(Bumped, who, force)

}  // namespace sccheck

namespace {

struct Problems {
    Array<script::CompileError> list;
    std::string all;  // "line:col message" per line
};

Problems check(const char* source) {
    Problems p;
    script::check("s", source, p.list);
    for (const script::CompileError& e : p.list)
        p.all += std::to_string(e.line) + ":" + std::to_string(e.col) + " " + e.message.c_str() + "\n";
    return p;
}

bool has(const Problems& p, const char* text) { return p.all.find(text) != std::string::npos; }

}  // namespace

TEST_CASE("script check: names the engine does not know, with the closest one") {
    const Problems p = check(R"(local P = mce.field("Movr", "position")
local V = mce.field("Mover", "velocty")
function update()
    mce.each("Mover", "Mover", function(e)
        mce.get(e, "Mover", "bounse")
        mce.set(e, "Mover", "positon", vector.create(0, 0, 0))
        mce.add(e, "Mover", { bounc = 1 })
        mce.spawn({ Mover = { position = vector.create(1, 2, 3) }, Nothing = true })
        mce.emit("Bumpd", { force = 1 })
        mce.emit("Bumped", { forse = 1 })
        local n = #mce.events("Bumped") + #mce.events("Mover")
    end)
end
)");
    INFO(p.all);
    CHECK(has(p, "1:21 unknown component 'Movr' (did you mean 'Mover'?)"));
    CHECK(has(p, "2:30 'Mover' has no field 'velocty' (did you mean 'velocity'?)"));
    CHECK(has(p, "5:29 'Mover' has no field 'bounse' (did you mean 'bounce'?)"));
    CHECK(has(p, "6:29 'Mover' has no field 'positon' (did you mean 'position'?)"));
    CHECK(has(p, "7:31 'Mover' has no field 'bounc' (did you mean 'bounce'?)"));
    CHECK(has(p, "unknown component 'Nothing'"));
    CHECK(has(p, "9:18 unknown event 'Bumpd' (did you mean 'Bumped'?)"));
    CHECK(has(p, "10:30 'Bumped' has no field 'forse' (did you mean 'force'?)"));
    CHECK(has(p, "unknown event 'Mover'"));
    CHECK(p.list.size() == 9);
}

TEST_CASE("script check: removed globals say what to use instead") {
    const Problems p = check(R"(function update()
    print("hi")
    local r = math.random()
    local t = os.clock()
end
)");
    INFO(p.all);
    CHECK(has(p, "2:5 'print' is not available in scripts; use mce.log"));
    CHECK(has(p, "3:15 'math.random' is not available in scripts; use mce.random()"));
    CHECK(has(p, "4:15 'os' is not available in scripts"));
}

TEST_CASE("script check: Luau's type checker runs against mce.d.luau") {
    const Problems p = check(R"(function update()
    mce.spwan({})
    local hit = mce.raycast2d(vector.create(0, 0, 0), vector.create(1, 0, 0))
    undefined_helper()
end
)");
    INFO(p.all);
    CHECK(has(p, "2:5 Key 'spwan' not found"));
    CHECK(has(p, "3:17"));  // raycast2d needs its max distance
    CHECK(has(p, "4:5 Unknown global 'undefined_helper'"));

    // The checks pass a script that uses the API as documented.
    const Problems ok = check(R"(
local POS = mce.field("Mover", "position")
local VEL = mce.field("Mover", "velocity")
function update()
    mce.each("Mover", function(e)
        local p, v = mce.get(e, POS, VEL)
        mce.set(e, POS, p + v * mce.dt)
        local hit = mce.raycast2d(p, vector.create(1, 0, 0), 5, e)
        if hit and mce.alive(hit.entity) then mce.emit("Bumped", { who = hit.entity, force = hit.distance }) end
    end)
    for _, b in mce.events("Bumped") do mce.log(tostring(b.force)) end
    if mce.pressed("Jump") then mce.spawn({ Mover = { bounce = 0.5 } }) end
    if mce.tick() > 100 then mce.load_scene("scenes/next.mcscene") end
end
)");
    INFO(ok.all);
    CHECK(ok.list.empty());
}

TEST_CASE("script check: --!strict asks for strict typing") {
    const char* body = "\nfunction update()\n    local x = mce.tick()\n    x = \"text\"\nend\n";
    CHECK(check(body).list.empty());
    const Problems strict = check((std::string("--!strict") + body).c_str());
    INFO(strict.all);
    CHECK(has(strict, "4:9 Expected this to be 'number', but got 'string'"));
}

TEST_CASE("script check: syntax errors stop the other checks") {
    const Problems p = check("function update()\n  local x = = 1\n  print(x)\nend\n");
    REQUIRE(p.list.size() == 1);
    CHECK(p.list[0].line == 2);
    CHECK(p.list[0].col == 13);
}

TEST_CASE("script check: compile fails with the first problem at file:line:col") {
    Result<Array<u8>> r = script::compile("scripts/a.luau", "function update()\n  mce.has(1, \"Movr\")\nend\n");
    REQUIRE_FALSE(r);
    CHECK(std::string(r.error().path.c_str()) == "scripts/a.luau:2:14: unknown component 'Movr' (did you mean 'Mover'?)");
}

TEST_CASE("script check: mce.d.luau lists the registry, sorted") {
    Array<char> defs;
    script::write_definitions(defs);
    const std::string text(defs.data(), defs.size());
    CHECK(text.find("declare mce: {") != std::string::npos);
    CHECK(text.find("raycast2d: (origin: vector, dir: vector, max: number, skip: Entity?) -> RayHit?") != std::string::npos);
    CHECK(text.find("\"Mover\"") != std::string::npos);
    CHECK(text.find("--   Mover { position: vector, velocity: vector, bounce: number }") != std::string::npos);
    CHECK(text.find("--   Bumped { who: Entity, force: number }") != std::string::npos);
    CHECK(text.find("\"LocalTransform\"") < text.find("\"Mover\""));
    Array<char> again;
    script::write_definitions(again);
    CHECK(std::string(again.data(), again.size()) == text);
}

TEST_CASE("script check: the inventory global is typed") {
    const Problems ok = check(R"(
function update()
    mce.each("Inventory", function(e)
        inventory.add(e, "potion", 3)
        local id, qty = inventory.slot(e, 1)
        if inventory.has(e, "potion") and inventory.count(e, "potion") > 2 and inventory.weight(e) < 9000 then
            inventory.equip(e, 1, "weapon")
            inventory.unequip(e, "weapon")
            local dropped: number = inventory.drop(e, 2)
        end
        mce.log(tostring(id) .. tostring(qty))
    end)
end
)");
    INFO(ok.all);
    CHECK(ok.list.empty());
    const Problems bad = check(R"(function update()
    inventory.add(1, "potion")
    inventory.eqip(mce.find("x"), 1)
end
)");
    INFO(bad.all);
    CHECK(has(bad, "3:5 Key 'eqip' not found"));
    CHECK_FALSE(bad.list.empty());
}
