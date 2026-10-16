// M-AUDIO2 2: .mcsound cooks to a sound set in an Sfx chunk (SFX 1.2) naming its member clips; bad
// sets are cook errors naming the file and the key.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace cook_test;

namespace {

constexpr const char* kStepA = "50d10000-0000-4000-8000-0000000000a1";
constexpr const char* kStepB = "50d10000-0000-4000-8000-0000000000a2";
constexpr const char* kSteps = "50d10000-0000-4000-8000-0000000000a3";
constexpr const char* kTex = "50d10000-0000-4000-8000-0000000000a4";

std::string report(const cook::CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

std::string project(const char* name, const std::string& sound) {
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/sfx/a.wav", make_wav(48000, 1, 4800, 300.0));
    write_file(root + "/assets/sfx/a.wav.mceasset", sidecar(kStepA, "wav"));
    write_file(root + "/assets/sfx/b.wav", make_wav(48000, 1, 4800, 500.0));
    write_file(root + "/assets/sfx/b.wav.mceasset", sidecar(kStepB, "wav"));
    write_file(root + "/assets/tex/t.png", make_gradient_png(4, 4));
    write_file(root + "/assets/tex/t.png.mceasset", sidecar(kTex, "texture"));
    write_file(root + "/assets/sfx/steps.mcsound", sound);
    return root;
}

}  // namespace

TEST_CASE("sound set: .mcsound cooks to an Sfx chunk naming its clips, with its ranges") {
    const std::string root = project("sound_set", std::string(R"({ "format": "mce-sound", "version": 1, "guid": ")") + kSteps +
                                                      R"(", "clips": ["guid:)" + kStepA + R"(", "guid:)" + kStepB +
                                                      R"("], "pick": "sequence", "pitch": [0.9, 1.2], "gain_db": [-4, -1] })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(report(r));
    REQUIRE(r.ok());
    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    auto h = assets.load_sync<asset::Sfx>(asset::parse_guid(kSteps).value());
    REQUIRE(h);
    auto v = asset::SfxView::read(assets.resolve(h.value()));
    REQUIRE(v);
    REQUIRE(v.value().set != nullptr);
    CHECK((v.value().header->flags & asset::SfxFlags::Set) != 0);
    CHECK(v.value().header->frame_count == 0);
    CHECK(v.value().set->pick == asset::SoundSetPick::Sequence);
    CHECK(v.value().set->pitch_min == doctest::Approx(0.9f));
    CHECK(v.value().set->pitch_max == doctest::Approx(1.2f));
    CHECK(v.value().set->gain_min_db == -4.f);
    CHECK(v.value().set->gain_max_db == -1.f);
    CHECK((v.value().set->flags & asset::SoundSetFlags::NoRepeat) != 0);
    REQUIRE(v.value().set_clips.size() == 2);
    CHECK(v.value().set_clips[0] == asset::parse_guid(kStepA).value());
    CHECK(v.value().set_clips[1] == asset::parse_guid(kStepB).value());
    assets.release(asset::AnyHandle(h.value()));
}

TEST_CASE("sound set: bad sets fail the cook naming the key") {
    const std::string a = std::string("\"guid:") + kStepA + "\"";
    const std::string tex = std::string("\"guid:") + kTex + "\"";
    const std::string head = std::string(R"({ "format": "mce-sound", "version": 1, "guid": ")") + kSteps + "\", ";
    const std::string cases[][2] = {
        {head + R"("clips": 3 })", "'clips'"},
        {head + R"("clips": [)" + a + R"(], "pick": "loud" })", "'pick'"},
        {head + R"("clips": [)" + a + R"(], "pitch": [2, 1] })", "'pitch'"},
        {head + R"("clips": [)" + a + R"(], "pitch": [0.1, 1] })", "'pitch'"},
        {head + R"("clips": [)" + tex + R"(] })", "clips[0]"},
        {head + R"("clips": [)" + a + R"(], "volume": 1 })", "volume"},
    };
    u32 n = 0;
    for (const auto& c : cases) {
        const std::string root = project(("sound_set_bad" + std::to_string(n++)).c_str(), c[0]);
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        const std::string text = report(r);
        INFO(c[0] << "\n" << text);
        CHECK_FALSE(r.ok());
        CHECK(text.find("steps.mcsound") != std::string::npos);
        CHECK(text.find(c[1]) != std::string::npos);
    }

    // A set that names another set.
    const std::string root = project("sound_set_nested", std::string(R"({ "format": "mce-sound", "version": 1, "guid": ")") + kSteps +
                                                             R"(", "clips": [)" + a + "] }");
    write_file(root + "/assets/sfx/outer.mcsound", std::string(R"({ "format": "mce-sound", "version": 1, "guid": "50d10000-0000-4000-8000-0000000000a5", "clips": ["guid:)") +
                                                       kSteps + "\"] }");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    const std::string text = report(r);
    INFO(text);
    CHECK_FALSE(r.ok());
    CHECK(text.find("another sound set") != std::string::npos);
}
