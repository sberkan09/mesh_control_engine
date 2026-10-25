#include "mce/editor/scene_find.h"

#include <cstdio>

#include "mce/ecs/hierarchy.h"
#include "mce/refl/type.h"

namespace mce::editor {

namespace {

EntityId parent_id(const SceneDocument& doc, const DocEntity& e) {
    const DocComponent* c = e.find(refl::type_of<ChildOf>());
    if (c == nullptr) return kNoEntity;
    const EntityId p = doc_entity_id(static_cast<const ChildOf*>(c->data())->parent);
    return p != e.id && doc.find(p) != nullptr ? p : kNoEntity;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool contains_ignoring_case(StringView text, StringView part) {
    if (part.size() > text.size()) return false;
    for (usize i = 0; i + part.size() <= text.size(); ++i) {
        usize k = 0;
        while (k < part.size() && lower(text[i + k]) == lower(part[k])) ++k;
        if (k == part.size()) return true;
    }
    return false;
}

// `Room/Wall/Torch`: the names from the root down, with a guard so a ChildOf cycle ends the walk.
cook::String path_of(const SceneDocument& doc, const DocEntity& e) {
    Array<const DocEntity*> chain;
    chain.push_back(&e);
    EntityId p = parent_id(doc, e);
    for (usize guard = 0; p != kNoEntity && guard <= doc.size(); ++guard) {
        const DocEntity* pe = doc.find(p);
        if (pe == nullptr) break;
        chain.push_back(pe);
        p = parent_id(doc, *pe);
    }
    cook::String out;
    for (usize i = chain.size(); i-- > 0;) {
        out.append(chain[i]->name.view());
        if (i > 0) out.append('/');
    }
    return out;
}

}  // namespace

Array<FindHit> find_entities(const SceneDocument& doc, const FindQuery& q) {
    Array<FindHit> hits;
    for (const DocEntity& e : doc.entities()) {
        if (!q.name.empty() && !contains_ignoring_case(e.name.view(), q.name)) continue;
        if (q.component != nullptr && e.find(q.component) == nullptr) continue;
        if (q.prefab_instances_only && e.prefab == asset::Guid{}) continue;
        FindHit h;
        h.id = e.id;
        h.name = e.name;
        h.path = path_of(doc, e);
        hits.push_back(static_cast<FindHit&&>(h));
    }
    return hits;
}

cook::String format_hits(Span<const FindHit> hits) {
    if (hits.empty()) {
        cook::String none;
        none.append(StringView("no matches"));
        return none;
    }
    cook::String out;
    for (usize i = 0; i < hits.size(); ++i) {
        if (i > 0) out.append('\n');
        out.append(hits[i].path.view());
        char buf[32];
        std::snprintf(buf, sizeof buf, " (id %u)", static_cast<unsigned>(hits[i].id));
        out.append(StringView(buf));
    }
    return out;
}

}  // namespace mce::editor
