#pragma once
// Rain and wind ambience beds that follow the weather (M-WEATHERAUDIO 10a). One optional WeatherAudio per scene (the
// first in ECS order drives). install_weather registers the component and adds the system "kit_weather_audio"
// (FixedUpdate_Pre, after "kit_weather", so it reads the weather of this tick).
//
//   WeatherAudio  `rain_source` and `wind_source` name two ordinary AudioSource entities (looping, play_on_start, any
//                 bus, an optional fade_s; the clips are the author's). `rain_db` (0) and `wind_db` (0) are the gain at
//                 full level, Range -60..12. `fade_seconds` (3, Range 0..60) is the time a bed takes from silent to
//                 full. `enabled` (1). Engine-written and saved: `rain_q` and `wind_q`, the current levels as
//                 integers out of 2^24 (the Weather.wet_q convention), and an explicit pad.
//
// The system only writes AudioSource::gain_db on the two named entities. It never starts or stops a source and never
// writes the Weather (its bytes stay as they are); audio_bridge follows a gain_db change on a playing source every
// tick, so nothing else is needed. A null or dead entity, or one without an AudioSource, is skipped; a source without
// a clip stays silent whatever the gain.
//   * Targets come from the first Weather: rain = kind weight x intensity, the weight by `current` (clear 0,
//     overcast 0.15, rain 1, storm 1, snow 0.35, a fixed constant), clamped to 0..1; wind = Weather.wind clamped to
//     0..1 (a calm 0.1 wind is a quiet bed).
//   * Each tick a level moves toward its target by the fixed integer step 2^24 / max(1, round(fade_seconds / dt)) and
//     stops on the target, so the fade is a ramp, a pure function of the saved levels and the tick count (no wall
//     clock). fade_seconds 0 reaches the target in one tick.
//   * gain_db = max_db + 20 log10(level), level in 0..1, floored at -60 dB; a level of 0 is exactly -60. Computed in f64,
//     stored as f32.
//   * NaN: `fade_seconds` falls back to 3, `rain_db` and `wind_db` to 0, a NaN Weather.intensity or wind counts as 0.
//     Authoring is clamped to its Range when read and not rewritten.
//   * Nothing is touched (no level moves, no gain is written, the world is unchanged) when there is no Weather, no
//     WeatherAudio, `enabled` is 0, or the world has no audio resources: neither an event queue for audio::PlaySfx
//     (registered by the audio install) nor an audio::AudioRef resource, as in a headless sim without audio. The
//     levels do not advance in such a world either. Where audio exists but nothing plays (no device, a Low tier),
//     only gain_db moves.
// `rain_source` and `wind_source` are authoring (not Save, like other entity references) and hash as raw handle
// bytes. Scenes without a WeatherAudio (or without a Weather) are unchanged. AudioSource::gain_db is authoring that
// the system writes, so a scene with a WeatherAudio hashes differently from one without by design.

#include "mce/ecs/entity.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct WeatherAudio {
    Entity rain_source;       // AudioSource entity for the rain bed; null: none
    Entity wind_source;       // AudioSource entity for the wind bed; null: none
    f32 rain_db = 0.0f;       // gain at full rain level
    f32 wind_db = 0.0f;       // gain at full wind level
    f32 fade_seconds = 3.0f;  // seconds from silent to full
    u8 enabled = 1;
    u8 pad_[3] = {};          // explicit pad (state_hash hashes whole columns); reserved
    u32 rain_q = 0;           // engine-written: rain level numerator out of 2^24
    u32 wind_q = 0;           // engine-written: wind level numerator out of 2^24
};
MCE_COMPONENT(WeatherAudio, (rain_source, Help{"the looping AudioSource entity that carries the rain sound"}),
              (wind_source, Help{"the looping AudioSource entity that carries the wind sound"}),
              (rain_db, Save, Range{-60, 12}, Help{"gain of the rain bed at full level, in dB"}),
              (wind_db, Save, Range{-60, 12}, Help{"gain of the wind bed at full level, in dB"}),
              (fade_seconds, Save, Range{0, 60}, Help{"seconds a bed takes from silent to full"}),
              (enabled, Save, Help{"0: the system does nothing"}), (rain_q, Save, ReadOnly), (wind_q, Save, ReadOnly))

// Adds the system (called by install_weather). Idempotent only through install_weather.
void install_weather_audio(World& world);

}  // namespace mce::kit
