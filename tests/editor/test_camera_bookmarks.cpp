// M-CAMBOOKMARK 1: nine saved viewpoints, both cameras per slot, saved as text.

#include <doctest/doctest.h>

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
