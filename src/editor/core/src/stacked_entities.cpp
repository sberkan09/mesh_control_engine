#include "mce/editor/stacked_entities.h"

#include <algorithm>
#include <cstdio>

#include "mce/ecs/transform.h"
#include "mce/refl/type.h"

namespace mce::editor {

namespace {

u64 mix(u64 h, u64 v) { return (h ^ v) * 0x100000001b3ull; }

// What two entities must share to be copies, cheap to compare: the prefab, the component types and the LocalTransform
// bytes.
u64 key_of(const DocEntity& e, const DocComponent& transform) {
    u64 h = mix(0xcbf29ce484222325ull, e.prefab.hi);
    h = mix(h, e.prefab.lo);
    for (const DocComponent& c : e.components) h = mix(h, static_cast<u64>(reinterpret_cast<uintptr_t>(c.type)));
    for (u64 w : transform.storage) h = mix(h, w);
    return h;
}

bool same_entity(const DocEntity& a, const DocEntity& b) {
    if (!(a.prefab == b.prefab) || a.components.size() != b.components.size()) return false;
    for (usize i = 0; i < a.components.size(); ++i) {
        const DocComponent& x = a.components[i];
        const DocComponent& y = b.components[i];
        if (x.type != y.type || x.overrides != y.overrides) return false;
        if (changed_fields_mask(*x.type, x.data(), y.data()) != 0) return false;
    }
    return true;
}

struct Candidate {
    u64 key = 0;
    usize index = 0;  // in the scene's entity order
};

}  // namespace

Array<StackedGroup> find_stacked(const SceneDocument& doc) {
    const refl::TypeRecord& transform_type = refl::type_of<LocalTransform>();
    Array<Candidate> cands;
    cands.reserve(doc.size());
    for (usize i = 0; i < doc.size(); ++i) {
        const DocEntity& e = doc.at(i);
        if (const DocComponent* t = e.find(transform_type)) cands.push_back({key_of(e, *t), i});
    }
    std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
        return a.key != b.key ? a.key < b.key : a.index < b.index;
    });
    Array<Array<usize>> found;  // groups as entity-order indices; a run of equal keys is split into its real copies
    for (usize run = 0; run < cands.size();) {
        usize end = run;
        while (end < cands.size() && cands[end].key == cands[run].key) ++end;
        Array<Array<usize>> parts;  // within the run: greedy partition by real equality (a key collision splits)
        for (usize k = run; k < end; ++k) {
            const DocEntity& e = doc.at(cands[k].index);
            bool placed = false;
            for (Array<usize>& p : parts)
                if (same_entity(doc.at(p[0]), e)) {
                    p.push_back(cands[k].index);
                    placed = true;
                    break;
                }
            if (!placed) {
                Array<usize> p;
                p.push_back(cands[k].index);
                parts.push_back(std::move(p));
            }
        }
        for (Array<usize>& p : parts)
            if (p.size() >= 2) found.push_back(std::move(p));
        run = end;
    }
    std::sort(found.begin(), found.end(), [](const Array<usize>& a, const Array<usize>& b) { return a[0] < b[0]; });
    Array<StackedGroup> out;
    for (const Array<usize>& g : found) {
        StackedGroup sg;
        for (usize i : g) sg.ids.push_back(doc.at(i).id);
        out.push_back(std::move(sg));
    }
    return out;
}

cook::String format_stacked(const SceneDocument& doc, Span<const StackedGroup> groups) {
    cook::String out;
    for (const StackedGroup& g : groups) {
        const DocEntity* first = doc.find(g.ids[0]);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s (id %u) x%zu: ids ", first != nullptr ? first->name.c_str() : "?",
                      static_cast<unsigned>(g.ids[0]), static_cast<size_t>(g.ids.size()));
        out += buf;
        for (usize i = 0; i < g.ids.size(); ++i) {
            std::snprintf(buf, sizeof buf, "%s%u", i == 0 ? "" : ", ", static_cast<unsigned>(g.ids[i]));
            out += buf;
        }
        out += "\n";
    }
    return out;
}

}  // namespace mce::editor
