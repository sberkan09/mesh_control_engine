#include "mce/editor/scene_stats.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/refl/type.h"

namespace mce::editor {

namespace {

EntityId parent_id(const SceneDocument& doc, const DocEntity& e) {
    const DocComponent* c = e.find(refl::type_of<ChildOf>());
    if (c == nullptr) return kNoEntity;
    const EntityId p = doc_entity_id(static_cast<const ChildOf*>(c->data())->parent);
    return p != e.id && doc.find(p) != nullptr ? p : kNoEntity;
}

bool before(const ComponentUse& a, const ComponentUse& b) {
    if (a.entities != b.entities) return a.entities > b.entities;
    return std::strcmp(a.type->name, b.type->name) < 0;
}

void append_count(cook::String& out, u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u", static_cast<unsigned>(v));
    out.append(StringView(buf));
}

void append_float(cook::String& out, f32 v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
    out.append(StringView(buf));
}

void append_vec(cook::String& out, const math::vec3& v) {
    out.append('(');
    append_float(out, v.x);
    out.append(StringView(", "));
    append_float(out, v.y);
    out.append(StringView(", "));
    append_float(out, v.z);
    out.append(')');
}

}  // namespace

SceneStats scene_stats(const SceneDocument& doc) {
    SceneStats s;
    s.entities = static_cast<u32>(doc.size());
    for (const DocEntity& e : doc.entities()) {
        // Depth: the parents above, with a guard so a ChildOf cycle ends the walk.
        u32 depth = 0;
        EntityId p = parent_id(doc, e);
        for (usize guard = 0; p != kNoEntity && guard <= doc.size(); ++guard) {
            ++depth;
            const DocEntity* pe = doc.find(p);
            p = pe == nullptr ? kNoEntity : parent_id(doc, *pe);
        }
        if (parent_id(doc, e) == kNoEntity) ++s.roots;
        s.max_depth = std::max(s.max_depth, depth);
        if (!(e.prefab == asset::Guid{})) ++s.prefab_instances;

        for (const DocComponent& c : e.components) {
            bool found = false;
            for (ComponentUse& use : s.components)
                if (use.type == c.type) {
                    ++use.entities;
                    found = true;
                    break;
                }
            if (!found) s.components.push_back({c.type, 1});
        }
        if (const DocComponent* t = e.find(refl::type_of<LocalTransform>())) {
            const math::vec3 pos = static_cast<const LocalTransform*>(t->data())->position;
            if (!s.has_bounds) {
                s.min = s.max = pos;
                s.has_bounds = true;
            } else {
                s.min = {std::min(s.min.x, pos.x), std::min(s.min.y, pos.y), std::min(s.min.z, pos.z)};
                s.max = {std::max(s.max.x, pos.x), std::max(s.max.y, pos.y), std::max(s.max.z, pos.z)};
            }
        }
    }
    std::sort(s.components.begin(), s.components.end(), before);
    return s;
}

cook::String format_stats(const SceneStats& s) {
    cook::String out;
    out.append(StringView("entities: "));
    append_count(out, s.entities);
    out.append(StringView(" ("));
    append_count(out, s.roots);
    out.append(StringView(s.roots == 1 ? " root, depth " : " roots, depth "));
    append_count(out, s.max_depth);
    if (s.prefab_instances > 0) {
        out.append(StringView(", "));
        append_count(out, s.prefab_instances);
        out.append(StringView(s.prefab_instances == 1 ? " prefab instance" : " prefab instances"));
    }
    out.append(')');
    if (s.has_bounds) {
        out.append(StringView("\nbounds: "));
        append_vec(out, s.min);
        out.append(StringView(" to "));
        append_vec(out, s.max);
    }
    for (const ComponentUse& u : s.components) {
        out.append('\n');
        out.append(StringView(u.type->name));
        out.append(StringView(": "));
        append_count(out, u.entities);
    }
    return out;
}

}  // namespace mce::editor
