// M-MONETIZE step 4: the platform ad provider. Off a phone mce_platform's ads are "none", so loads end
// in NoFill and shows in Failed; answers map back to placements by request, since in test mode every
// placement of a format shares Google's test unit; real units come only from the catalog.

#include <doctest/doctest.h>

#include "mce/convar.h"
#include "mce/monetize/platform_ads.h"
#include "mce/platform/ads.h"

using namespace mce;
using namespace mce::monetize;

namespace {

Catalog catalog() {
    Catalog c;
    AdPlacementInfo a;
    a.id = "continue";
    a.format = AdFormat::Rewarded;
    a.android_unit = "ca-app-pub-1111111111111111/2222222222";
    a.ios_unit = "ca-app-pub-1111111111111111/3333333333";
    CHECK(c.add_ad(a));
    a = AdPlacementInfo{};
    a.id = "bonus";  // no units: always the test unit
    a.format = AdFormat::Rewarded;
    CHECK(c.add_ad(a));
    a = AdPlacementInfo{};
    a.id = "menu";
    a.format = AdFormat::Banner;
    CHECK(c.add_ad(a));
    return c;
}

struct Collect final : Sink {
    Array<AdAnswer> ads;
    void post(const StoreAnswer&) override {}
    void post(const AdAnswer& a) override { ads.push_back(a); }
};

platform::ads::Answer answer(platform::ads::AnswerKind k, u32 request, StringView unit, u32 amount = 0) {
    platform::ads::Answer a;
    a.kind = k;
    a.request = request;
    a.unit = unit;
    a.amount = amount;
    return a;
}

}  // namespace

TEST_CASE("platform ads: off a phone there is no network and every request still gets its answer") {
    CHECK(platform::ads::backend() == platform::ads::Backend::None);
    CHECK_FALSE(platform::ads::privacy_options_required());
    const Catalog c = catalog();
    Collect sink;
    PlatformAds ads;
    CHECK_FALSE(ads.available());
    CHECK(StringView(ads.name()) == "none");
    ads.start(c, sink);
    ads.load(0, 1);
    ads.show(1, 2);
    ads.show_banner(2, 3, BannerEdge::Bottom, SafeInsets{0, 0, 0, 48});
    ads.hide_banner();
    CHECK_FALSE(ads.privacy_options_required());
    REQUIRE(sink.ads.size() == 3);
    CHECK(sink.ads[0].kind == AdEventKind::NoFill);
    CHECK(sink.ads[0].placement == 0);
    CHECK(sink.ads[1].kind == AdEventKind::Failed);
    CHECK(sink.ads[1].placement == 1);
    CHECK(sink.ads[2].kind == AdEventKind::NoFill);
    CHECK(sink.ads[2].placement == 2);
    CHECK(sink.ads[2].request == 3);
    ads.stop();
}

TEST_CASE("platform ads: test units in debug builds, the catalog's units in release, test units without one") {
    const Catalog c = catalog();
    Collect sink;
    PlatformAds ads;
    REQUIRE(console::execute("monetize.ads.test auto"));
    ads.start(c, sink);
#if defined(NDEBUG)
    CHECK_FALSE(ads.test_units());
#else
    CHECK(ads.test_units());
#endif
    ads.set_test_units(false);
#if defined(__APPLE__)
    CHECK(ads.unit(0) == "ca-app-pub-1111111111111111/3333333333");
#else
    CHECK(ads.unit(0) == "ca-app-pub-1111111111111111/2222222222");
#endif
    CHECK(ads.unit(1) == platform::ads::test_unit(platform::ads::Format::Rewarded));
    CHECK(ads.unit(2) == platform::ads::test_unit(platform::ads::Format::Banner));
    CHECK(ads.unit(9).empty());
    ads.set_test_units(true);
    CHECK(ads.unit(0) == platform::ads::test_unit(platform::ads::Format::Rewarded));
    ads.stop();

    REQUIRE(console::execute("monetize.ads.test on"));
    ads.start(c, sink);
    CHECK(ads.test_units());
    ads.stop();
    REQUIRE(console::execute("monetize.ads.test off"));
    ads.start(c, sink);
    CHECK_FALSE(ads.test_units());
    ads.stop();
    REQUIRE(console::execute("monetize.ads.test auto"));
    // Google's demo units are its published ones.
    CHECK(platform::ads::test_unit(platform::ads::Format::Interstitial).substr(0, 28) == "ca-app-pub-3940256099942544/");
}

TEST_CASE("platform ads: answers map to the placement that asked, even when units are shared") {
    const Catalog c = catalog();
    Collect sink;
    PlatformAds ads;
    ads.start(c, sink);
    ads.set_test_units(true);
    ads.load(0, 10);  // both rewarded placements ask with the one test unit
    ads.load(1, 11);
    sink.ads.clear();  // the refusals made off a phone
    using K = platform::ads::AnswerKind;
    const StringView unit = platform::ads::test_unit(platform::ads::Format::Rewarded);
    ads.answer(answer(K::Loaded, 11, unit));
    ads.answer(answer(K::Loaded, 10, unit));
    ads.answer(answer(K::Rewarded, 10, unit, 5));
    ads.answer(answer(K::Loaded, 99, unit));  // never asked: ignored
    REQUIRE(sink.ads.size() == 3);
    CHECK(sink.ads[0].placement == 1);
    CHECK(sink.ads[0].request == 11);
    CHECK(sink.ads[1].placement == 0);
    CHECK(sink.ads[2].kind == AdEventKind::Rewarded);
    CHECK(sink.ads[2].placement == 0);
    CHECK(sink.ads[2].reward_amount == 5);
    ads.stop();
}
