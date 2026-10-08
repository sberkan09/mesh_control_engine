#pragma once
// Synthetic event helpers for mce_input tests (no SDL, no window).

#include "mce/input/default_map.h"
#include "mce/input/input_system.h"

namespace mce::test {

inline platform::Event key_event(platform::Key k, bool down, u64 ts_us, u32 flags = 0) {
    platform::Event e;
    e.type = down ? platform::EventType::KeyDown : platform::EventType::KeyUp;
    e.code = static_cast<u32>(k);
    e.flags = flags;
    e.timestamp_us = ts_us;
    return e;
}

inline platform::Event mouse_move(f32 x, f32 y, u64 ts_us) {
    platform::Event e;
    e.type = platform::EventType::MouseMove;
    e.x = x;
    e.y = y;
    e.timestamp_us = ts_us;
    return e;
}

inline platform::Event simple_event(platform::EventType t, u64 ts_us, u32 code = 0) {
    platform::Event e;
    e.type = t;
    e.code = code;
    e.timestamp_us = ts_us;
    return e;
}

// An InputSystem on the default map with its tick grid starting at t = 0.
inline void setup_default(input::InputSystem& in) {
    Array<byte> chunk;
    input::default_map_chunk(chunk);
    auto r = in.load_map(chunk.span());
    REQUIRE(r.has_value());
    in.set_tick_origin_us(0);
    in.set_tick_rate(60);
}

}  // namespace mce::test
