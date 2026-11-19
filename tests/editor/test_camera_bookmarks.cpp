// M-CAMBOOKMARK 1: nine saved viewpoints, both cameras per slot, saved as text.

#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <string>

#include "mce/editor/camera_bookmarks.h"

using namespace mce;
using namespace mce::editor;

namespace {

std::string text_of(const cook::String& s) { return std::string(s.view().data(), s.view().size()); }

}  // namespace

TEST_CASE("CameraBookmarks save and recall restore both cameras") {
    CameraBookmarks marks;
    EditorCamera2D cam2d;
    cam2d.center = {3.0f, -4.5f};
    cam2d.zoom = 2.5f;
    OrbitCamera orbit;
    orbit.target = {1.0f, 2.0f, 3.0f};
    orbit.yaw = 1.25f;
    orbit.pitch = 0.5f;
    orbit.distance = 20.0f;
    marks.save(2, cam2d, orbit);
    CHECK(marks.has(2));
    CHECK_FALSE(marks.has(1));
    CHECK(marks.count() == 1);

    EditorCamera2D other2d;
    OrbitCamera other;
    CHECK_FALSE(marks.recall(1, other2d, other));  // empty slot: untouched
    CHECK(other2d.zoom == 1.0f);
    CHECK(marks.recall(2, other2d, other));
    CHECK(other2d.center.x == 3.0f);
    CHECK(other2d.center.y == -4.5f);
    CHECK(other2d.zoom == 2.5f);
    CHECK(other.target.z == 3.0f);
    CHECK(other.yaw == 1.25f);
    CHECK(other.pitch == 0.5f);
    CHECK(other.distance == 20.0f);
    CHECK(other.effective == 20.0f);

    marks.save(CameraBookmarks::kSlots, cam2d, orbit);  // out of range: ignored
    CHECK(marks.count() == 1);
    marks.clear(2);
    CHECK(marks.count() == 0);
}

TEST_CASE("CameraBookmarks serialize and parse round-trip; parse skips bad lines") {
    CameraBookmarks marks;
    EditorCamera2D cam2d;
    cam2d.center = {0.1f, 0.2f};
    cam2d.zoom = 0.75f;
    OrbitCamera orbit;
    orbit.target = {-1.5f, 0.25f, 9.0f};
    marks.save(0, cam2d, orbit);
    marks.save(8, cam2d, orbit);
    const cook::String text = marks.serialize();

    CameraBookmarks back;
    back.parse(text.view());
    CHECK(back.count() == 2);
    CHECK(back.has(0));
    CHECK(back.has(8));
    CHECK(text_of(back.serialize()) == text_of(text));

    CameraBookmarks messy;
    messy.parse(
        "0 1 2 3 4 5 6 7 8 9\n10 1 2 3 4 5 6 7 8 9\nx\n3 1 2 3 4 5 6 7 8\n4 1 2 3 4 5 6 7 8 9 10\n5 nan 2 3 4 5 6 7 8 "
        "9\n"
        "6 1 2 3 4 5 6 7 8 9\r\n");
    CHECK(messy.count() == 1);
    CHECK(messy.has(5));
}

TEST_CASE("CameraBookmarks fuzz: a round trip keeps every slot exactly") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        const auto val = [&] {
            const f32 mag = static_cast<f32>(rng() % 2000000) / 997.0f - 1000.0f;
            return rng() % 8 == 0 ? mag * 1e30f : mag;  // some very large and some tiny-fraction values
        };
        CameraBookmarks a;
        for (u32 slot = 0; slot < CameraBookmarks::kSlots; ++slot) {
            if (rng() % 2 == 0) continue;
            EditorCamera2D c2;
            c2.center = {val(), val()};
            c2.zoom = val();
            OrbitCamera o;
            o.target = {val(), val(), val()};
            o.yaw = val();
            o.pitch = val();
            o.distance = val();
            a.save(slot, c2, o);
        }
        CameraBookmarks b;
        b.parse(a.serialize().view());
        CHECK(b.count() == a.count());
        for (u32 slot = 0; slot < CameraBookmarks::kSlots; ++slot) {
            CHECK(b.has(slot) == a.has(slot));
            EditorCamera2D x2, y2;
            OrbitCamera xo, yo;
            if (!a.recall(slot, x2, xo) || !b.recall(slot, y2, yo)) continue;
            CHECK(x2.center.x == y2.center.x);
            CHECK(x2.zoom == y2.zoom);
            CHECK(xo.target.z == yo.target.z);
            CHECK(xo.distance == yo.distance);
        }
    }
}

// Found by the fuzz below: 1e300 is finite as a double but became infinity in the float slot.
TEST_CASE("CameraBookmarks parse skips a line whose number overflows a float") {
    CameraBookmarks b;
    b.parse("1 1e300 0 1 0 0 0 0 0 12\n2 0 0 1 0 0 0 0 0 12\n");
    CHECK(!b.has(0));
    CHECK(b.has(1));
}

TEST_CASE("CameraBookmarks fuzz: parse never stores a non-finite number, whatever the text") {
    static const char* const pieces[] = {"1",   "9",   "0",        "10",  "-3", "2.5", "1e300", "-1e300", "1e39",
                                         "nan", "inf", "0x1p+200", "abc", "",   " ",   "\r",    "1e-60",  "3.4e38"};
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        std::string text;
        for (u32 line = 0, n = static_cast<u32>(rng() % 12); line < n; ++line) {
            const u32 fields = rng() % 3 == 0 ? static_cast<u32>(rng() % 14) : 10u;
            for (u32 f = 0; f < fields; ++f) {
                text += pieces[rng() % (sizeof(pieces) / sizeof(pieces[0]))];
                text += ' ';
            }
            text += '\n';
        }
        CameraBookmarks b;
        b.parse(StringView(text.data(), text.size()));
        for (u32 slot = 0; slot < CameraBookmarks::kSlots; ++slot) {
            EditorCamera2D c2;
            OrbitCamera o;
            if (!b.recall(slot, c2, o)) continue;
            CHECK(std::isfinite(c2.center.x));
            CHECK(std::isfinite(c2.center.y));
            CHECK(std::isfinite(c2.zoom));
            CHECK(std::isfinite(o.target.x));
            CHECK(std::isfinite(o.target.y));
            CHECK(std::isfinite(o.target.z));
            CHECK(std::isfinite(o.yaw));
            CHECK(std::isfinite(o.pitch));
            CHECK(std::isfinite(o.distance));
        }
    }
}
