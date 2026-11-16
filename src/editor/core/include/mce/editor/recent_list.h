#pragma once
// RecentList — the most recently used scene files (M-RECENT): newest first, no duplicates, at most `cap` entries.
// The shell touches a path whenever a scene is opened or saved and lists the rest under File > Open Recent. The list
// saves as one path per line, newest first, so the file is readable and diff-friendly. Headless, tested in CI.

#include "mce/containers/array.h"
#include "mce/cook/string.h"

namespace mce::editor {

class RecentList {
public:
    static constexpr u32 kDefaultCap = 10;

    explicit RecentList(u32 cap = kDefaultCap) : cap_(cap) {}

    // Moves `path` to the front (adding it when new) and drops the oldest entries beyond the cap. An empty path is
    // ignored.
    void touch(StringView path);
    // Removes `path` (a file that no longer exists); false when it was not listed.
    bool remove(StringView path);
    void clear() { items_.clear(); }

    const Array<cook::String>& items() const { return items_; }
    u32 cap() const { return cap_; }

    // One path per line, newest first.
    cook::String serialize() const;
    // Replaces the list with the lines of `text` (newest first). Blank lines, '\r' and repeated paths are skipped;
    // lines beyond the cap are dropped.
    void parse(StringView text);

private:
    Array<cook::String> items_;
    u32 cap_;
};

}  // namespace mce::editor
