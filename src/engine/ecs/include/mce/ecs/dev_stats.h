#pragma once
// DevStatsLines — extra lines for the stock player's stats overlay (F3, r.stats). A world resource
// any module writes by key and the player draws under its render stats line, so an opt-in module
// (mce_net's NetStatsMeter, M-NET2 step 1) shows its numbers without the player linking it.
// Resources are not part of World::state_hash, so writing it never changes a replay.
//
//   world.set_resource(DevStatsLines{});  // once (or let the writer add it)
//   world.resource<DevStatsLines>().set("net", "NET RTT 152 ms ...");

#include "mce/fixed_string.h"

namespace mce {

struct DevStatsLines {
    static constexpr u32 kMaxLines = 4;
    struct Line {
        FixedString<32> key;
        FixedString<160> text;
    };
    Line lines[kMaxLines];
    u32 count = 0;

    // Replaces the line named `key`, or adds it (ignored when every line is taken).
    void set(StringView key, StringView text) {
        for (u32 i = 0; i < count; ++i) {
            if (lines[i].key.view() == key) {
                lines[i].text.assign(text);
                return;
            }
        }
        if (count == kMaxLines) return;
        lines[count].key.assign(key);
        lines[count].text.assign(text);
        ++count;
    }

    void remove(StringView key) {
        for (u32 i = 0; i < count; ++i) {
            if (lines[i].key.view() != key) continue;
            for (u32 j = i + 1; j < count; ++j) lines[j - 1] = lines[j];
            --count;
            return;
        }
    }
};

}  // namespace mce
