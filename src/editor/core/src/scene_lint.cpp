#include "mce/editor/scene_lint.h"

#include <cmath>
#include <cstdio>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/refl/type.h"

namespace mce::editor {

namespace {

bool finite3(const math::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

EntityId parent_id(const DocEntity& e) {
    const DocComponent* c = e.find(refl::type_of<ChildOf>());
    return c == nullptr ? kNoEntity : doc_entity_id(static_cast<const ChildOf*>(c->data())->parent);
}

const char* describe(LintKind k) {
    switch (k) {
        case LintKind::EmptyName: return "no name";
        case LintKind::DuplicateSiblingName: return "duplicate name among its siblings";
        case LintKind::ZeroScale: return "scale of zero on an axis";
        case LintKind::NonFiniteTransform: return "position, rotation or scale is not a number";
        case LintKind::MissingParent: return "parent does not exist";
    }
    return "";
}

}  // namespace

Array<LintFinding> lint_scene(const SceneDocument& doc) {
    Array<LintFinding> out;
    auto add = [&out](const DocEntity& e, LintKind kind) {
        LintFinding f;
        f.id = e.id;
        f.kind = kind;
        f.name = e.name;
        out.push_back(f);
    };
    const Span<const DocEntity> all = doc.entities();
    for (usize i = 0; i < all.size(); ++i) {
        const DocEntity& e = all[i];
        if (e.name.view().empty()) add(e, LintKind::EmptyName);

        const EntityId parent = parent_id(e);
        if (parent != kNoEntity && (parent == e.id || doc.find(parent) == nullptr)) add(e, LintKind::MissingParent);

        // A prefab instance keeps only the fields it overrides: the transform it shows is not in this document.
        if (const DocComponent* t = e.find(refl::type_of<LocalTransform>())) {
            const auto& lt = *static_cast<const LocalTransform*>(t->data());
            const bool finite = finite3(lt.position) && finite3(lt.scale) && std::isfinite(lt.rotation.x) &&
                                std::isfinite(lt.rotation.y) && std::isfinite(lt.rotation.z) && std::isfinite(lt.rotation.w);
            if (!finite) add(e, LintKind::NonFiniteTransform);
            else if (lt.scale.x == 0.0f || lt.scale.y == 0.0f || lt.scale.z == 0.0f) add(e, LintKind::ZeroScale);
        }

        if (e.name.view().empty()) continue;
        for (usize j = 0; j < i; ++j) {
            if (all[j].name.view() == e.name.view() && parent_id(all[j]) == parent) {
                add(e, LintKind::DuplicateSiblingName);
                break;
            }
        }
    }
    return out;
}

cook::String format_lint(Span<const LintFinding> findings) {
    cook::String out;
    for (const LintFinding& f : findings) {
        if (!out.empty()) out.append('\n');
        out.append(f.name.view());
        char buf[24];
        std::snprintf(buf, sizeof buf, " (id %u): ", static_cast<unsigned>(f.id));
        out.append(StringView(buf));
        out.append(StringView(describe(f.kind)));
    }
    return out;
}

}  // namespace mce::editor
