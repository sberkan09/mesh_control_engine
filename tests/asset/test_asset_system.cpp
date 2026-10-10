// Runtime AssetSystem: mount (dev dir + pack), sync load, refcount, deferred unload after
// MCE_FRAMES_IN_FLIGHT frames, generations, placeholders, hot-reload commit, upload sink calls.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "mce/asset/asset_system.h"
#include "mce/asset/xxhash.h"
#include "test_support.h"

using namespace mce;
using namespace mce::asset;
namespace fs = std::filesystem;

namespace {

const Guid kTex{0x0102030405060708ull, 0x1112131415161718ull};
const Guid kSfx{0x2102030405060708ull, 0x2112131415161718ull};

std::string tmp_dir(const char* name) {
    fs::path p = fs::path(MCE_TEST_TMP_DIR) / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p.generic_string();
}

void write_bytes(const std::string& path, Span<const byte> b) {
    fs::create_directories(fs::path(path).parent_path());
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f);
    std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
}

void texture_container(u8 shade, Array<byte>& out) {
    u8 px[2 * 2 * 4];
    std::memset(px, shade, sizeof(px));
    const TextureMipData mip{2, 2, Span<const byte>(reinterpret_cast<const byte*>(px), sizeof(px))};
    Array<byte> payload;
    write_texture(TextureFormat::RGBA8, 1, 0, Span<const TextureMipData>(&mip, 1), payload);
    ContainerWriter w;
    ContainerWriter::ChunkDesc d;
    d.type = ChunkType::Texture;
    d.version = chunk_version(TEXTURE_VERSION, 0);
    d.guid = kTex;
    w.add(d, payload.span());
    w.finish(out);
}

void sfx_container(Array<byte>& out) {
    const f32 s[4] = {0, 0.5f, -0.5f, 0};
    SfxHeader h{};
    h.sample_rate = 48000;
    h.channels = 1;
    h.frame_count = 4;
    Array<byte> payload;
    write_sfx(h, Span<const f32>(s, 4), payload);
    ContainerWriter w;
    ContainerWriter::ChunkDesc d;
    d.type = ChunkType::Sfx;
    d.version = chunk_version(SFX_VERSION, 0);
    d.guid = kSfx;
    w.add(d, payload.span());
    w.finish(out);
}

std::string cooked_path(const std::string& root, Guid g, u64 hash) {
    char name[96];
    std::snprintf(name, sizeof(name), "/%.2s/%s/%016llx.mce", to_hex(g).c_str(), to_hex(g).c_str(), static_cast<unsigned long long>(hash));
    return root + name;
}

// Writes a dev cooked tree (manifest + content-addressed files) and returns the content hashes.
void make_cooked_tree(const std::string& root, u64& tex_hash, u64& sfx_hash) {
    Array<byte> tex, sfx;
    texture_container(0x40, tex);
    sfx_container(sfx);
    tex_hash = xxh64(tex.data(), tex.size());
    sfx_hash = xxh64(sfx.data(), sfx.size());
    write_bytes(cooked_path(root, kTex, tex_hash), tex.span());
    write_bytes(cooked_path(root, kSfx, sfx_hash), sfx.span());
    ManifestItem items[2] = {{kTex, tex_hash, ChunkType::Texture, "textures/a.png"}, {kSfx, sfx_hash, ChunkType::Sfx, "audio/b.wav"}};
    Array<byte> mp;
    write_manifest(Span<ManifestItem>(items, 2), mp);
    ContainerWriter w;
    ContainerWriter::ChunkDesc d;
    d.type = ChunkType::Manifest;
    w.add(d, mp.span());
    Array<byte> manifest;
    w.finish(manifest);
    write_bytes(root + "/manifest.mce", manifest.span());
}

struct CountingSink final : IGpuUploadSink {
    int textures = 0, meshes = 0, materials = 0, destroyed = 0;
    Result<void> upload_mesh(AssetKey, Span<const byte>) override {
        ++meshes;
        return ok();
    }
    Result<void> upload_texture(AssetKey, Span<const byte>, u32) override {
        ++textures;
        return ok();
    }
    Result<void> update_material(AssetKey, Span<const byte>) override {
        ++materials;
        return ok();
    }
    void destroy_gpu_objects(Span<const RetiredAsset> r) override { destroyed += static_cast<int>(r.size()); }
};

}  // namespace

TEST_CASE("asset system: mount a dev tree, load, refcount, deferred unload") {
    const std::string root = tmp_dir("dev_mount");
    u64 th = 0, sh = 0;
    make_cooked_tree(root, th, sh);

    AssetSystem as;
    CountingSink sink;
    as.set_upload_sink(&sink);
    const StringView roots[] = {root};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    REQUIRE(as.asset_count() == 2);
    const AssetKey tk = as.find(kTex);
    REQUIRE(tk != kInvalidAssetKey);
    CHECK(as.info(tk).path == "textures/a.png");
    CHECK(as.info(tk).type == ChunkType::Texture);

    auto h = as.load_sync<Texture>(kTex);
    REQUIRE(h);
    CHECK(h.value().valid());
    CHECK(sink.textures == 1);
    auto tv = TextureView::read(as.resolve(h.value()));
    REQUIRE(tv);
    CHECK(static_cast<u8>(tv.value().pixels(0)[0]) == 0x40);

    auto h2 = as.load_sync<Texture>(tk);  // already resident: refcount bump, no second upload
    REQUIRE(h2);
    CHECK(h2.value() == h.value());
    CHECK(as.refcount(tk) == 2);
    CHECK(sink.textures == 1);

    // Wrong type for the key is an error, not a reinterpretation.
    CHECK(!as.load_sync<Sfx>(tk));

    as.release(h.value());
    as.release(h2.value());
    CHECK(as.refcount(tk) == 0);
    for (u32 f = 0; f + 1 < MCE_FRAMES_IN_FLIGHT; ++f) {
        as.end_frame();
        CHECK(as.is_alive(h.value()));  // still in flight
    }
    as.end_frame();
    CHECK(!as.is_alive(h.value()));
    CHECK(sink.destroyed == 1);
    bool saw_unloading = false;
    for (const AssetEvent& e : as.events()) saw_unloading = saw_unloading || e.kind == AssetEvent::Kind::Unloading;
    CHECK(saw_unloading);

    // A straggler handle resolves to the magenta placeholder; it never aliases a reload.
    CHECK(as.resolve(h.value()).data() == as.placeholder<Texture>().data());
    auto again = as.load_sync<Texture>(kTex);
    REQUIRE(again);
    CHECK(again.value().generation != h.value().generation);
    CHECK(as.resolve(h.value()).data() == as.placeholder<Texture>().data());
    as.release(again.value());
}

TEST_CASE("asset system: re-acquire between release and reap keeps the asset resident") {
    const std::string root = tmp_dir("dev_resurrect");
    u64 th = 0, sh = 0;
    make_cooked_tree(root, th, sh);
    AssetSystem as;
    const StringView roots[] = {root};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    auto h = as.load_sync<Sfx>(kSfx);
    REQUIRE(h);
    as.release(h.value());
    auto h2 = as.load_sync<Sfx>(kSfx);
    REQUIRE(h2);
    CHECK(h2.value() == h.value());
    for (u32 f = 0; f < MCE_FRAMES_IN_FLIGHT; ++f) as.end_frame();
    CHECK(as.is_alive(h2.value()));
    bool saw_failed = false;
    for (const AssetEvent& e : as.events()) saw_failed = saw_failed || e.kind == AssetEvent::Kind::UnloadFailed;
    CHECK(saw_failed);
    as.release(h2.value());
}

TEST_CASE("asset system: corrupted cooked file fails the load, placeholder serves") {
    const std::string root = tmp_dir("dev_corrupt");
    u64 th = 0, sh = 0;
    make_cooked_tree(root, th, sh);
    {
        std::FILE* f = std::fopen(cooked_path(root, kTex, th).c_str(), "r+b");
        REQUIRE(f);
        std::fseek(f, 120, SEEK_SET);
        std::fputc(0x7F, f);
        std::fclose(f);
    }
    AssetSystem as;
    const StringView roots[] = {root};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    auto h = as.load_sync<Texture>(kTex);
    REQUIRE(!h);
    CHECK(h.error().code == Errc::CrcMismatch);  // content hash no longer matches the name
    CHECK(as.resolve(AssetHandle<Texture>{}).data() == as.placeholder<Texture>().data());
    as.end_frame();
    bool saw = false;
    for (const AssetEvent& e : as.events()) saw = saw || e.kind == AssetEvent::Kind::LoadFailed;
    CHECK(saw);
}

TEST_CASE("asset system: hot-reload commit bumps the generation; AssetRef re-stamps") {
    const std::string root = tmp_dir("dev_reload");
    u64 th = 0, sh = 0;
    make_cooked_tree(root, th, sh);
    AssetSystem as;
    CountingSink sink;
    as.set_upload_sink(&sink);
    const StringView roots[] = {root};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    auto h = as.load_sync<Texture>(kTex);
    REQUIRE(h);
    AssetRef<Texture> ref{kTex, h.value()};

    Array<byte> fresh;
    texture_container(0x99, fresh);
    REQUIRE(as.commit_reload(kTex, fresh.span()));
    CHECK(sink.textures == 2);
    CHECK(!as.is_alive(h.value()));  // raw handle is stale...
    auto tv = TextureView::read(as.resolve(ref));  // ...the component ref follows the swap
    REQUIRE(tv);
    CHECK(static_cast<u8>(tv.value().pixels(0)[0]) == 0x99);
    CHECK(ref.handle.generation == as.generation(as.find(kTex)));
    as.end_frame();
    bool saw = false;
    for (const AssetEvent& e : as.events()) saw = saw || (e.kind == AssetEvent::Kind::Reloaded && e.new_generation == e.old_generation + 1);
    CHECK(saw);

    // A broken reload is all-or-nothing: the current generation keeps serving.
    Array<byte> broken = fresh;
    broken[100] ^= byte{0xFF};
    CHECK(!as.commit_reload(kTex, broken.span()));
    CHECK(as.is_alive(ref.handle));

    for (u32 f = 0; f < MCE_FRAMES_IN_FLIGHT; ++f) as.end_frame();
    CHECK(sink.destroyed == 1);  // the retired generation's GPU objects
    as.release(ref.handle);
}

TEST_CASE("asset system: pack mount serves chunks straight out of the pack") {
    const std::string root = tmp_dir("pack_mount");
    Array<byte> tex, sfx;
    texture_container(0x22, tex);
    sfx_container(sfx);
    ManifestItem items[2] = {{kTex, xxh64(tex.data(), tex.size()), ChunkType::Texture, "a.png"},
                             {kSfx, xxh64(sfx.data(), sfx.size()), ChunkType::Sfx, "b.wav"}};
    Array<byte> mp;
    write_manifest(Span<ManifestItem>(items, 2), mp);
    ContainerWriter w;
    ContainerWriter::ChunkDesc md;
    md.type = ChunkType::Manifest;
    w.add(md, mp.span());
    for (Array<byte>* c : {&tex, &sfx}) {
        auto v = ContainerView::open(c->span());
        REQUIRE(v);
        ContainerWriter::ChunkDesc d;
        d.type = v.value().entry(0).type;
        d.version = v.value().entry(0).chunk_version;
        d.guid = v.value().entry(0).guid();
        w.add(d, v.value().payload(0));
    }
    Array<byte> pack;
    w.finish(pack);
    write_bytes(root + "/assets.mce", pack.span());

    AssetSystem as;
    const std::string pack_path = root + "/assets.mce";
    const StringView roots[] = {pack_path};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    auto s = as.load_sync<Sfx>(kSfx);
    REQUIRE(s);
    auto sv = SfxView::read(as.resolve(s.value()));
    REQUIRE(sv);
    CHECK(sv.value().samples[1] == 0.5f);
    as.release(s.value());
}

TEST_CASE("asset system: placeholders are valid payloads of their type") {
    AssetSystem as;
    CHECK(TextureView::read(as.placeholder<Texture>()).has_value());
    CHECK(SfxView::read(as.placeholder<Sfx>()).has_value());
    CHECK(SfxView::read(as.placeholder<Sfx>()).value().header->frame_count == 48000);
    CHECK(FontView::read(as.placeholder<Font>()).has_value());
    CHECK(MaterialChunk::read(as.placeholder<Material>()).has_value());
    CHECK(SceneView::read(as.placeholder<Scene>()).has_value());
    REQUIRE(MeshView::read(as.placeholder<Mesh>()).has_value());
    CHECK(MeshView::read(as.placeholder<Mesh>()).value().submeshes.empty());
}

TEST_CASE("asset system: missing mount root is an error") {
    AssetSystem as;
    const StringView roots[] = {"/definitely/not/here"};
    auto r = as.mount(Span<const StringView>(roots, 1));
    REQUIRE(!r);
    CHECK(r.error().code == Errc::FileNotFound);
}

TEST_CASE("asset system: async load reads off the main thread, commits on poll") {
    const std::string root = tmp_dir("dev_async");
    u64 th = 0, sh = 0;
    make_cooked_tree(root, th, sh);
    AssetSystem as;
    CountingSink sink;
    as.set_upload_sink(&sink);
    const StringView roots[] = {root};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    const AssetKey tk = as.find(kTex);

    const AsyncTicket t = as.request_async<Texture>(tk);
    REQUIRE(t.valid());
    CHECK(as.async_status(t) == AsyncStatus::Pending);
    CHECK(as.async_pending() == 1);
    CHECK(as.refcount(tk) == 0);  // nothing is committed before a poll
    CHECK(!as.take_async(t));     // not ready yet: the ticket stays live
    as.wait_async();
    REQUIRE(as.async_status(t) == AsyncStatus::Ready);
    CHECK(sink.textures == 1);  // uploaded at commit, on the main thread
    auto h = as.take_async(t);
    REQUIRE(h);
    CHECK(as.async_status(t) == AsyncStatus::Unknown);  // spent
    CHECK(as.refcount(tk) == 1);
    auto tv = TextureView::read(as.resolve(AssetHandle<Texture>{h.value().index, h.value().generation}));
    REQUIRE(tv);
    CHECK(static_cast<u8>(tv.value().pixels(0)[0]) == 0x40);

    // Already resident: ready at once, one more reference, no second read or upload.
    const AsyncTicket t2 = as.request_async<Texture>(tk);
    CHECK(as.async_status(t2) == AsyncStatus::Ready);
    auto h2 = as.take_async(t2);
    REQUIRE(h2);
    CHECK(h2.value().generation == h.value().generation);
    CHECK(as.refcount(tk) == 2);
    CHECK(sink.textures == 1);
    as.release(h.value());
    as.release(h2.value());

    // Wrong type and unknown keys fail at once.
    const AsyncTicket bad = as.request_async<Sfx>(tk);
    CHECK(as.async_status(bad) == AsyncStatus::Failed);
    CHECK(!as.take_async(bad));
    const AsyncTicket missing = as.request_async<Texture>(kInvalidAssetKey);
    REQUIRE(as.async_status(missing) == AsyncStatus::Failed);
    CHECK(as.take_async(missing).error().code == Errc::NotFound);
}

TEST_CASE("asset system: cancelled async loads leave nothing behind") {
    const std::string root = tmp_dir("dev_async_cancel");
    u64 th = 0, sh = 0;
    make_cooked_tree(root, th, sh);
    AssetSystem as;
    const StringView roots[] = {root};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    const AssetKey sk = as.find(kSfx);

    const AsyncTicket pending = as.request_async<Sfx>(sk);
    as.cancel_async(pending);  // the read in flight finishes and is dropped
    CHECK(as.async_status(pending) == AsyncStatus::Unknown);
    as.wait_async();
    CHECK(as.refcount(sk) == 0);

    const AsyncTicket ready = as.request_async<Sfx>(sk);
    as.wait_async();
    REQUIRE(as.async_status(ready) == AsyncStatus::Ready);
    CHECK(as.refcount(sk) == 1);
    as.cancel_async(ready);  // releases the reference the commit took
    CHECK(as.refcount(sk) == 0);

    // A ticket left pending when the system goes away is joined cleanly by the destructor.
    (void)as.request_async<Sfx>(sk);
}

TEST_CASE("asset system: an async read of a corrupt file fails like load_sync") {
    const std::string root = tmp_dir("dev_async_corrupt");
    u64 th = 0, sh = 0;
    make_cooked_tree(root, th, sh);
    {
        std::FILE* f = std::fopen(cooked_path(root, kTex, th).c_str(), "r+b");
        REQUIRE(f);
        std::fseek(f, 120, SEEK_SET);
        std::fputc(0x7F, f);
        std::fclose(f);
    }
    AssetSystem as;
    const StringView roots[] = {root};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    const AsyncTicket t = as.request_async<Texture>(as.find(kTex));
    as.wait_async();
    REQUIRE(as.async_status(t) == AsyncStatus::Failed);
    CHECK(as.take_async(t).error().code == Errc::CrcMismatch);
    as.end_frame();
    bool saw = false;
    for (const AssetEvent& e : as.events()) saw = saw || e.kind == AssetEvent::Kind::LoadFailed;
    CHECK(saw);
}

TEST_CASE("asset system: async loads from a pack are ready at once") {
    const std::string root = tmp_dir("pack_async");
    Array<byte> sfx;
    sfx_container(sfx);
    ManifestItem items[1] = {{kSfx, xxh64(sfx.data(), sfx.size()), ChunkType::Sfx, "b.wav"}};
    Array<byte> mp;
    write_manifest(Span<ManifestItem>(items, 1), mp);
    ContainerWriter w;
    ContainerWriter::ChunkDesc md;
    md.type = ChunkType::Manifest;
    w.add(md, mp.span());
    auto v = ContainerView::open(sfx.span());
    REQUIRE(v);
    ContainerWriter::ChunkDesc d;
    d.type = v.value().entry(0).type;
    d.version = v.value().entry(0).chunk_version;
    d.guid = v.value().entry(0).guid();
    w.add(d, v.value().payload(0));
    Array<byte> pack;
    w.finish(pack);
    const std::string pack_path = root + "/assets.mce";
    write_bytes(pack_path, pack.span());

    AssetSystem as;
    const StringView roots[] = {pack_path};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
    const AsyncTicket t = as.request_async<Sfx>(as.find(kSfx));
    CHECK(as.async_status(t) == AsyncStatus::Ready);
    auto h = as.take_async(t);
    REQUIRE(h);
    as.release(h.value());
}
