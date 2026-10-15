// Animation editing (ROADMAP M-CINEMA step 3; animgraph_editor.h).

#include "mce/editor/animgraph_editor.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/kit/kit3d.h"
#include "mce/string_id.h"

namespace mce::editor {

using cook::AnimGraphDocState;
using cook::AnimGraphDocTransition;
using cook::AnimGraphDocument;

namespace {

constexpr f32 kGridX = 220.0f;
constexpr f32 kGridY = 110.0f;
constexpr u32 kGridColumns = 4;

const refl::TypeRecord& animator_type() { return refl::type_of<kit::Animator>(); }

}  // namespace

// ---------------------------------------------------------------- clips

bool mesh_clips(EditorSession& session, asset::Guid mesh, Array<MeshClipInfo>& out) {
    out.clear();
    asset::AssetSystem* assets = session.project().mounted();
    if (!assets || mesh.is_null()) return false;
    const asset::AssetKey key = assets->find(mesh);
    if (key == asset::kInvalidAssetKey) return false;
    Result<asset::AssetHandle<asset::Mesh>> h = assets->load_sync<asset::Mesh>(key);
    if (!h) return false;
    bool ok = false;
    if (Result<asset::MeshView> v = asset::MeshView::read(assets->resolve(h.value()))) {
        ok = true;
        for (u32 i = 0; i < v.value().clips.size(); ++i) {
            MeshClipInfo c;
            c.name.assign(v.value().clip_name(i));
            c.duration = v.value().clips[i].duration;
            out.push_back(c);
        }
    }
    assets->release(asset::AnyHandle(h.value()));
    return ok;
}

// ---------------------------------------------------------------- the document

bool AnimGraphEditor::read_text(StringView text, AnimGraphDocument& out) {
    cook::JsonDoc doc;
    cook::JsonError jerr;
    if (!doc.parse(text, jerr) || !doc.root().is_object()) {
        error_ = cook::String("not a JSON object");
        return false;
    }
    cook::DiagList diags;
    if (cook::read_animgraph_document(doc.root(), rel_.view(), out, diags)) return true;
    error_ = cook::String();
    u32 shown = 0;
    for (const cook::Diagnostic& d : diags.all()) {
        if (d.severity != cook::Severity::Error) continue;
        if (shown++ == 3) break;
        if (!error_.empty()) error_ += "; ";
        error_ += d.message.view();
    }
    if (error_.empty()) error_ = cook::String("the graph does not read");
    return false;
}

cook::String AnimGraphEditor::text() const {
    cook::String t;
    cook::write_animgraph_document(doc_, guid_, t);
    return t;
}

void AnimGraphEditor::set_doc(const AnimGraphDocument& d) {
    doc_ = d;
    if (state_ >= doc_.states.size()) state_ = kNoState;
    if (transition_ >= doc_.transitions.size()) transition_ = kNoTransition;
    layout();
    ++revision_;
}

void AnimGraphEditor::layout() {
    layout_.resize(doc_.states.size() * 2, 0.0f);
    u32 free = 0;
    for (u32 i = 0; i < doc_.states.size(); ++i) {
        const AnimGraphDocState& s = doc_.states[i];
        if (s.has_pos) {
            layout_[i * 2] = s.pos[0];
            layout_[i * 2 + 1] = s.pos[1];
            continue;
        }
        layout_[i * 2] = 40.0f + kGridX * static_cast<f32>(free % kGridColumns);
        layout_[i * 2 + 1] = 40.0f + kGridY * static_cast<f32>(free / kGridColumns);
        ++free;
    }
}

void AnimGraphEditor::position(u32 i, f32& x, f32& y) const {
    x = y = 0.0f;
    if (i * 2 + 1 >= layout_.size()) return;
    x = layout_[i * 2];
    y = layout_[i * 2 + 1];
}

bool AnimGraphEditor::open(EditorSession& session, asset::Guid guid) {
    if (guid == guid_ && dirty_) return true;
    const ProjectAsset* a = session.project().find(guid);
    if (!a || a->type != asset::ChunkType::AnimGraph) {
        error_ = cook::String("not an animation graph (.mcanim)");
        return false;
    }
    cook::String file;
    if (!cook::fs::read_text(session.project().asset_path(a->path.view()).view(), file)) {
        error_ = cook::String("the graph could not be read");
        return false;
    }
    const cook::String rel_before = rel_;
    rel_ = a->path;
    AnimGraphDocument d;
    if (!read_text(file.view(), d)) {
        rel_ = rel_before;
        return false;
    }
    if (guid != guid_) {
        undo_.clear();
        redo_.clear();
        state_ = kNoState;
        transition_ = kNoTransition;
    }
    guid_ = guid;
    disk_ = file;
    dirty_ = false;
    error_ = cook::String();
    set_doc(d);
    return true;
}

void AnimGraphEditor::close() {
    guid_ = {};
    rel_ = cook::String();
    disk_ = cook::String();
    doc_ = AnimGraphDocument();
    layout_.clear();
    state_ = kNoState;
    transition_ = kNoTransition;
    dirty_ = false;
    undo_.clear();
    redo_.clear();
    ++revision_;
}

Result<asset::Guid> AnimGraphEditor::create(EditorSession& session, StringView rel_path, StringView clip) {
    constexpr const char* kWhere = "editor.animgraph_create";
    if (!session.project().is_open()) return Error(Errc::InvalidArgument, kWhere, "no project is open");
    if (rel_path.size() < 8 || rel_path.substr(rel_path.size() - 7) != ".mcanim")
        return Error(Errc::InvalidArgument, kWhere, "a graph's path ends in .mcanim");
    if (cook::fs::exists(session.project().asset_path(rel_path).view())) return Error(Errc::AlreadyExists, kWhere, rel_path);
    AnimGraphDocument d;
    AnimGraphDocState s;
    s.name.assign("idle");
    s.clip.assign(clip.empty() ? StringView("Idle") : clip);
    s.pos[0] = 40.0f;
    s.pos[1] = 40.0f;
    s.has_pos = true;
    d.states.push_back(s);
    const asset::Guid guid = cook::generate_guid();
    cook::String t;
    cook::write_animgraph_document(d, guid, t);
    MCE_TRY(session.write_asset(rel_path, t.view()));
    if (!open(session, guid)) return Error(Errc::ImportFailed, kWhere, "the new graph did not cook (see the log)");
    return guid;
}

bool AnimGraphEditor::sync(EditorSession& session) {
    if (!is_open() || dirty_) return false;
    const ProjectAsset* a = session.project().find(guid_);
    if (!a) {
        close();
        return true;
    }
    cook::String file;
    if (!cook::fs::read_text(session.project().asset_path(a->path.view()).view(), file)) return false;
    if (file.view() == disk_.view() && a->path.view() == rel_.view()) return false;
    rel_ = a->path;
    disk_ = file;
    AnimGraphDocument d;
    if (!read_text(file.view(), d)) return false;
    error_ = cook::String();
    set_doc(d);
    return true;
}

void AnimGraphEditor::select_state(u32 i) {
    state_ = i < doc_.states.size() ? i : kNoState;
    transition_ = kNoTransition;
}

void AnimGraphEditor::select_transition(u32 i) {
    transition_ = i < doc_.transitions.size() ? i : kNoTransition;
    state_ = kNoState;
}

// ---------------------------------------------------------------- edits

bool AnimGraphEditor::apply(const AnimGraphDocument& in) {
    if (!is_open()) {
        error_ = cook::String("no graph is open");
        return false;
    }
    // Every state is written with where it is drawn, so the layout survives the round trip.
    AnimGraphDocument next = in;
    for (u32 i = 0; i < next.states.size(); ++i)
        if (!next.states[i].has_pos) {
            position(i, next.states[i].pos[0], next.states[i].pos[1]);
            next.states[i].has_pos = true;
        }
    cook::String t;
    cook::write_animgraph_document(next, guid_, t);
    AnimGraphDocument back;
    if (!read_text(t.view(), back)) return false;
    error_ = cook::String();
    const cook::String before = text();
    if (t.view() == before.view()) return true;
    if (!dirty_) {
        undo_.push_back(before);
        if (undo_.size() > kMaxUndo) undo_.erase(0);
    }
    redo_.clear();
    dirty_ = true;
    set_doc(back);
    return true;
}

cook::String AnimGraphEditor::unique_state_name(StringView base) const {
    cook::String name(base.empty() ? StringView("state") : base);
    for (u32 n = 2;; ++n) {
        bool taken = false;
        for (const AnimGraphDocState& s : doc_.states)
            if (s.name.view() == name.view()) taken = true;
        if (!taken) return name;
        name = cook::String(base.empty() ? StringView("state") : base);
        appendf(name, " %u", n);
    }
}

bool AnimGraphEditor::add_state(StringView name, StringView clip, f32 x, f32 y) {
    AnimGraphDocument next = doc_;
    AnimGraphDocState s;
    s.name = unique_state_name(name);
    s.clip.assign(clip.empty() ? StringView("Idle") : clip);
    s.pos[0] = x;
    s.pos[1] = y;
    s.has_pos = true;
    next.states.push_back(s);
    if (!apply(next)) return false;
    select_state(static_cast<u32>(doc_.states.size()) - 1);
    return true;
}

bool AnimGraphEditor::remove_state(u32 i) {
    if (i >= doc_.states.size()) return false;
    if (doc_.states.size() == 1) {
        error_ = cook::String("a graph keeps at least one state");
        return false;
    }
    AnimGraphDocument next = doc_;
    // Keep the drawn positions of the states that stay.
    for (u32 k = 0; k < next.states.size(); ++k)
        if (!next.states[k].has_pos) {
            position(k, next.states[k].pos[0], next.states[k].pos[1]);
            next.states[k].has_pos = true;
        }
    next.states.erase(i);
    Array<AnimGraphDocTransition> kept;
    for (AnimGraphDocTransition t : next.transitions) {
        if (t.from == i || t.to == i) continue;
        if (t.from != cook::kAnimGraphAny && t.from > i) --t.from;
        if (t.to > i) --t.to;
        kept.push_back(t);
    }
    next.transitions = kept;
    next.start = next.start == i ? 0 : next.start > i ? next.start - 1 : next.start;
    if (!apply(next)) return false;
    select_state(kNoState);
    return true;
}

bool AnimGraphEditor::set_state(u32 i, const AnimGraphDocState& s) {
    if (i >= doc_.states.size()) return false;
    AnimGraphDocument next = doc_;
    next.states[i] = s;
    if (!apply(next)) return false;
    select_state(i);
    return true;
}

bool AnimGraphEditor::move_state(u32 i, f32 x, f32 y) {
    if (i >= doc_.states.size()) return false;
    AnimGraphDocState s = doc_.states[i];
    s.pos[0] = std::round(x);
    s.pos[1] = std::round(y);
    s.has_pos = true;
    return set_state(i, s);
}

bool AnimGraphEditor::set_start(u32 i) {
    if (i >= doc_.states.size()) return false;
    AnimGraphDocument next = doc_;
    next.start = i;
    return apply(next);
}

bool AnimGraphEditor::add_transition(u32 from, u32 to) {
    if (to >= doc_.states.size() || (from != cook::kAnimGraphAny && from >= doc_.states.size())) return false;
    AnimGraphDocTransition t;
    t.from = from;
    t.to = to;
    if (from != cook::kAnimGraphAny && !doc_.states[from].loop) {
        t.on_finish = true;
    } else if (!doc_.params.empty()) {
        cook::AnimGraphDocCondition c;
        c.param = 0;
        c.op = asset::AnimCompare::Greater;
        c.value = 0.5f;
        t.when.push_back(c);
    } else {
        error_ = cook::String("a transition waits on a parameter or on a one-shot's end: add a parameter first");
        return false;
    }
    AnimGraphDocument next = doc_;
    next.transitions.push_back(t);
    if (!apply(next)) return false;
    select_transition(static_cast<u32>(doc_.transitions.size()) - 1);
    return true;
}

bool AnimGraphEditor::remove_transition(u32 i) {
    if (i >= doc_.transitions.size()) return false;
    AnimGraphDocument next = doc_;
    next.transitions.erase(i);
    if (!apply(next)) return false;
    select_transition(kNoTransition);
    return true;
}

bool AnimGraphEditor::set_transition(u32 i, const AnimGraphDocTransition& t) {
    if (i >= doc_.transitions.size()) return false;
    AnimGraphDocument next = doc_;
    next.transitions[i] = t;
    if (!apply(next)) return false;
    select_transition(i);
    return true;
}

bool AnimGraphEditor::add_param(StringView name) {
    if (doc_.params.size() >= asset::kAnimGraphMaxParams) {
        error_ = cook::String("a graph has up to four parameters");
        return false;
    }
    AnimGraphDocument next = doc_;
    next.params.push_back(cook::String(name));
    return apply(next);
}

bool AnimGraphEditor::rename_param(u32 i, StringView name) {
    if (i >= doc_.params.size()) return false;
    AnimGraphDocument next = doc_;
    next.params[i].assign(name);
    return apply(next);
}

bool AnimGraphEditor::remove_param(u32 i) {
    if (i >= doc_.params.size()) return false;
    for (const AnimGraphDocState& s : doc_.states)
        if (s.is_blend() && s.blend_param == i) {
            error_ = cook::String("a blend state uses this parameter");
            return false;
        }
    for (const AnimGraphDocTransition& t : doc_.transitions)
        for (const cook::AnimGraphDocCondition& c : t.when)
            if (c.param == i) {
                error_ = cook::String("a transition's condition uses this parameter");
                return false;
            }
    AnimGraphDocument next = doc_;
    next.params.erase(i);
    for (AnimGraphDocState& s : next.states)
        if (s.is_blend() && s.blend_param > i) --s.blend_param;
    for (AnimGraphDocTransition& t : next.transitions)
        for (cook::AnimGraphDocCondition& c : t.when)
            if (c.param > i) --c.param;
    return apply(next);
}

bool AnimGraphEditor::set_name(StringView name) {
    AnimGraphDocument next = doc_;
    next.name.assign(name);
    return apply(next);
}

Result<void> AnimGraphEditor::save(EditorSession& session) {
    if (!dirty_) return ok();
    const cook::String t = text();
    MCE_TRY(session.write_asset(rel_.view(), t.view()));
    disk_ = t;
    dirty_ = false;
    return ok();
}

bool AnimGraphEditor::undo(EditorSession& session) {
    if (undo_.empty()) return false;
    AnimGraphDocument d;
    if (!read_text(undo_.back().view(), d)) return false;
    redo_.push_back(text());
    undo_.pop_back();
    set_doc(d);
    dirty_ = true;
    return static_cast<bool>(save(session));
}

bool AnimGraphEditor::redo(EditorSession& session) {
    if (redo_.empty()) return false;
    AnimGraphDocument d;
    if (!read_text(redo_.back().view(), d)) return false;
    if (dirty_) {
        if (Result<void> r = save(session); !r) return false;
    }
    undo_.push_back(text());
    redo_.pop_back();
    set_doc(d);
    dirty_ = true;
    return static_cast<bool>(save(session));
}

void AnimGraphEditor::users(const EditorScene& scene, Array<EntityId>& out) const {
    out.clear();
    if (!is_open()) return;
    for (const DocEntity& e : scene.doc().entities()) {
        const DocComponent* c = e.find(animator_type());
        if (c && static_cast<const kit::Animator*>(c->data())->graph.guid == guid_) out.push_back(e.id);
    }
}

// ---------------------------------------------------------------- preview

bool AnimPreview::running(const EditorSession& session) const {
    const World* w = session.play().world();
    return session.play().active() && w && !entity_.is_null() && w->alive(entity_);
}

bool AnimPreview::start(EditorSession& session, EntityId entity) {
    why_ = cook::String();
    const DocEntity* e = session.scene().doc().find(entity);
    if (!e || !e->find(animator_type())) {
        why_ = cook::String("pick an entity with an Animator");
        return false;
    }
    usize index = 0;
    while (index < session.scene().doc().size() && session.scene().doc().at(index).id != entity) ++index;
    PlaySession& play = session.play();
    if (!play.active()) {
        if (!session.enter_play()) {
            why_ = cook::String("play does not start (see the Cook panel)");
            return false;
        }
        play.pause();
    }
    if (index >= play.entities().size()) {
        why_ = cook::String("the entity is not in the play world");
        session.stop_play();
        return false;
    }
    doc_entity_ = entity;
    entity_ = play.entities()[index];
    const kit::Animator* a = play.world()->try_get<kit::Animator>(entity_);
    if (!a) {
        why_ = cook::String("the entity has no Animator in play");
        session.stop_play();
        entity_ = Entity::null;
        return false;
    }
    graph_ = a->graph;
    return true;
}

void AnimPreview::stop(EditorSession& session) {
    if (session.play().active() && !entity_.is_null()) session.stop_play();
    entity_ = Entity::null;
    doc_entity_ = kNoEntity;
    graph_ = {};
}

bool AnimPreview::show_clip(EditorSession& session, StringView clip, f32 seconds, bool loop) {
    if (!running(session)) return false;
    World& w = *session.play().world();
    kit::Animator* a = w.try_get_mut<kit::Animator>(entity_);
    if (!a) return false;
    session.play().pause();
    a->graph = {};
    a->clip = FixedString<32>(clip);
    a->playing = clip.empty() ? 0 : StringId(clip).value;
    a->previous = 0;
    a->blend_clip = 0;
    a->blend = 1.0f;
    a->blend_weight = 0.0f;
    a->time = seconds < 0.0f ? 0.0f : seconds;
    a->loop = loop;
    a->finished = false;
    a->restart = false;
    w.touch<kit::Animator>(entity_);
    return true;
}

bool AnimPreview::run_graph(EditorSession& session, const f32 params[4]) {
    if (!running(session)) return false;
    World& w = *session.play().world();
    kit::Animator* a = w.try_get_mut<kit::Animator>(entity_);
    if (!a) return false;
    if (a->graph.guid != graph_.guid) {
        a->graph = graph_;
        a->state = FixedString<32>();
        a->restart = true;
    }
    a->params = math::vec4{params[0], params[1], params[2], params[3]};
    w.touch<kit::Animator>(entity_);
    session.play().resume();
    return true;
}

StringView AnimPreview::state(const EditorSession& session) const {
    if (!running(session)) return {};
    const kit::Animator* a = session.play().world()->try_get<kit::Animator>(entity_);
    return a ? a->state.view() : StringView();
}

}  // namespace mce::editor
