// M-HARDEN2 4: small pure helpers the editor leans on that no test named: the prefab override field masks and the
// entity-reference walk of the scene document, the perf monitor's stage names and values, and the 2D editor camera's
// drag-pan and render camera.

#include <doctest/doctest.h>

#include <cstring>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/editor/perf_monitor.h"
#include "mce/editor/scene_document.h"
#include "mce/editor/viewport2d.h"

using namespace mce;
using namespace mce::editor;

TEST_CASE("field masks: all, changed and copied fields follow the type's top-level fields") {
    const refl::TypeRecord& type = refl::type_of<LocalTransform>();
    REQUIRE(type.fields.size() == 3);  // position, rotation, scale
    CHECK(all_fields_mask(type) == 0b111);

    LocalTransform a, b;
    CHECK(changed_fields_mask(type, &a, &b) == 0);
    b.position.x = 5.0f;
    CHECK(changed_fields_mask(type, &a, &b) == 0b001);
    b.scale.z = 2.0f;
    CHECK(changed_fields_mask(type, &a, &b) == 0b101);

    // Copying only the position field leaves the scale alone.
    LocalTransform dst;
    copy_fields(type, 0b001, &b, &dst);
    CHECK(dst.position.x == 5.0f);
    CHECK(dst.scale.z == 1.0f);
    copy_fields(type, all_fields_mask(type), &b, &dst);
    CHECK(changed_fields_mask(type, &dst, &b) == 0);
    copy_fields(type, 0, &a, &dst);  // an empty mask copies nothing
    CHECK(dst.position.x == 5.0f);
}

TEST_CASE("field masks: entity-reference fields are found and visited, plain components have none") {
    CHECK(entity_ref_fields_mask(refl::type_of<LocalTransform>()) == 0);

    const refl::TypeRecord& child = refl::type_of<ChildOf>();
    CHECK(entity_ref_fields_mask(child) == 0b1);

    ChildOf value;
    value.parent = Entity{};
    u32 seen = 0;
    visit_entity_refs(
        child, &value,
        [](Entity* field, void* user) {
            ++*static_cast<u32*>(user);
            *field = Entity{};  // a visitor may rewrite the reference
        },
        &seen);
    CHECK(seen == 1);

    LocalTransform plain;
    seen = 0;
    visit_entity_refs(
        refl::type_of<LocalTransform>(), &plain, [](Entity*, void* user) { ++*static_cast<u32*>(user); }, &seen);
    CHECK(seen == 0);
}

TEST_CASE("perf monitor: every stage has a name and reads its own field of a sample") {
    FrameSample s;
    s.frame_ms = 1.0f;
    s.sim_ms = 2.0f;
    s.ui_ms = 3.0f;
    s.record_ms = 4.0f;
    s.submit_ms = 5.0f;
    s.gpu_ms = 6.0f;
    const PerfStage stages[] = {PerfStage::Frame,  PerfStage::Sim,    PerfStage::Ui,
                                PerfStage::Record, PerfStage::Submit, PerfStage::Gpu};
    f32 expect = 1.0f;
    for (const PerfStage st : stages) {
        CHECK(perf_stage_value(s, st) == expect);
        CHECK(std::strlen(perf_stage_name(st)) > 0);
        CHECK(std::strcmp(perf_stage_name(st), "?") != 0);
        expect += 1.0f;
    }
    // Count is not a stage.
    CHECK(std::strcmp(perf_stage_name(PerfStage::Count), "?") == 0);
    CHECK(perf_stage_value(s, PerfStage::Count) < 0.0f);
    // A device without timestamp queries leaves the GPU stage unmeasured.
    CHECK(perf_stage_value(FrameSample{}, PerfStage::Gpu) < 0.0f);
}

TEST_CASE("EditorCamera2D: drag-pan moves the world with the cursor; the render camera follows without pixel snap") {
    EditorCamera2D cam;
    cam.center = {1.0f, 2.0f};
    cam.zoom = 2.0f;
    const rhi::Extent2D size{800, 600};
    const math::vec2 under = cam.screen_to_world({100.0f, 100.0f}, size);

    cam.pan_pixels({48.0f, -24.0f});  // ppm = 96: half a metre right, a quarter metre down on screen
    CHECK(cam.center.x == doctest::Approx(1.0f - 0.5f));
    CHECK(cam.center.y == doctest::Approx(2.0f - 0.25f));
    // The world point that was under the cursor is now under the cursor shifted by the drag.
    const math::vec2 now = cam.screen_to_world({148.0f, 76.0f}, size);
    CHECK(now.x == doctest::Approx(under.x));
    CHECK(now.y == doctest::Approx(under.y));

    const render::Camera2D rc = cam.to_render();
    CHECK(rc.position.x == cam.center.x);
    CHECK(rc.position.y == cam.center.y);
    CHECK(rc.zoom == cam.zoom);
    CHECK_FALSE(rc.pixel_snap);
}
