#include <doctest/doctest.h>

#include <cstring>

#include "mce/rhi/rhi.h"
#include "test_support.h"

using namespace mce;
using namespace mce::rhi;

TEST_CASE("rhi: format helpers") {
    CHECK(is_depth_format(TextureFormat::D32_Float));
    CHECK_FALSE(is_depth_format(TextureFormat::RGBA8_UNorm));
    CHECK(is_compressed_format(TextureFormat::BC7_SRGB));
    CHECK(bytes_per_pixel(TextureFormat::RGBA8_UNorm) == 4);
    CHECK(bytes_per_pixel(TextureFormat::RGBA16F) == 8);
    CHECK(bytes_per_pixel(TextureFormat::BC1_UNorm) == 0);
    CHECK(vertex_format_size(VertexFormat::Float3) == 12);
    CHECK(vertex_format_size(VertexFormat::UNorm8x4) == 4);
}

TEST_CASE("rhi: command buffer records and reads back in order") {
    CommandBuffer cb;
    CHECK(cb.empty());

    PassAttachment color;
    color.clear.color[0] = 0.25f;
    RenderPassDesc pass;
    pass.colors = Span<const PassAttachment>(&color, 1);
    pass.debug_name = "main";

    const PipelineHandle pipe{3, 7};
    const f32 uniforms[16] = {1, 2, 3, 4};
    const u16 indices[3] = {0, 1, 2};
    const u8 verts[36] = {9};

    cb.begin_pass(pass);
    cb.push_debug_scope("tri");
    cb.bind_pipeline(pipe);
    cb.set_uniforms(bank::kFrame, as_bytes(uniforms));
    cb.set_vertex_data(as_bytes(Span<const u8>(verts)));
    cb.set_index_data(as_bytes(Span<const u16>(indices)), IndexFormat::U16);
    cb.draw(3, 1, 0, 0);
    cb.pop_debug_scope();
    cb.end_pass();
    CHECK(cb.command_count() == 9);
    CHECK_FALSE(cb.in_pass());

    const CmdType expected[] = {CmdType::BeginPass, CmdType::PushDebugScope, CmdType::BindPipeline,
                                CmdType::SetUniforms, CmdType::SetVertexData, CmdType::SetIndexData,
                                CmdType::Draw,        CmdType::PopDebugScope, CmdType::EndPass};
    u32 i = 0;
    const CmdHeader* h = nullptr;
    const void* payload = nullptr;
    for (auto r = cb.reader(); r.next(h, payload); ++i) {
        REQUIRE(i < 9);
        CHECK(h->type == expected[i]);
        CHECK(h->size % 16 == 0);
        switch (h->type) {
            case CmdType::BeginPass: {
                const auto* p = static_cast<const cmd::BeginPass*>(payload);
                CHECK(p->color_count == 1);
                CHECK_FALSE(p->has_depth);
                CHECK(p->colors[0].texture.is_null());
                CHECK(p->colors[0].clear.color[0] == 0.25f);
                CHECK(StringView(p->debug_name) == "main");
                break;
            }
            case CmdType::BindPipeline:
                CHECK(static_cast<const cmd::BindPipeline*>(payload)->pipeline == pipe);
                break;
            case CmdType::SetUniforms: {
                const auto* p = static_cast<const cmd::SetUniforms*>(payload);
                CHECK(p->bank == bank::kFrame);
                REQUIRE(p->size == sizeof(uniforms));
                CHECK(std::memcmp(CommandBuffer::inline_data(p, p->size).data(), uniforms, sizeof(uniforms)) == 0);  // NOLINT(bugprone-suspicious-memory-comparison): checks a bit-exact copy
                break;
            }
            case CmdType::SetIndexData: {
                const auto* p = static_cast<const cmd::SetIndexData*>(payload);
                CHECK(p->format == IndexFormat::U16);
                REQUIRE(p->size == 6);
                CHECK(std::memcmp(CommandBuffer::inline_data(p, p->size).data(), indices, 6) == 0);
                break;
            }
            case CmdType::Draw: {
                const auto* p = static_cast<const cmd::Draw*>(payload);
                CHECK(p->index_count == 3);
                CHECK(p->instance_count == 1);
                break;
            }
            default: break;
        }
    }
    CHECK(i == 9);

    cb.reset();
    CHECK(cb.empty());
    CHECK(cb.command_count() == 0);
}

TEST_CASE("rhi: command buffer on the frame arena") {
    mem::LinearArena arena(mem::heap(), 256 * KiB);
    CommandBuffer cb(arena);
    PassAttachment color;
    RenderPassDesc pass;
    pass.colors = Span<const PassAttachment>(&color, 1);
    for (int n = 0; n < 100; ++n) {
        cb.begin_pass(pass);
        cb.draw_non_indexed(3);
        cb.end_pass();
    }
    CHECK(cb.command_count() == 300);
    CHECK(arena.used() > 0);
}

TEST_CASE("rhi: misuse asserts") {
    CommandBuffer cb;
    {
        test::ExpectAssert expect;
        cb.end_pass();
    }
    {
        test::ExpectAssert expect;
        cb.draw(3);
    }
    {
        test::ExpectAssert expect;
        cb.set_uniforms(7, {});
    }
}

TEST_CASE("rhi: draw args and indirect draws round-trip") {
    CommandBuffer cb;
    PassAttachment color;
    RenderPassDesc pass;
    pass.colors = Span<const PassAttachment>(&color, 1);
    const DrawIndexedIndirectCommand cmds[3] = {{6, 1, 0, 0, 0}, {12, 1, 6, 4, 0}, {6, 2, 0, 16, 1}};
    cb.begin_pass(pass);
    cb.set_draw_args(Span<const DrawIndexedIndirectCommand>(cmds));
    cb.draw_indirect(2, 1);
    cb.draw_indirect_buffer(BufferHandle{}, 40, 3);
    cb.end_pass();

    const CmdHeader* h = nullptr;
    const void* p = nullptr;
    auto r = cb.reader();
    REQUIRE(r.next(h, p));
    CHECK(h->type == CmdType::BeginPass);
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::SetDrawArgs);
    const auto* args = static_cast<const cmd::SetDrawArgs*>(p);
    REQUIRE(args->count == 3);
    const Span<const byte> data =
        CommandBuffer::inline_data(args, static_cast<u32>(args->count * sizeof(DrawIndexedIndirectCommand)));
    const auto* copied = reinterpret_cast<const DrawIndexedIndirectCommand*>(data.data());
    CHECK(copied[1].index_count == 12);
    CHECK(copied[1].first_index == 6);
    CHECK(copied[1].base_vertex == 4);
    CHECK(copied[2].instance_count == 2);
    CHECK(copied[2].base_instance == 1);
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::DrawIndirect);
    CHECK(static_cast<const cmd::DrawIndirect*>(p)->first_command == 1);
    CHECK(static_cast<const cmd::DrawIndirect*>(p)->draw_count == 2);
    CHECK(std::string_view(cmd_type_name(CmdType::DrawIndirect)) == "DrawIndirect");
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::DrawIndirectBuffer);
    CHECK(static_cast<const cmd::DrawIndirectBuffer*>(p)->offset == 40);
    CHECK(static_cast<const cmd::DrawIndirectBuffer*>(p)->draw_count == 3);
    CHECK(std::string_view(cmd_type_name(CmdType::DrawIndirectBuffer)) == "DrawIndirectBuffer");
}

TEST_CASE("rhi: resolve records outside a pass; the uniform bank range covers kObject") {
    CommandBuffer cb;
    const TextureHandle src{3, 1}, dst{4, 2};
    cb.resolve(src, dst);
    const CmdHeader* h = nullptr;
    const void* p = nullptr;
    auto r = cb.reader();
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::Resolve);
    CHECK(static_cast<const cmd::Resolve*>(p)->src == src);
    CHECK(static_cast<const cmd::Resolve*>(p)->dst == dst);
    CHECK(std::string_view(cmd_type_name(CmdType::Resolve)) == "Resolve");
    CHECK(vertex_format_size(VertexFormat::SNorm16x4) == 8);
    CHECK(vertex_format_size(VertexFormat::SNorm16x2) == 4);
    CHECK(vertex_format_size(VertexFormat::UNorm16x2) == 4);

    PassAttachment color;
    RenderPassDesc pass;
    pass.colors = Span<const PassAttachment>(&color, 1);
    cb.begin_pass(pass);
    const byte object[16] = {};
    cb.set_uniforms(bank::kObject, Span<const byte>(object));
    {
        test::ExpectAssert expect;
        cb.resolve(src, dst);  // inside a pass
    }
    cb.end_pass();
}

TEST_CASE("rhi: storage data and timestamps round-trip; layered attachments default to layer 0") {
    CommandBuffer cb;
    cb.write_timestamp(0);
    PassAttachment depth;
    CHECK(depth.layer == 0);
    depth.layer = 2;
    RenderPassDesc pass;
    pass.depth = depth;
    cb.begin_pass(pass);
    const u32 rows[4] = {1, 2, 3, 4};
    cb.set_storage_data(bank::kClusters, as_bytes(Span<const u32>(rows)));
    cb.end_pass();
    cb.write_timestamp(5);

    const CmdHeader* h = nullptr;
    const void* p = nullptr;
    auto r = cb.reader();
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::WriteTimestamp);
    CHECK(static_cast<const cmd::WriteTimestamp*>(p)->index == 0);
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::BeginPass);
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::SetStorageData);
    const auto* sd = static_cast<const cmd::SetStorageData*>(p);
    CHECK(sd->binding == bank::kClusters);
    REQUIRE(sd->size == sizeof(rows));
    CHECK(std::memcmp(CommandBuffer::inline_data(sd, sd->size).data(), rows, sizeof(rows)) == 0);  // NOLINT(bugprone-suspicious-memory-comparison): checks a bit-exact copy
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::EndPass);
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::WriteTimestamp);
    CHECK(static_cast<const cmd::WriteTimestamp*>(p)->index == 5);
    CHECK_FALSE(r.next(h, p));
    CHECK(std::string_view(cmd_type_name(CmdType::SetStorageData)) == "SetStorageData");
    CHECK(std::string_view(cmd_type_name(CmdType::WriteTimestamp)) == "WriteTimestamp");
    {
        test::ExpectAssert expect;
        cb.write_timestamp(kMaxTimestamps);
    }
}

TEST_CASE("rhi: compute commands round-trip outside passes") {
    CommandBuffer cb;
    const BufferHandle args{3, 1};
    const TextureHandle img{2, 1};
    cb.bind_storage_image(1, img, 2, StorageAccess::Write);
    cb.dispatch(8, 4);
    cb.barrier(barrier::kIndirect | barrier::kStorageBuffer);
    cb.dispatch_indirect(args, 12);
    cb.barrier(0);  // nothing to wait for: not recorded
    cb.barrier();

    const CmdHeader* h = nullptr;
    const void* p = nullptr;
    auto r = cb.reader();
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::BindStorageImage);
    const auto* si = static_cast<const cmd::BindStorageImage*>(p);
    CHECK(si->unit == 1);
    CHECK(si->texture == img);
    CHECK(si->mip == 2);
    CHECK(si->access == StorageAccess::Write);
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::Dispatch);
    const auto* d = static_cast<const cmd::Dispatch*>(p);
    CHECK(d->groups_x == 8);
    CHECK(d->groups_y == 4);
    CHECK(d->groups_z == 1);
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::Barrier);
    CHECK(static_cast<const cmd::Barrier*>(p)->bits == (barrier::kIndirect | barrier::kStorageBuffer));
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::DispatchIndirect);
    CHECK(static_cast<const cmd::DispatchIndirect*>(p)->buffer == args);
    CHECK(static_cast<const cmd::DispatchIndirect*>(p)->offset == 12);
    REQUIRE(r.next(h, p));
    REQUIRE(h->type == CmdType::Barrier);
    CHECK(static_cast<const cmd::Barrier*>(p)->bits == barrier::kAll);
    CHECK_FALSE(r.next(h, p));
    CHECK(cb.command_count() == 5);
    CHECK(std::string_view(cmd_type_name(CmdType::DispatchIndirect)) == "DispatchIndirect");

    PassAttachment color;
    RenderPassDesc pass;
    pass.colors = Span<const PassAttachment>(&color, 1);
    cb.begin_pass(pass);
    {
        test::ExpectAssert expect;
        cb.dispatch(1);
    }
    {
        test::ExpectAssert expect;
        cb.barrier();
    }
    cb.end_pass();
    {
        test::ExpectAssert expect;
        cb.dispatch_indirect(args, 2);  // not 4-aligned
    }
}
