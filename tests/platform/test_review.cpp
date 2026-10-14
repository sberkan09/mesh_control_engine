// M-MOBILE step 11c: the review prompt. Off a phone there is no review backend, so request_review() is a
// logged no-op that still counts the request (the fake the cloud checks; the prompt itself is checked in
// the iOS Simulator and an Android emulator).

#include <doctest/doctest.h>

#include "mce/fixed_string.h"
#include "mce/log.h"
#include "mce/platform/review.h"

using namespace mce;

TEST_CASE("review: off a phone request_review() is a logged no-op") {
    CHECK(platform::review_backend() == platform::ReviewBackend::None);
    CHECK(StringView(platform::review_backend_name(platform::review_backend())) == "none");
    CHECK(StringView(platform::review_backend_name(platform::ReviewBackend::StoreKit)) == "storekit");
    CHECK(StringView(platform::review_backend_name(platform::ReviewBackend::PlayInAppReview)) == "play");

    log::set_level(log::Level::Info);  // the test runner logs warnings and up
    const u64 seq = log::console_last_seq();
    const u32 before = platform::review_requests();
    CHECK_FALSE(platform::request_review());
    CHECK_FALSE(platform::request_review());
    CHECK(platform::review_requests() == before + 2);

    log::ConsoleLine lines[8];
    const usize n = log::read_console(seq, Span<log::ConsoleLine>(lines, 8));
    usize logged = 0;
    for (usize i = 0; i < n; ++i)
        if (StringView(lines[i].text.c_str()).find("request_review()") != StringView::npos) ++logged;
    CHECK(logged == 2);
    log::set_level(log::Level::Warn);
}
