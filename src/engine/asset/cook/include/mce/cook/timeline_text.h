#pragma once
// The .mctimeline source format (M-CINEMA): read by the `timeline` importer, written by tools that
// edit a timeline (the editor's Timeline panel, MCP). The format is documented in
// timeline_importer.cpp and user guide 13.5.
//
// TimelineDocument is the source as data: tracks as they are authored (a transform track stays one
// track with up to three channels; the cook splits it into LocalTransform property tracks).
// Writing emits only what differs from the defaults, so reading what was written gives back the
// same document, and cooking it the same CHNK_TIMELINE bytes.

#include "mce/asset/formats.h"
#include "mce/asset/guid.h"
#include "mce/cook/diagnostics.h"
#include "mce/cook/json.h"

namespace mce::cook {

inline constexpr u32 kTimelineFormatVersion = 1;

enum class TimelineDocTrackType : u8 { Transform, Property, Activation, Animation, Audio, Camera, Event, Subtitle, Video };
inline constexpr u32 kTimelineDocTrackTypes = 9;
const char* timeline_track_type_name(TimelineDocTrackType t);  // "transform", ...
const char* timeline_ease_name(asset::TimelineEase e);         // "linear", ...

namespace TimelineChannel {
inline constexpr u8 Position = 1u << 0;
inline constexpr u8 Rotation = 1u << 1;
inline constexpr u8 Scale = 1u << 2;
}

// One key. Which fields mean something depends on the track type.
struct TimelineDocKey {
    f32 t = 0.0f;         // seconds (Activation: the range's start)
    f32 duration = 0.0f;  // Activation: the range's length; Subtitle: the line's; Video: how long it shows (0: to its end)
    asset::TimelineEase ease = asset::TimelineEase::Linear;  // Transform / Property
    f32 value[4] = {};    // Property (value_count floats)
    u8 channels = 0;      // Transform: TimelineChannel bits set by this key
    f32 position[3] = {};
    f32 rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    f32 scale[3] = {1.0f, 1.0f, 1.0f};
    String name;          // Animation: the clip; Event: the signal; Subtitle: the text
    String speaker;       // Subtitle (empty: none)
    bool loop = false;    // Animation
    f32 fade = -1.0f;     // Animation: < 0 keeps the Animator's fade
    asset::Guid sound;    // Audio
    f32 gain_db = 0.0f;   // Audio, Video
    asset::Guid video;    // Video
    u8 camera = asset::kTimelineSelf;  // Camera: the cut's camera
    f32 blend = 0.0f;                  // Camera: seconds the view eases over from the previous cut's camera
};

struct TimelineDocTrack {
    TimelineDocTrackType type = TimelineDocTrackType::Event;
    u8 target = asset::kTimelineSelf;  // binding index or kTimelineSelf (Camera / Subtitle: unused)
    String path;                       // Property / Activation: "Component.field"
    u8 value_count = 0;                // Property: resolved from `path` by the reader
    bool quat = false;                 // Property: the field is a quaternion (slerp)
    Array<TimelineDocKey> keys;        // in time order
};

struct TimelineDocument {
    String name;
    f32 duration = 0.0f;  // 0: the end of the last key
    Array<TimelineDocTrack> tracks;

    // The end of the last key (its time plus its duration).
    f32 keys_end() const;
    // The duration a player plays: `duration`, or keys_end() when it is 0.
    f32 length() const { return duration > 0.0f ? duration : keys_end(); }
};

// Resolves a Property / Activation path against the reflected components: sets value_count and
// quat. False with `why` when the path does not name a field the track can drive.
bool resolve_timeline_path(TimelineDocTrack& track, String& why);

// Parses a .mctimeline root into `out`. False when any error diagnostic was added.
bool read_timeline_document(JsonValue doc, StringView file, TimelineDocument& out, DiagList& diags);

// Appends `d` as .mctimeline source text (two-space indented, one key per line, a trailing newline).
void write_timeline_document(const TimelineDocument& d, asset::Guid guid, String& out);

// The CHNK_TIMELINE payload of a document that read cleanly.
void build_timeline_chunk(const TimelineDocument& d, Array<byte>& payload);

}  // namespace mce::cook
