#pragma once
// Subtitles and sound captions (M-ACCESS step 1): what the player's subtitle view (mce/ui/subtitles.h)
// shows at the bottom of the screen. Lines come from three places:
//
//   - timeline subtitle tracks (timeline.h): the line a playing TimelinePlayer is on, while it is on it
//   - SoundCaption, a tag on an entity with an SfxTrigger or an AudioSource: "[door slams]" each time
//     the trigger fires, or while the source plays
//   - ShowCaption, an event a game pushes for anything else (a voiced line, a one-shot PlaySfx)
//
// The view decides what to draw (access.subtitles for speech, access.captions for sounds); the sim
// keeps every line either way. The Captions resource is presentation state (resources are not in the
// state hash) and SoundCaption holds only authored fields, so a run hashes the same with captions
// on or off. kit::install_stock installs it.

#include "mce/containers/array.h"
#include "mce/ecs/entity.h"
#include "mce/fixed_string.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

enum class CaptionKind : u8 { Speech, Sound };
MCE_ENUM(CaptionKind, Speech, Sound)

// Show a line for `seconds` of sim time (<= 0: kDefaultCaptionSeconds). Push in FixedUpdate_Pre or
// _Post; the line shows from the tick "kit_captions" reads it.
struct ShowCaption {
    FixedString<32> speaker;  // "" for narration and sounds
    FixedString<192> text;
    f32 seconds = 0.0f;
    CaptionKind kind = CaptionKind::Speech;
};

inline constexpr f32 kDefaultCaptionSeconds = 3.0f;

// A caption for the sound this entity makes: its SfxTrigger's plays (each fire shows `text` for
// `seconds`), or its AudioSource (shown while the source plays; a one-shot source for `seconds`).
struct SoundCaption {
    FixedString<48> text;    // "[door slams]"
    f32 seconds = 2.0f;
};
MCE_COMPONENT(SoundCaption, (text, Help{"what the sound is, shown when sound captions are on: \"[door slams]\""}),
              (seconds, Range{0.5f, 30}, Help{"how long a one-shot's caption stays"}))

struct CaptionLine {
    FixedString<32> speaker;
    FixedString<192> text;
    CaptionKind kind = CaptionKind::Speech;
    f32 left = 0.0f;  // seconds still to show (a playing loop's line refreshes each tick)
    Entity source;    // the SoundCaption entity, else null
};

// The lines showing, oldest first (at most kMaxCaptions; a new line drops the oldest), and what
// each SoundCaption entity had done by last tick (kept here, not in the component, so the
// component's bytes stay the authored ones).
struct Captions {
    struct Seen {
        Entity entity;
        u32 fired = 0;        // SfxTrigger plays already captioned
        bool playing = false;  // the AudioSource had started
    };
    Array<CaptionLine> lines;
    Array<Seen> seen;
};
inline constexpr u32 kMaxCaptions = 8;

// Registers SoundCaption, ShowCaption, the Captions resource and "kit_captions" (FixedUpdate_Post,
// after "kit_effects" when installed). Idempotent.
void install_captions(World& world);

// Every line to show now: the playing timelines' subtitles (by entity order), then the Captions
// lines oldest first. Speech and sounds alike; the view filters by kind.
void caption_lines(World& world, Array<CaptionLine>& out);

}  // namespace mce::kit
