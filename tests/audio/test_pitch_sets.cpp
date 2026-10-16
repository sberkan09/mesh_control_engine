// M-AUDIO2 2: per-voice pitch (a playback rate, 32.32 fixed point, linear interpolation between
// frames) and sound sets (an Sfx chunk naming member clips, picked per play by the ClipBank from a
// seed the bridge derives from the tick, with pitch and gain ranges).

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

// Mono clip whose samples are exact: s[i] = i / 2^16.
Array<f32> ramp(u32 frames) {
    Array<f32> a;
    a.resize(frames);
    for (u32 i = 0; i < frames; ++i) a[i] = static_cast<f32>(i) / 65536.f;
    return a;
}

PlayParams at_pitch(f32 pitch, bool loop = false) {
    PlayParams p;
    p.pitch = pitch;
    p.loop = loop;
    p.pan = 0.f;
    return p;
}

// Left channel of a mono clip at pan 0 (constant power: both channels at sqrt(1/2)).
f32 left(const Array<f32>& out, u32 frame) { return out[frame * 2] / std::sqrt(0.5f); }

}  // namespace

TEST_CASE("pitch: rate 2 reads every other frame, 0.5 interpolates halfway, and the clip ends on time") {
    auto dev = test::offline_device();
    const Array<f32> clip = ramp(4800);
    const ClipHandle c = dev->register_clip(test::payload(clip)).value();

    const AudioHandle up = dev->play(c, at_pitch(2.f));
    Array<f32> out;
    test::render(*dev, out, 4000);
    for (u32 i = 0; i < 2400; i += 97) CHECK(left(out, i) == doctest::Approx(clip[i * 2]).epsilon(1e-6));
    CHECK_FALSE(dev->is_playing(up));  // 4800 frames at twice the speed: 2400 frames
    CHECK(out[2400 * 2] == 0.f);

    const AudioHandle down = dev->play(c, at_pitch(0.5f));
    test::render(*dev, out, 4800);
    for (u32 i = 0; i < 2000; i += 131) {
        CHECK(left(out, 2 * i) == doctest::Approx(clip[i]).epsilon(1e-6));
        CHECK(left(out, 2 * i + 1) == doctest::Approx(0.5f * (clip[i] + clip[i + 1])).epsilon(1e-5));
    }
    CHECK(dev->is_playing(down));  // 9600 frames at half speed
    test::render(*dev, out, 4800 + 480);
    CHECK_FALSE(dev->is_playing(down));
}

TEST_CASE("pitch: 1 is bit-exact with the unpitched path; any rate renders the same twice; streams ignore it") {
    const Array<f32> clip = ramp(9000);
    auto render_at = [&](f32 pitch, u64& hash) {
        auto dev = test::offline_device();
        const ClipHandle c = dev->register_clip(test::payload(clip, 1, 1000)).value();
        PlayParams p = at_pitch(pitch, true);
        dev->play(c, p);
        Array<f32> out;
        test::render(*dev, out, 48000);
        hash = dev->command_hash();
        return out;
    };
    u64 h1 = 0, h_default = 0, h13 = 0, h13b = 0;
    const Array<f32> one = render_at(1.f, h1);
    {
        auto dev = test::offline_device();
        const ClipHandle c = dev->register_clip(test::payload(clip, 1, 1000)).value();
        PlayParams p;
        p.loop = true;
        dev->play(c, p);
        Array<f32> out;
        test::render(*dev, out, 48000);
        h_default = dev->command_hash();
        bool same = true;
        for (usize i = 0; i < out.size(); ++i) same = same && out[i] == one[i];
        CHECK(same);
    }
    CHECK(h1 == h_default);  // pitch 1 hashes as plays did before pitch existed
    const Array<f32> a = render_at(1.3f, h13);
    const Array<f32> b = render_at(1.3f, h13b);
    CHECK(h13 == h13b);
    CHECK_FALSE(h13 == h1);
    bool same = true;
    f32 peak = 0.f;
    for (usize i = 0; i < a.size(); ++i) {
        same = same && a[i] == b[i];
        peak = std::fabs(a[i]) > peak ? std::fabs(a[i]) : peak;
    }
    CHECK(same);
    // A looping pitched voice stays inside the clip: never past its last sample.
    CHECK(peak <= clip[8999] * std::sqrt(0.5f) + 1e-6f);

    // A streamed clip plays at 1 whatever the pitch asks.
    auto dev = test::offline_device();
    Array<f32> stereo;
    stereo.resize(9000 * 2);
    for (u32 i = 0; i < 9000; ++i) stereo[i * 2] = stereo[i * 2 + 1] = clip[i];
    const ClipHandle s = dev->register_stream(test::payload(stereo, 2)).value();
    dev->play(s, at_pitch(2.f));
    Array<f32> out;
    test::render(*dev, out, 4800);
    CHECK(out[4000 * 2] == clip[4000]);
}

TEST_CASE("pitch: set_handle_pitch changes a live voice; out-of-range pitches clamp") {
    auto dev = test::offline_device();
    const Array<f32> clip = ramp(48000);
    const ClipHandle c = dev->register_clip(test::payload(clip)).value();
    const AudioHandle v = dev->play(c, at_pitch(1.f));
    Array<f32> out;
    test::render(*dev, out, 480);
    dev->set_handle_pitch(v, 3.f);
    test::render(*dev, out, 480);
    // 480 frames at 1, then 3 frames per frame.
    CHECK(left(out, 100) == doctest::Approx(clip[480 + 300]).epsilon(1e-6));

    const AudioHandle w = dev->play(c, at_pitch(100.f));  // clamps to kMaxPitch
    test::render(*dev, out, 16000);  // w: 48000 frames at 4; v: the rest of the clip at 3
    CHECK_FALSE(dev->is_playing(w));
    CHECK_FALSE(dev->is_playing(v));
}

// ---------------------------------------------------------------------------- sound sets

namespace {

const asset::Guid kSet{0x5e70000000000001ull, 0x0000000000000001ull};
const asset::Guid kSeq{0x5e70000000000001ull, 0x0000000000000002ull};
const asset::Guid kNested{0x5e70000000000001ull, 0x0000000000000003ull};
asset::Guid member(u32 i) { return asset::Guid{0x5e70000000000002ull, 0x100ull + i}; }

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

void container(asset::Guid guid, Span<const byte> payload, u8 minor, Array<byte>& out) {
    asset::ContainerWriter w;
    asset::ContainerWriter::ChunkDesc d;
    d.type = asset::ChunkType::Sfx;
    d.version = asset::chunk_version(asset::SFX_VERSION, minor);
    d.guid = guid;
    w.add(d, payload);
    w.finish(out);
}

// Four DC clips (member i hums at 0.1 * (i + 1)), a random set and a sequence over them, and a set
// that names another set.
std::string sets_tree(const char* name) {
    const std::string root = tmp_dir(name);
    Array<asset::ManifestItem> items;
    auto put = [&](asset::Guid g, Span<const byte> payload, u8 minor) {
        Array<byte> c;
        container(g, payload, minor, c);
        const u64 hash = asset::xxh64(c.data(), c.size());
        char file[96];
        std::snprintf(file, sizeof(file), "/%.2s/%s/%016llx.mce", asset::to_hex(g).c_str(), asset::to_hex(g).c_str(),
                      static_cast<unsigned long long>(hash));
        write_bytes(root + file, c.span());
        items.push_back(asset::ManifestItem{g, hash, asset::ChunkType::Sfx, "audio/x"});
    };
    for (u32 i = 0; i < 4; ++i) {
        Array<f32> s;
        s.resize(4800, 0.1f * static_cast<f32>(i + 1));
        asset::SfxHeader h{};
        h.sample_rate = 48000;
        h.channels = 1;
        h.frame_count = 4800;
        Array<byte> p;
        asset::write_sfx(h, s.span(), p);
        put(member(i), p.span(), 0);
    }
    asset::Guid all[4] = {member(0), member(1), member(2), member(3)};
    asset::SoundSetHeader set{};
    set.pick = asset::SoundSetPick::Random;
    set.flags = asset::SoundSetFlags::NoRepeat;
    set.pitch_min = 0.9f;
    set.pitch_max = 1.1f;
    set.gain_min_db = -6.f;
    set.gain_max_db = 0.f;
    Array<byte> p;
    asset::write_sound_set(set, Span<const asset::Guid>(all, 4), p);
    put(kSet, p.span(), 2);
    set.pick = asset::SoundSetPick::Sequence;
    set.pitch_min = set.pitch_max = 1.f;
    set.gain_min_db = set.gain_max_db = 0.f;
    asset::write_sound_set(set, Span<const asset::Guid>(all, 3), p);
    put(kSeq, p.span(), 2);
    asset::Guid inner[1] = {kSet};
    asset::write_sound_set(set, Span<const asset::Guid>(inner, 1), p);
    put(kNested, p.span(), 2);

    Array<byte> mp;
    asset::write_manifest(items.span(), mp);
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

u32 member_of(ClipBank& bank, ClipHandle c) {
    for (u32 i = 0; i < 4; ++i)
        if (bank.find(member(i)) == c) return i;
    return 99;
}

}  // namespace

TEST_CASE("sound sets: random picks cover the set without repeats, in range, the same for the same seeds") {
    asset::AssetSystem as;
    mount(as, sets_tree("sets_pick"));
    auto dev = test::offline_device();
    ClipBank bank(as, *dev);
    CHECK(bank.acquire(kSet).is_null());  // a set has no clip of its own
    CHECK(bank.is_set(kSet));
    CHECK_FALSE(clip_payload_from_sfx(as.resolve(as.load_sync<asset::Sfx>(kSet).value())));

    u32 seen[4] = {};
    u32 last = 99;
    Array<ClipPick> first;
    for (u64 seed = 0; seed < 200; ++seed) {
        const ClipPick p = bank.pick(kSet, seed * 7919);
        const u32 m = member_of(bank, p.clip);
        REQUIRE(m < 4);
        CHECK(m != last);
        last = m;
        ++seen[m];
        CHECK(p.pitch >= 0.9f);
        CHECK(p.pitch <= 1.1f);
        CHECK(p.gain_db >= -6.f);
        CHECK(p.gain_db <= 0.f);
        first.push_back(p);
    }
    for (u32 c : seen) CHECK(c > 25);

    // Another bank (another run) given the same seeds picks the same.
    auto dev2 = test::offline_device();
    ClipBank again(as, *dev2);
    for (u64 seed = 0; seed < 200; ++seed) {
        const ClipPick p = again.pick(kSet, seed * 7919);
        CHECK(member_of(again, p.clip) == member_of(bank, first[seed].clip));
        CHECK(p.pitch == first[seed].pitch);
        CHECK(p.gain_db == first[seed].gain_db);
    }

    // A sequence cycles in order; a plain clip picks itself at its own gain and pitch.
    for (u32 i = 0; i < 7; ++i) CHECK(member_of(bank, bank.pick(kSeq, 12345).clip) == i % 3);
    const ClipPick plain = bank.pick(member(2), 1);
    CHECK(plain.clip == bank.find(member(2)));
    CHECK(plain.pitch == 1.f);
    CHECK(plain.gain_db == 0.f);
    // A set inside a set plays nothing (the cook refuses one; this is the runtime's guard).
    CHECK(bank.pick(kNested, 3).clip.is_null());
    bank.release_all();
}

TEST_CASE("sound sets: the bridge picks per play from the tick, so two runs send the same commands") {
    auto run = [](const char* dir, u32& distinct) {
        asset::AssetSystem as;
        mount(as, sets_tree(dir));
        auto dev = test::offline_device();
        ClipBank bank(as, *dev);
        World w;
        install(w, *dev, &bank);
        w.add_system(Phase::FixedUpdate_Pre, "steps", [](Frame& f) {
            if (f.tick % 3 == 0) {
                PlaySfx ev;
                ev.clip = kSet;
                f.events().push(ev);
            }
        });
        AudioSource src;
        src.clip.guid = kSet;
        src.play_on_start = true;
        src.loop = true;
        const Entity e = w.spawn_with(src);
        Array<f32> out;
        f32 levels[64] = {};
        u32 n = 0;
        for (u64 t = 1; t <= 120; ++t) {
            w.tick(1.0 / 60.0, t);
            test::render(*dev, out, 800);
            if (t % 3 == 0 && n < 64) levels[n++] = out[400 * 2];
        }
        distinct = 0;
        for (u32 i = 1; i < n; ++i) distinct += levels[i] != levels[i - 1] ? 1u : 0u;
        const AudioSource* s = w.try_get<AudioSource>(e);
        CHECK(dev->is_playing(s->handle));
        CHECK(s->pick_gain <= 0);
        CHECK(s->pick_gain >= -12);
        const u64 h = dev->command_hash();
        w.destroy(e);
        bank.release_all();
        return h;
    };
    u32 d1 = 0, d2 = 0;
    const u64 a = run("sets_bridge_a", d1);
    const u64 b = run("sets_bridge_b", d2);
    CHECK(a == b);
    CHECK(d1 == d2);
    CHECK(d1 > 10);  // the footsteps vary
}
