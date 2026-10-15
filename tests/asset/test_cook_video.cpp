// M-CINEMA 5 (ADR-0042): an .mpg cooks to CHNK_VID (asset::VideoView): the stream as imported plus
// its sound as 48 kHz PCM; "audio": false drops the sound; a file that is not MPEG-1 is an error
// naming it.

#include <doctest/doctest.h>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace cook_test;

namespace {

constexpr const char* kFilm = "7e7a0000-0000-4000-8000-0000000000b1";
constexpr const char* kSilent = "7e7a0000-0000-4000-8000-0000000000b2";

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

}  // namespace

TEST_CASE("video: .mpg cooks to CHNK_VID with its sound at 48 kHz") {
    const std::string root = fresh_dir("video_cook");
    const std::string clip = read_file(std::string(MCE_TEST_DATA_DIR) + "/video/red_blue.mpg");
    REQUIRE(!clip.empty());
    write_file(root + "/assets/film/intro.mpg", clip);
    write_file(root + "/assets/film/intro.mpg.mceasset", sidecar(kFilm, "video"));
    write_file(root + "/assets/film/quiet.mpg", clip);
    write_file(root + "/assets/film/quiet.mpg.mceasset", sidecar(kSilent, "video", R"({ "audio": false })"));
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    INFO(report(r));
    REQUIRE(r.ok());

    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    const asset::AssetKey key = assets.find(asset::parse_guid(kFilm).value());
    REQUIRE(key != asset::kInvalidAssetKey);
    CHECK(assets.info(key).type == asset::ChunkType::Video);
    auto h = assets.load_sync<asset::Video>(key);
    REQUIRE(h);
    auto v = asset::VideoView::read(assets.resolve(h.value()));
    REQUIRE(v);
    CHECK(v.value().header->width == 64);
    CHECK(v.value().header->height == 48);
    CHECK(v.value().header->frame_rate == doctest::Approx(25.0f));
    CHECK(v.value().header->duration == doctest::Approx(2.0f).epsilon(0.05));
    CHECK(v.value().mpeg.size() == clip.size());
    CHECK(v.value().header->audio_channels == 2);
    CHECK(static_cast<f64>(v.value().header->audio_frames) == doctest::Approx(96000.0).epsilon(0.03));
    CHECK(v.value().audio.size() == static_cast<usize>(v.value().header->audio_frames) * 2);

    const asset::AssetKey quiet = assets.find(asset::parse_guid(kSilent).value());
    auto hq = assets.load_sync<asset::Video>(quiet);
    REQUIRE(hq);
    auto vq = asset::VideoView::read(assets.resolve(hq.value()));
    REQUIRE(vq);
    CHECK(vq.value().header->audio_channels == 0);
    CHECK(vq.value().audio.empty());
    assets.release(asset::AnyHandle(h.value()));
    assets.release(asset::AnyHandle(hq.value()));
}

TEST_CASE("video: a file that is not MPEG-1 fails the cook naming it") {
    const std::string root = fresh_dir("video_cook_bad");
    write_file(root + "/assets/film/broken.mpg", std::string(2048, 'x'));
    write_file(root + "/assets/film/broken.mpg.mceasset", sidecar(kFilm, "video"));
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    CHECK_FALSE(r.ok());
    const std::string text = report(r);
    CHECK(text.find("broken.mpg") != std::string::npos);
    CHECK(text.find("MPEG-1") != std::string::npos);
}
