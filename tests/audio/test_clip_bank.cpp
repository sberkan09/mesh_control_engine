// Sounds by asset GUID: cooked Sfx payload -> ClipPayload, the ClipBank (load, register, refcount,
// failures, hot reload) and the bridge playing AudioSource / PlaySfx by GUID.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "audio_test_util.h"
#include "mce/asset/xxhash.h"
#include "mce/audio/components.h"

using namespace mce;
using namespace mce::audio;
namespace fs = std::filesystem;

namespace {

const asset::Guid kHum{0x3102030405060708ull, 0x3112131415161718ull};
const asset::Guid kMissing{0x4102030405060708ull, 0x4112131415161718ull};

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

// A constant-value mono clip, optionally looping over [loop_start, loop_end).
void sfx_payload(f32 value, u64 frames, Array<byte>& out, u64 loop_start = 0, u64 loop_end = 0, u32 rate = 48000) {
    Array<f32> s;
    s.resize(static_cast<usize>(frames), value);
    asset::SfxHeader h{};
    h.sample_rate = rate;
    h.channels = 1;
    h.frame_count = frames;
    h.loop_start = loop_start;
    h.loop_end = loop_end;
    h.flags = loop_end > 0 ? asset::SfxFlags::Looping : 0u;
    asset::write_sfx(h, s.span(), out);
}

void sfx_container(asset::Guid guid, f32 value, Array<byte>& out) {
    Array<byte> payload;
    sfx_payload(value, 4800, payload);
    asset::ContainerWriter w;
    asset::ContainerWriter::ChunkDesc d;
    d.type = asset::ChunkType::Sfx;
    d.version = asset::chunk_version(asset::SFX_VERSION, 0);
    d.guid = guid;
    w.add(d, payload.span());
    w.finish(out);
}

// Dev cooked tree holding one Sfx asset (kHum, a 0.1 DC hum).
std::string cooked_tree(const char* name) {
    const std::string root = tmp_dir(name);
    Array<byte> c;
    sfx_container(kHum, 0.1f, c);
    const u64 hash = asset::xxh64(c.data(), c.size());
    char file[96];
    std::snprintf(file, sizeof(file), "/%.2s/%s/%016llx.mce", asset::to_hex(kHum).c_str(), asset::to_hex(kHum).c_str(),
                  static_cast<unsigned long long>(hash));
    write_bytes(root + file, c.span());
    asset::ManifestItem items[1] = {{kHum, hash, asset::ChunkType::Sfx, "audio/hum.wav"}};
    Array<byte> mp;
    asset::write_manifest(Span<asset::ManifestItem>(items, 1), mp);
    asset::ContainerWriter w;
    asset::ContainerWriter::ChunkDesc d;
    d.type = asset::ChunkType::Manifest;
    w.add(d, mp.span());
    Array<byte> manifest;
    w.finish(manifest);
    write_bytes(root + "/manifest.mce", manifest.span());
    return root;
}

void mount(asset::AssetSystem& as, const std::string& root) {
    const StringView roots[] = {StringView(root.c_str())};
    REQUIRE(as.mount(Span<const StringView>(roots, 1)));
}

f32 peak(const Array<f32>& a, usize from) {
    f32 m = 0.f;
    for (usize i = from; i < a.size(); ++i) m = std::fabs(a[i]) > m ? std::fabs(a[i]) : m;
    return m;
}

asset::Guid g_event_clip;

}  // namespace

TEST_CASE("clip_payload_from_sfx: cooked Sfx chunk -> mixer clip, bad payloads rejected") {
    Array<byte> one_shot;
    sfx_payload(0.5f, 100, one_shot);
    auto p = clip_payload_from_sfx(one_shot.span());
    REQUIRE(p);
    CHECK(p.value().frame_count == 100);
    CHECK(p.value().channels == 1);
    CHECK(p.value().loop_start == 0);
    CHECK(p.value().frames[99] == 0.5f);

    // A looping clip ends at loop_end and wraps to loop_start.
    Array<byte> looping;
    sfx_payload(0.5f, 100, looping, 10, 90);
    auto l = clip_payload_from_sfx(looping.span());
    REQUIRE(l);
    CHECK(l.value().frame_count == 90);
    CHECK(l.value().loop_start == 10);

    Array<byte> bad_loop;
    sfx_payload(0.5f, 100, bad_loop, 50, 120);
    CHECK_FALSE(clip_payload_from_sfx(bad_loop.span()));

    Array<byte> wrong_rate;
    sfx_payload(0.5f, 100, wrong_rate, 0, 0, 44100);
    CHECK_FALSE(clip_payload_from_sfx(wrong_rate.span()));

    const byte junk[8] = {};
    CHECK_FALSE(clip_payload_from_sfx(Span<const byte>(junk, 8)));
}

TEST_CASE("ClipBank: loads once, holds one retain, fails quietly, releases") {
    asset::AssetSystem as;
    mount(as, cooked_tree("bank_basic"));
    auto dev = test::offline_device();
    ClipBank bank(as, *dev);

    const ClipHandle a = bank.acquire(kHum);
    REQUIRE_FALSE(a.is_null());
    CHECK(dev->is_clip_registered(a));
    CHECK(bank.acquire(kHum) == a);
    CHECK(bank.find(kHum) == a);
    CHECK(as.refcount(as.find(kHum)) == 1);

    // Unknown GUID: null handle, remembered so it is not reloaded (or re-logged) every tick.
    CHECK(bank.acquire(kMissing).is_null());
    CHECK(bank.acquire(kMissing).is_null());
    CHECK(bank.size() == 2);
    CHECK(bank.acquire(asset::Guid{}).is_null());
    CHECK(bank.find(asset::Guid{}).is_null());

    bank.release(kHum);
    Array<f32> out;
    test::render(*dev, out, 480);  // the mixer acknowledges the release
    CHECK_FALSE(dev->is_clip_registered(a));
    CHECK(as.refcount(as.find(kHum)) == 0);
    CHECK(bank.find(kHum).is_null());
}

TEST_CASE("ClipBank: release_all waits for the mixer before dropping the retains (#145)") {
    asset::AssetSystem as;
    mount(as, cooked_tree("bank_release_all"));
    auto dev = test::offline_device();
    ClipBank bank(as, *dev);
    const ClipHandle a = bank.acquire(kHum);
    REQUIRE_FALSE(a.is_null());
    PlayParams pp;
    pp.loop = true;
    const AudioHandle h = dev->play(a, pp);
    Array<f32> out;
    test::render(*dev, out, 480);
    REQUIRE(dev->is_playing(h));

    // No render in between: release_all itself drains the mixer's acknowledgement.
    bank.release_all();
    CHECK_FALSE(dev->is_clip_registered(a));
    CHECK(as.refcount(as.find(kHum)) == 0);
    test::render(*dev, out, 480);
    CHECK(dev->stats().voices_real == 0);
}

TEST_CASE("audio_bridge plays AudioSource and PlaySfx by asset GUID, and follows a hot reload") {
    asset::AssetSystem as;
    mount(as, cooked_tree("bank_bridge"));
    auto dev = test::offline_device();
    ClipBank bank(as, *dev);

    World w;
    install(w, *dev, &bank);
    AudioSource src;
    src.clip.guid = kHum;
    src.loop = true;
    src.play_on_start = true;
    const Entity e = w.spawn_with(src);

    const f64 dt = 1.0 / 60.0;
    bank.sync();
    w.tick(dt, 1);
    const AudioSource* s = w.try_get<AudioSource>(e);
    REQUIRE(s != nullptr);
    CHECK(dev->is_playing(s->handle));
    CHECK(s->playing_clip == bank.find(kHum));
    Array<f32> out;
    test::render(*dev, out, 4800);
    const f32 before = peak(out, 2400);
    CHECK(before > 0.05f);

    // One-shot by GUID shares the registered clip.
    g_event_clip = kHum;
    w.add_system(Phase::FixedUpdate_Pre, "shoot", [](Frame& f) {
        if (f.tick == 2) {
            PlaySfx ev;
            ev.clip = g_event_clip;
            f.events().push(ev);
        }
    });
    w.tick(dt, 2);
    test::render(*dev, out, 480);
    CHECK(dev->stats().voices_real == 2);
    test::render(*dev, out, 9600);  // the one-shot (4800 frames) ends
    CHECK(dev->stats().voices_real == 1);

    // Hot reload: the asset now hums at 0.4. sync() re-registers it and the looping source
    // restarts on the new clip.
    const ClipHandle old_clip = bank.find(kHum);
    const AudioHandle old_voice = s->handle;
    Array<byte> fresh;
    sfx_container(kHum, 0.4f, fresh);
    REQUIRE(as.commit_reload(kHum, fresh.span()));
    bank.sync();
    const ClipHandle new_clip = bank.find(kHum);
    REQUIRE_FALSE(new_clip.is_null());
    CHECK_FALSE(new_clip == old_clip);
    CHECK(as.refcount(as.find(kHum)) == 1);
    w.tick(dt, 3);
    CHECK(s->playing_clip == new_clip);
    CHECK_FALSE(s->handle == old_voice);
    test::render(*dev, out, 4800);
    CHECK(dev->is_playing(s->handle));
    CHECK_FALSE(dev->is_clip_registered(old_clip));
    CHECK(peak(out, 4800) == doctest::Approx(before * 4.f).epsilon(0.01));
    for (u32 f = 0; f < MCE_FRAMES_IN_FLIGHT; ++f) as.end_frame();

    w.destroy(e);
    test::render(*dev, out, 4800);
    CHECK(dev->stats().voices_real == 0);
}
