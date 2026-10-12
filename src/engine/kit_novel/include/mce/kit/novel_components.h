#pragma once
// mce_kit_novel's components (M-NOVEL): plain reflected data, so a project's cook knows them by
// including this header in its TYPES file (mce_add_project) without linking the kit. The runtime
// that drives them is mce/kit/novel.h.
//
// A novel's scene is a stage: every part of the screen is an entity the scene places (a Sprite, a
// TextLabel, an AudioSource, a Light2D, the camera) tagged with a StagePart that says which part
// it plays; the kit writes textures, text and tints into them each tick. A scene without stage
// parts gets a default stage spawned on its first tick (novel::NovelState::default_stage).
//
// The whole story state is one StoryState on an entity with a SaveKey, every field Save-flagged,
// so the engine's save games carry a run with nothing else (the autosave writes it at every
// @checkpoint). NPC dialogue (novel::Dialogue) keeps its own cursor and shares the variables of
// the world's StoryState.

#include "mce/refl/macros.h"
#include "mce/types.h"

namespace mce::novel {

// What a stage entity is for. `index` tells parts of the same role apart: background layer 0..4
// (far to near), cast slot 0..3, line 0..3, choice row 0..3, letter line 0..15, panel line 0..15.
enum class StageRole : u8 {
    None,
    Background,    // Sprite: one layer of the location (textures/bg/<location>_<layer>.png)
    Cast,          // Sprite: a cast slot (textures/cast/<who>_<expression>_<pose>.png)
    Dim,           // Sprite: the full-screen shade under choices and panels
    Fade,          // Sprite: the full-screen black used for scene changes
    Box,           // Sprite: the dialogue box
    NameTag,       // Sprite: the speaker tag
    NameText,      // TextLabel: the speaker's name
    Line,          // TextLabel: one of the box's lines
    ChoiceRow,     // Sprite: a choice row (also the title menu and a game screen's rows)
    ChoiceText,    // TextLabel: a choice row's first line
    ChoiceText2,   // TextLabel: a choice row's second line
    LetterCard,    // Sprite: the paper a letter is read on
    LetterLine,    // TextLabel: one line of a letter
    Panel,         // Sprite: the panel behind a checkpoint, the backlog, an ending, the extras page
    PanelLine,     // TextLabel: one panel line
    Hud,           // TextLabel: the game's HUD line (NovelHooks::hud)
    Title,         // TextLabel: the game's title on the title screen
    Glow,          // Sprite: the soft light behind the selected row
    Music,         // AudioSource: the looping music bed
    Ambience,      // AudioSource: the looping ambience bed
    Light,         // Light2D (Point): the lamp that moves with the time of day
    AmbientLight,  // Light2D (Ambient): the light the background layers are lit by
    Camera,        // Post2DComponent: the colour grade (textures/lut_<name>.png) and vignette
};
MCE_ENUM(StageRole, None, Background, Cast, Dim, Fade, Box, NameTag, NameText, Line, ChoiceRow, ChoiceText, ChoiceText2,
         LetterCard, LetterLine, Panel, PanelLine, Hud, Title, Glow, Music, Ambience, Light, AmbientLight, Camera)

struct StagePart {
    StageRole role = StageRole::None;
    u8 index = 0;
    u8 pad_[2] = {};
};
MCE_COMPONENT(StagePart, role, (index, Range{0, 15}, Help{"which of several parts with this role"}))

inline constexpr u32 kMaxFlags = 256;    // flags[8] bits
inline constexpr u32 kMaxCounters = 64;  // counters[]
inline constexpr u32 kMaxDepth = 8;      // the call stack
inline constexpr u32 kMaxSlots = 4;      // cast slots

// The story state (a novel's run, or the variables NPC dialogue shares). `pc` is the statement
// the run resumes at (0 = no run in progress), `ret` the call stack. Variables are flags (bits)
// and counters (i16); `mce story compile` numbers them from story/story.json. The stage fields
// are the picture at the save point, so Continue rebuilds it (character 255 = empty slot).
struct StoryState {
    u16 pc = 0;
    u16 ret[kMaxDepth] = {};
    u8 depth = 0;
    u8 pad0_ = 0;
    u32 flags[kMaxFlags / 32] = {};
    i16 counters[kMaxCounters] = {};
    u32 endings_seen = 0;  // bit e: ending e reached in this slot (kept across new runs)
    u32 pages = 0;         // pages read in this run (statistics)
    u8 bg = 0, bg_time = 0, bg_lut = 0, music = 0, ambience = 0;
    u8 pad1_[3] = {};
    u8 cast[kMaxSlots] = {255, 255, 255, 255};
    u8 expr[kMaxSlots] = {};
    u8 pose[kMaxSlots] = {};
};
MCE_COMPONENT(StoryState, (pc, Save, ReadOnly), (ret, Save, ReadOnly), (depth, Save, ReadOnly), (flags, Save),
              (counters, Save), (endings_seen, Save), (pages, Save, ReadOnly), (bg, Save, ReadOnly),
              (bg_time, Save, ReadOnly), (bg_lut, Save, ReadOnly), (music, Save, ReadOnly), (ambience, Save, ReadOnly),
              (cast, Save, ReadOnly), (expr, Save, ReadOnly), (pose, Save, ReadOnly))

}  // namespace mce::novel
