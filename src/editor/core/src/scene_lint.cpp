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

struct RefScan {
    const SceneDocument* doc = nullptr;
    bool dangling = false;
};

void check_ref(Entity* field, void* user) {
    auto& scan = *static_cast<RefScan*>(user);
    const EntityId id = doc_entity_id(*field);
    if (id != kNoEntity && scan.doc->find(id) == nullptr) scan.dangling = true;
}

// True when a component other than ChildOf (MissingParent covers that one) refers to an entity the scene lacks.
bool has_dangling_ref(const SceneDocument& doc, const DocEntity& e) {
    RefScan scan;
    scan.doc = &doc;
    for (const DocComponent& comp : e.components) {
        if (comp.type == &refl::type_of<ChildOf>()) continue;
        // The visitor only reads; the document hands out its bytes as const.
        visit_entity_refs(*comp.type, const_cast<void*>(comp.data()), check_ref, &scan);
        if (scan.dangling) return true;
    }
    return false;
}

const char* describe(LintKind k) {
    switch (k) {
        case LintKind::EmptyName: return "no name";
        case LintKind::DuplicateSiblingName: return "duplicate name among its siblings";
        case LintKind::ZeroScale: return "scale of zero on an axis";
        case LintKind::NonFiniteTransform: return "position, rotation or scale is not a number";
        case LintKind::MissingParent: return "parent does not exist";
        case LintKind::DanglingRef: return "refers to an entity the scene lacks";
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

        // A prefab instance's references may name entities of the prefab, which this document does not hold.
        if (e.prefab == asset::Guid{} && has_dangling_ref(doc, e)) add(e, LintKind::DanglingRef);

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

namespace {

void null_missing_ref(Entity* field, void* user) {
    const auto& doc = *static_cast<const SceneDocument*>(user);
    const EntityId id = doc_entity_id(*field);
    if (id != kNoEntity && doc.find(id) == nullptr) *field = Entity::null;
}

void fix_transform(LocalTransform& t) {
    if (!finite3(t.position)) t.position = {};
    if (!finite3(t.scale)) t.scale = {1.0f, 1.0f, 1.0f};
    if (!(std::isfinite(t.rotation.x) && std::isfinite(t.rotation.y) && std::isfinite(t.rotation.z) &&
          std::isfinite(t.rotation.w)))
        t.rotation = {};
    if (t.scale.x == 0.0f) t.scale.x = 1.0f;
    if (t.scale.y == 0.0f) t.scale.y = 1.0f;
    if (t.scale.z == 0.0f) t.scale.z = 1.0f;
}

bool name_taken(const SceneDocument& doc, EntityId parent, StringView name, EntityId except) {
    for (const DocEntity& o : doc.entities())
        if (o.id != except && o.name.view() == name && parent_id(o) == parent) return true;
    return false;
}

}  // namespace

usize fix_scene_lint(SceneDocument& doc) {
    const Array<LintFinding> found = lint_scene(doc);
    if (found.empty()) return 0;
    auto flagged = [&found](EntityId id, LintKind kind) {
        for (const LintFinding& f : found)
            if (f.id == id && f.kind == kind) return true;
        return false;
    };
    // Names first and the parent link next: sibling groups depend on both.
    for (DocEntity& e : doc.entities_mut()) {
        if (flagged(e.id, LintKind::EmptyName)) {
            char buf[24];
            std::snprintf(buf, sizeof buf, "Entity %u", static_cast<unsigned>(e.id));
            e.name.assign(StringView(buf));
        }
        if (flagged(e.id, LintKind::MissingParent)) (void)doc.remove_component(e.id, refl::type_of<ChildOf>());
    }
    for (DocEntity& e : doc.entities_mut()) {
        if (flagged(e.id, LintKind::DanglingRef)) {
            for (DocComponent& comp : e.components) {
                if (comp.type == &refl::type_of<ChildOf>()) continue;
                visit_entity_refs(*comp.type, comp.data(), null_missing_ref, &doc);
            }
        }
        if (flagged(e.id, LintKind::NonFiniteTransform) || flagged(e.id, LintKind::ZeroScale)) {
            if (DocComponent* t = e.find(refl::type_of<LocalTransform>()))
                fix_transform(*static_cast<LocalTransform*>(t->data()));
        }
    }
    // Duplicates last, in document order: the first holder keeps the name, a later one takes the first free " (n)".
    for (DocEntity& e : doc.entities_mut()) {
        if (!flagged(e.id, LintKind::DuplicateSiblingName)) continue;
        const EntityId parent = parent_id(e);
        const cook::String base(e.name.view());
        for (u32 n = 2;; ++n) {
            char suffix[24];
            std::snprintf(suffix, sizeof suffix, " (%u)", static_cast<unsigned>(n));
            const usize room = e.name.capacity() - StringView(suffix).size();
            FixedString<64> candidate(base.view().size() <= room ? base.view() : base.view().substr(0, room));
            candidate.append(StringView(suffix));
            if (name_taken(doc, parent, candidate.view(), e.id)) continue;
            e.name = candidate;
            break;
        }
    }
    return found.size();
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
