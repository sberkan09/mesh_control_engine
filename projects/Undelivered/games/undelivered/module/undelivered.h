#pragma once
// Undelivered gameplay: the story VM, the presentation on the 2D kit, input, saves and the bot.

#include "components.h"
#include "mce/asset/guid.h"
#include "mce/gameplay.h"

namespace undelivered {

// Where the run is between two inputs.
enum class Mode : mce::u8 {
    Boot,          // the first tick: tables, the save entity
    Title,         // the title menu
    Letters,       // the title's Letters page (endings and letters seen)
    Reading,       // a page is on the box
    Choice,        // choice rows are up
    Letter,        // a letter page is on the card
    Drawer,        // pick an envelope
    DrawerAction,  // sealed / read / leave
    Journal,       // the night panel (the autosave)
    Backlog,       // the last pages
    Ending,        // the ending card
    Fade,          // a timed black
    Wait,          // a timed hold
    Done,          // nothing to run (no save entity in the scene)
};

struct LogEntry {
    mce::u8 speaker = 255;
    mce::u16 line[4] = {};
};

// The runtime state the systems share (a resource; not saved: the save is NovelSave on its entity).
// A host or test sets `autopilot` before the first tick: 0 plays by input, 1 + story::Tag picks
// options by that tag (first, leave, stay, shut, write) and never waits.
struct NovelState {
    mce::u8 autopilot = 0;
    bool autosave = true;  // write the autosave slot at the journal (tests turn it off)
    bool instant_text = false;
    Mode mode = Mode::Boot;
    mce::Entity save_entity;
    mce::asset::AssetSystem* assets = nullptr;

    // asset tables (GUIDs by name, from the pack's paths)
    mce::asset::Guid bg[16][5];
    mce::u8 bg_count[16] = {};
    mce::asset::Guid cast[8][5][2];
    mce::asset::Guid lut[5];
    mce::asset::Guid music[8];
    mce::asset::Guid ambience[8];
    mce::asset::Guid sfx[12];
    mce::asset::Guid paper_normal;

    // the page on the box
    mce::u8 page_speaker = 255;
    mce::u16 page_line[4] = {};
    mce::u32 page_chars = 0;  // code points over the four lines
    mce::f32 revealed = 0.0f;

    // the choice (or the title menu / drawer actions, which reuse the rows)
    mce::u16 opt_first = 0, opt_count = 0;
    mce::u8 vis[4] = {};  // option indices shown, in order
    mce::u8 vis_count = 0;
    mce::u8 cursor = 0;

    // the letter card
    mce::u16 letter_first = 0, letter_count = 0;
    mce::u8 letter_number = 0;

    // the drawer
    mce::u8 drawer_cursor = 0;

    // timers and transitions
    mce::u32 hold_ticks = 0;
    mce::f32 fade = 0.0f;  // black overlay alpha
    mce::f32 cast_alpha[3] = {};
    mce::u32 ticks = 0;
    mce::u32 skip_cooldown = 0;
    mce::f32 prev_move_x = 0.0f, prev_move_y = 0.0f;

    // the backlog ring
    LogEntry log[64];
    mce::u8 log_head = 0, log_count = 0, log_scroll = 0;

    // statistics (tests, the journal)
    mce::u32 pages = 0, choices = 0, letters_read = 0, saves = 0, ops = 0;
    mce::u8 ending = 0;  // story::Ending reached (0 = none yet)
    mce::u8 endings_this_session = 0;
    bool finished = false;  // an @end ran
    bool paused_for_input = false;
};

// Registers the game's components and systems on top of the engine's.
mce::ResultV install(mce::World& world);

// The NovelSave of the scene's save entity (null when the scene has none / before the first tick).
NovelSave* novel_save(mce::World& world);

// Starts a fresh run on the next tick (keeps the slot's endings_seen).
void new_game(mce::World& world);

}  // namespace undelivered
