// The .mcnav importer (M-FUZZ 2b): a bake is stored as baked, but only when it is a version-2 file whose trailing
// hash matches. Everything else stops at the cook with a message that says what to do.

#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/cook.h"
#include "mce/hash.h"
#include "mce/physics3d/navmesh3d.h"

using namespace mce;
using namespace cook_test;

namespace {

std::string diags_of(const cook::CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

std::string bake() {
    physics::NavGeometry3D g;
    g.add_box(math::translation({0.0f, -0.5f, 0.0f}), {10.0f, 0.5f, 10.0f});
    physics::NavBakeSettings3D s;
    s.cell_size = 0.25f;
    s.tile_size = 32;
    Array<byte> blob;
    REQUIRE(physics::bake_navmesh(g, s, blob));
    return std::string(reinterpret_cast<const char*>(blob.data()), blob.size());
}

cook::CookReport cook_bytes(const std::string& name, const std::string& bytes) {
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/navmesh/n.mcnav", bytes);
    write_file(root + "/assets/navmesh/n.mcnav.mceasset",
               R"({ "guid": "5f0a29ed-c6da-4360-bb6f-71a76b74287a", "importer": "navmesh", "importer_version": 2, "settings": {} })");
    cook::CookService svc;
    return svc.cook(StringView(root.c_str()), cook::CookOptions{});
}

}  // namespace

TEST_CASE("navmesh importer: a version-2 bake cooks; damage, old versions and fragments are refused with a hint") {
    const std::string good = bake();
    {
        const cook::CookReport r = cook_bytes("nav_good", good);
        INFO(diags_of(r));
        CHECK(r.ok());
    }
    {  // one flipped byte inside the layers: the hash no longer matches
        std::string bad = good;
        bad[bad.size() / 2] = static_cast<char>(bad[bad.size() / 2] ^ 0x5A);
        const cook::CookReport r = cook_bytes("nav_bad_hash", bad);
        CHECK_FALSE(r.ok());
        CHECK(diags_of(r).find("re-bake") != std::string::npos);
    }
    {  // version 1: no trailer
        std::string old = good.substr(0, good.size() - 8);
        old[4] = 1;
        const cook::CookReport r = cook_bytes("nav_v1", old);
        CHECK_FALSE(r.ok());
        CHECK(diags_of(r).find("re-bake") != std::string::npos);
    }
    {  // shorter than a header and a trailer
        const cook::CookReport r = cook_bytes("nav_short", good.substr(0, 12));
        CHECK_FALSE(r.ok());
        CHECK(diags_of(r).find("truncated") != std::string::npos);
    }
    {  // not a bake at all
        const cook::CookReport r = cook_bytes("nav_magic", std::string(64, 'x'));
        CHECK_FALSE(r.ok());
        CHECK(diags_of(r).find("bad magic") != std::string::npos);
    }
}
