#include "mce/editor/hierarchy_filter.h"

namespace mce::editor {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool contains_word(StringView hay, StringView word) {
    if (word.size() > hay.size()) return false;
    for (usize i = 0; i + word.size() <= hay.size(); ++i) {
        usize j = 0;
        while (j < word.size() && lower(hay[i + j]) == lower(word[j])) ++j;
        if (j == word.size()) return true;
    }
    return false;
}

}  // namespace

bool HierarchyFilter::name_matches(StringView name, StringView query) {
    usize i = 0;
    while (i < query.size()) {
        while (i < query.size() && query[i] == ' ') ++i;
        const usize start = i;
        while (i < query.size() && query[i] != ' ') ++i;
        if (i > start && !contains_word(name, StringView(query.data() + start, i - start))) return false;
    }
    return true;
}

void HierarchyFilter::rebuild(const EditorScene& scene, StringView query) {
    const SceneDocument& doc = scene.doc();
    bool blank = true;
    for (const char c : query) blank = blank && c == ' ';
    active_ = !blank;
    matches_ = 0;
    flags_.clear();
    if (!active_) return;
    for (usize i = 0; i < doc.size(); ++i) flags_.push_back(u8{0});
    for (usize i = 0; i < doc.size(); ++i) {
        if (!name_matches(doc.at(i).name.view(), query)) continue;
        flags_[i] = 2;
        ++matches_;
    }
    // Ancestors of the matches; the walk stops at a root, an already-marked entity or after doc.size() steps (a cycle).
    for (usize i = 0; i < doc.size(); ++i) {
        if (flags_[i] != 2) continue;
        EntityId p = scene.parent_of(doc.at(i).id);
        for (usize steps = 0; p != kNoEntity && steps < doc.size(); ++steps) {
            const i32 pi = doc.index_of(p);
            if (pi < 0) break;
            u8& f = flags_[static_cast<usize>(pi)];
            if (f != 0) break;
            f = 1;
            p = scene.parent_of(p);
        }
    }
}

}  // namespace mce::editor
