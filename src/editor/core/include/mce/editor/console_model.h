#pragma once
// ConsoleModel — the console panel's data (editor-tools.md §2.9, FR-DBG-040). Headless.
//
// Lines come from core's in-memory console ring (log::read_console), drained once per editor
// frame into a larger scrollback here. Commands go through core's convar/confunc registry
// (console::execute), so a "set" takes effect the same frame and its feedback is logged.

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/log.h"

namespace mce::editor {

class ConsoleModel {
public:
    static constexpr usize kDefaultScrollback = 4096;
    static constexpr usize kHistorySize = 64;

    explicit ConsoleModel(usize scrollback = kDefaultScrollback);

    // Pulls new lines from the log ring. Returns how many arrived. Lines that overflowed the log
    // ring between two polls are counted in dropped().
    usize poll();

    usize size() const { return count_; }
    // i = 0 is the oldest line still in the scrollback.
    const log::ConsoleLine& line(usize i) const;
    void clear();
    u64 dropped() const { return dropped_; }
    // Bumps whenever lines are added or cleared (lets the panel keep its scroll position).
    u64 revision() const { return revision_; }

    // Executes one console line ("name", "name value", "func args"). Echoes it to the log first.
    // Returns false when the name is unknown or the value fails to parse.
    bool exec(StringView line);

    // Command history, newest last. browse_history(-1) steps back, (+1) forward; returns the
    // entry to show, or "" past the newest one.
    usize history_size() const { return history_.size(); }
    StringView history(usize i) const { return history_[i].view(); }
    StringView browse_history(i32 direction);

    // Names of convars and confuncs starting with `prefix`, sorted. Returns the count.
    usize complete(StringView prefix, Array<const char*>& out) const;
    // Longest common prefix of the completions of `prefix` (for Tab).
    FixedString<128> complete_common(StringView prefix) const;

private:
    Array<log::ConsoleLine> lines_;  // ring of capacity lines_.size()
    usize head_ = 0;                 // index of the oldest line
    usize count_ = 0;
    u64 last_seq_ = 0;
    u64 dropped_ = 0;
    u64 revision_ = 0;
    Array<FixedString<256>> history_;
    i32 history_cursor_ = -1;  // -1 = not browsing
};

}  // namespace mce::editor
