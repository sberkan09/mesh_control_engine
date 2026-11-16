#include "mce/editor/unique_names.h"

namespace mce::editor {

namespace {

bool taken(const Array<FixedString<64>>& used, StringView name) {
    for (const FixedString<64>& u : used)
        if (u.view() == name) return true;
    return false;
}

// "Wall (12)" -> "Wall"; anything else is returned as it is.
StringView strip_suffix(StringView name) {
    if (name.size() < 4 || name[name.size() - 1] != ')') return name;
    usize i = name.size() - 1;
    usize digits = 0;
    while (i > 0 && name[i - 1] >= '0' && name[i - 1] <= '9') {
        --i;
        ++digits;
    }
    if (digits == 0 || i < 3 || name[i - 1] != '(' || name[i - 2] != ' ') return name;
    return StringView(name.data(), i - 2);
}

FixedString<64> with_suffix(StringView base, u32 n) {
    char digits[16];
    usize len = 0;
    for (u32 v = n; len < sizeof digits - 1; v /= 10) {  // reversed
        digits[len++] = static_cast<char>('0' + v % 10);
        if (v < 10) break;
    }
    FixedString<64> tail;
    tail.append(StringView(" (", 2));
    while (len > 0) tail.append(StringView(&digits[--len], 1));
    tail.append(StringView(")", 1));
    const usize room = 63 - tail.size();  // the base is cut so the suffix always fits
    FixedString<64> out;
    out.append(base.size() > room ? StringView(base.data(), room) : base);
    out.append(tail.view());
    return out;
}

}  // namespace

usize make_names_unique(EditorScene& scene, Span<const EntityId> ids) {
    auto in_ids = [&](EntityId id) {
        for (EntityId i : ids)
            if (i == id) return true;
        return false;
    };
    Array<FixedString<64>> used;  // names of the entities that stay as they are, then the ones decided so far
    for (const DocEntity& e : scene.doc().entities())
        if (!in_ids(e.id)) used.push_back(e.name);

    struct Rename {
        EntityId id;
        FixedString<64> name;
    };
    Array<Rename> renames;
    for (const DocEntity& e : scene.doc().entities()) {
        if (!in_ids(e.id)) continue;
        if (!taken(used, e.name.view())) {
            used.push_back(e.name);
            continue;
        }
        const StringView base = strip_suffix(e.name.view());
        FixedString<64> candidate;
        for (u32 n = 2;; ++n) {
            candidate = with_suffix(base, n);
            if (!taken(used, candidate.view())) break;
        }
        used.push_back(candidate);
        renames.push_back({e.id, candidate});
    }
    if (renames.empty()) return 0;
    scene.begin_edit("Make names unique");
    for (const Rename& r : renames) scene.rename_entity(r.id, r.name.view());
    scene.end_edit();
    return renames.size();
}

}  // namespace mce::editor
