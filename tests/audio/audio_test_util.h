#pragma once
// Offline-backend helpers: the exact callback body, pumped by the test (fake device, fake clock).

#include <doctest/doctest.h>

#include "mce/audio/audio.h"
#include "mce/containers/array.h"

namespace mce::test {

inline mem::UniquePtr<audio::AudioDevice> offline_device(u32 fifo = 1024) {
    audio::AudioDesc d;
    d.backend = audio::Backend::Offline;
    d.command_capacity = fifo;
    auto r = audio::AudioDevice::create(d);
    REQUIRE(r.has_value());
    return std::move(r).value();
}

// Constant-value mono clip.
inline Array<f32> dc_clip(u32 frames, f32 value) {
    Array<f32> a;
    a.resize(frames, value);
    return a;
}

inline audio::ClipPayload payload(const Array<f32>& frames, u32 channels = 1, u32 loop_start = 0) {
    audio::ClipPayload p;
    p.frames = frames.data();
    p.frame_count = static_cast<u32>(frames.size() / channels);
    p.channels = channels;
    p.loop_start = loop_start;
    return p;
}

// Renders `frames` stereo frames into `out` in device-sized blocks, calling update() between
// blocks like a frame loop would.
inline void render(audio::AudioDevice& dev, Array<f32>& out, u32 frames, u32 block = 480, bool update = true) {
    out.resize(static_cast<usize>(frames) * 2);
    for (u32 done = 0; done < frames;) {
        const u32 n = frames - done < block ? frames - done : block;
        dev.render_offline(out.data() + done * 2, n);
        if (update) dev.update();
        done += n;
    }
}

}  // namespace mce::test
