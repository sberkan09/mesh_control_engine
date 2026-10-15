// M-ACCESS step 4: the screen reader bridge. Off a phone and without MCE_SCREEN_READER_NATIVE there
// is no device reader, so announce() reaches only the sink; the device bridges (TalkBack's JNI
// calls: test_android_bridges.py) are checked where they can be.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "mce/platform/screen_reader.h"

using namespace mce;
namespace sr = mce::platform::screen_reader;

TEST_CASE("M-ACCESS: screen reader announcements reach the sink; the desktop has no device reader") {
    CHECK(sr::backend() == sr::Backend::None);
    CHECK(StringView(sr::backend_name(sr::backend())) == "none");
    CHECK(StringView(sr::backend_name(sr::Backend::Windows)) == "windows");
    CHECK_FALSE(sr::running());

    std::vector<std::string> heard;
    sr::set_sink([](void* user, StringView text) { static_cast<std::vector<std::string>*>(user)->emplace_back(text); },
                 &heard);
    const sr::Stats before = sr::stats();
    CHECK_FALSE(sr::announce("Music, slider, 40%"));  // nothing speaks it here
    CHECK_FALSE(sr::announce(""));                     // nothing to say: not even the sink
    REQUIRE(heard.size() == 1);
    CHECK(heard[0] == "Music, slider, 40%");
    CHECK(sr::stats().announced == before.announced + 1);
    CHECK(sr::stats().spoken == before.spoken);
    sr::set_sink(nullptr, nullptr);
}
