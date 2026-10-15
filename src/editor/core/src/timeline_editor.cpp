// Timeline authoring (ROADMAP M-CINEMA step 2; timeline_editor.h).

#include "mce/editor/timeline_editor.h"

#include <cmath>
#include <cstring>

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/ecs/transform.h"
#include "mce/kit/timeline.h"
#include "mce/math/math.h"

namespace mce::editor {

using cook::TimelineDocKey;
using cook::TimelineDocTrack;
using cook::TimelineDocTrackType;
using T = TimelineDocTrackType;

namespace {

bool same_time(f32 a, f32 b) { return std::fabs(a - b) < TimelineEditor::kSameTime; }

const f32* channel_values(const TimelineDocKey& k, const TimelineDocTrack& t, u8 channel, u32& count, bool& quat) {
    if (t.type == T::Property) {
        count = t.value_count;
        quat = t.quat;
        return k.value;
    }
    quat = channel == cook::TimelineChannel::Rotation;
    count = quat ? 4 : 3;
    return channel == cook::TimelineChannel::Position ? k.position : quat ? k.rotation : k.scale;
}

bool keys_channel(const TimelineDocKey& k, const TimelineDocTrack& t, u8 channel) {
    return t.type == T::Property || (k.channels & channel) != 0;
}

const refl::TypeRecord& player_type() { return refl::type_of<kit::TimelinePlayer>(); }

const kit::TimelinePlayer* player_of(const EditorScene& scene, EntityId id) {
    const DocEntity* e = scene.doc().find(id);
    const DocComponent* c = e ? e->find(player_type()) : nullptr;
    return c ? static_cast<const kit::TimelinePlayer*>(c->data()) : nullptr;
}

u32 floats_of(refl::FieldKind k) {
    using K = refl::FieldKind;
    switch (k) {
        case K::F32: return 1;
        case K::Vec2: return 2;
        case K::Vec3: return 3;
        case K::Vec4:
        case K::Color:
        case K::Quat: return 4;
        default: return 0;
    }
}

}  // namespace

// ---------------------------------------------------------------- helpers

void sample_timeline_track(const TimelineDocTrack& track, f32 t, f32 out[4], u8 channel) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (track.type == T::Transform && channel == cook::TimelineChannel::Rotation) out[3] = 1.0f;
    if (track.type == T::Transform && channel == cook::TimelineChannel::Scale) out[0] = out[1] = out[2] = 1.0f;
    if (track.type != T::Property && track.type != T::Transform) return;
    const TimelineDocKey* a = nullptr;
    const TimelineDocKey* b = nullptr;
    for (const TimelineDocKey& k : track.keys) {
        if (!keys_channel(k, track, channel)) continue;
        if (k.t <= t || !a) {
            if (k.t <= t || !a) a = &k;
            if (k.t > t) break;
        } else {
            b = &k;
            break;
        }
    }
    if (!a) return;
    u32 n = 0;
    bool quat = false;
    const f32* va = channel_values(*a, track, channel, n, quat);
    if (!b || !(b->t > a->t) || t <= a->t) {
        std::memcpy(out, va, sizeof(f32) * n);
        return;
    }
    const f32* vb = channel_values(*b, track, channel, n, quat);
    const f32 u = kit::timeline_eased(a->ease, (t - a->t) / (b->t - a->t));
    if (quat) {
        const math::quat q = math::slerp(math::normalize(math::quat{va[0], va[1], va[2], va[3]}),
                                         math::normalize(math::quat{vb[0], vb[1], vb[2], vb[3]}), u);
        out[0] = q.x;
        out[1] = q.y;
        out[2] = q.z;
        out[3] = q.w;
        return;
    }
    for (u32 i = 0; i < n; ++i) out[i] = va[i] + (vb[i] - va[i]) * u;
}

f32 snap_time(f32 t, f32 step) {
    if (t < 0.0f) t = 0.0f;
    if (!(step > 0.0f)) return t;
    return std::round(t / step) * step;
}

// ---------------------------------------------------------------- the document

bool TimelineEditor::read_text(StringView text, cook::TimelineDocument& out) {
    cook::JsonDoc doc;
    cook::JsonError jerr;
    if (!doc.parse(text, jerr) || !doc.root().is_object()) {
        error_ = cook::String("not a JSON object");
        return false;
    }
    cook::DiagList diags;
    if (cook::read_timeline_document(doc.root(), rel_.view(), out, diags)) return true;
    error_ = cook::String();
    u32 shown = 0;
    for (const cook::Diagnostic& d : diags.all()) {
        if (d.severity != cook::Severity::Error) continue;
        if (shown++ == 3) break;
        if (!error_.empty()) error_ += "; ";
        error_ += d.message.view();
    }
    if (error_.empty()) error_ = cook::String("the timeline does not read");
    return false;
}

cook::String TimelineEditor::text() const {
    cook::String t;
    cook::write_timeline_document(doc_, guid_, t);
    return t;
}

void TimelineEditor::set_doc(const cook::TimelineDocument& d) {
    doc_ = d;
    if (track_ >= doc_.tracks.size()) track_ = kNoTrack, key_ = kNoKey;
    if (track_ != kNoTrack && key_ >= doc_.tracks[track_].keys.size()) key_ = kNoKey;
    const f32 len = doc_.length();
    if (playhead_ > len) playhead_ = len;
    ++revision_;
}

bool TimelineEditor::open(EditorSession& session, asset::Guid guid) {
    if (guid == guid_ && dirty_) return true;
    const ProjectAsset* a = session.project().find(guid);
    if (!a || a->type != asset::ChunkType::Timeline) {
        error_ = cook::String("not a timeline (.mctimeline)");
        return false;
    }
    cook::String file;
    if (!cook::fs::read_text(session.project().asset_path(a->path.view()).view(), file)) {
        error_ = cook::String("the timeline could not be read");
        return false;
    }
    const cook::String rel_before = rel_;
    rel_ = a->path;
    cook::TimelineDocument d;
    if (!read_text(file.view(), d)) {
        rel_ = rel_before;
        return false;
    }
    if (guid != guid_) {
        undo_.clear();
        redo_.clear();
        track_ = kNoTrack;
        key_ = kNoKey;
        playhead_ = 0.0f;
    }
    guid_ = guid;
    disk_ = file;
    dirty_ = false;
    error_ = cook::String();
    set_doc(d);
    return true;
}

void TimelineEditor::close() {
    guid_ = {};
    rel_ = cook::String();
    disk_ = cook::String();
    doc_ = cook::TimelineDocument();
    track_ = kNoTrack;
    key_ = kNoKey;
    playhead_ = 0.0f;
    dirty_ = false;
    recording_ = false;
    undo_.clear();
    redo_.clear();
    ++revision_;
}

Result<asset::Guid> TimelineEditor::create(EditorSession& session, StringView rel_path, f32 duration) {
    constexpr const char* kWhere = "editor.timeline_create";
    if (!session.project().is_open()) return Error(Errc::InvalidArgument, kWhere, "no project is open");
    if (rel_path.size() < 12 || rel_path.substr(rel_path.size() - 11) != ".mctimeline")
        return Error(Errc::InvalidArgument, kWhere, "a timeline's path ends in .mctimeline");
    if (cook::fs::exists(session.project().asset_path(rel_path).view())) return Error(Errc::AlreadyExists, kWhere, rel_path);
    cook::TimelineDocument d;
    d.duration = duration > 0.0f ? duration : 5.0f;
    TimelineDocTrack start;
    start.type = T::Event;
    TimelineDocKey k;
    k.name.assign("start");
    start.keys.push_back(k);
    d.tracks.push_back(start);
    const asset::Guid guid = cook::generate_guid();
    cook::String t;
    cook::write_timeline_document(d, guid, t);
    MCE_TRY(session.write_asset(rel_path, t.view()));
    if (!open(session, guid)) return Error(Errc::ImportFailed, kWhere, "the new timeline did not cook (see the log)");
    return guid;
}

bool TimelineEditor::sync(EditorSession& session) {
    if (!is_open() || dirty_) return false;
    const ProjectAsset* a = session.project().find(guid_);
    if (!a) {  // deleted (or moved away from the catalog)
        close();
        return true;
    }
    cook::String file;
    if (!cook::fs::read_text(session.project().asset_path(a->path.view()).view(), file)) return false;
    if (file.view() == disk_.view() && a->path.view() == rel_.view()) return false;
    rel_ = a->path;
    disk_ = file;
    cook::TimelineDocument d;
    if (!read_text(file.view(), d)) return false;  // keeps the last good timeline; error_ says why
    error_ = cook::String();
    set_doc(d);
    return true;
}

void TimelineEditor::select(u32 track, u32 key) {
    track_ = track < doc_.tracks.size() ? track : kNoTrack;
    key_ = track_ != kNoTrack && key < doc_.tracks[track_].keys.size() ? key : kNoKey;
}

void TimelineEditor::set_playhead(f32 t) {
    const f32 len = doc_.length();
    playhead_ = t < 0.0f ? 0.0f : t > len ? len : t;
}

// ---------------------------------------------------------------- edits

bool TimelineEditor::apply(const cook::TimelineDocument& next) {
    if (!is_open()) {
        error_ = cook::String("no timeline is open");
        return false;
    }
    cook::String t;
    cook::write_timeline_document(next, guid_, t);
    cook::TimelineDocument back;
    if (!read_text(t.view(), back)) return false;
    error_ = cook::String();
    const cook::String before = text();
    cook::String after;
    cook::write_timeline_document(back, guid_, after);
    if (after.view() == before.view()) return true;
    if (!dirty_) {
        undo_.push_back(before);
        if (undo_.size() > kMaxUndo) undo_.erase(0);
    }
    redo_.clear();
    dirty_ = true;
    set_doc(back);
    return true;
}

u32 TimelineEditor::find_track(TimelineDocTrackType type, u8 target, StringView path) const {
    for (u32 i = 0; i < doc_.tracks.size(); ++i) {
        const TimelineDocTrack& t = doc_.tracks[i];
        if (t.type == type && t.target == target && t.path.view() == path) return i;
    }
    return kNoTrack;
}

u32 TimelineEditor::put_key(TimelineDocTrack& track, const TimelineDocKey& key) {
    for (u32 i = 0; i < track.keys.size(); ++i)
        if (same_time(track.keys[i].t, key.t)) {
            track.keys[i] = key;
            return i;
        }
    u32 at = 0;
    while (at < track.keys.size() && track.keys[at].t < key.t) ++at;
    track.keys.push_back(key);
    for (u32 i = static_cast<u32>(track.keys.size()) - 1; i > at; --i) {
        TimelineDocKey tmp = track.keys[i];
        track.keys[i] = track.keys[i - 1];
        track.keys[i - 1] = tmp;
    }
    return at;
}

bool TimelineEditor::add_track(TimelineDocTrackType type, u8 target, StringView path) {
    cook::TimelineDocument next = doc_;
    TimelineDocTrack t;
    t.type = type;
    t.target = type == T::Camera || type == T::Subtitle ? asset::kTimelineSelf : target;
    t.path.assign(path);
    if (type == T::Property || type == T::Activation) {
        cook::String why;
        if (!cook::resolve_timeline_path(t, why)) {
            error_ = why;
            return false;
        }
    }
    // A track starts with one key at the playhead: an empty track does not read back.
    TimelineDocKey k;
    k.t = playhead_;
    switch (type) {
        case T::Transform: k.channels = cook::TimelineChannel::Position; break;
        case T::Activation: k.duration = 1.0f; break;
        case T::Animation: k.name.assign("idle"); break;
        case T::Audio: break;
        case T::Event: k.name.assign("event"); break;
        case T::Subtitle:
            k.duration = 2.0f;
            k.name.assign("...");
            break;
        default: break;
    }
    if (type == T::Property && t.quat) k.value[3] = 1.0f;
    t.keys.push_back(k);
    next.tracks.push_back(t);
    if (type == T::Audio) {
        error_ = cook::String("an audio key needs a sound: add the track from a sound asset");
        return false;
    }
    if (!apply(next)) return false;
    select(static_cast<u32>(doc_.tracks.size()) - 1, 0);
    return true;
}

bool TimelineEditor::remove_track(u32 track) {
    if (track >= doc_.tracks.size()) return false;
    if (doc_.tracks.size() == 1) {
        error_ = cook::String("a timeline keeps at least one track");
        return false;
    }
    cook::TimelineDocument next = doc_;
    next.tracks.erase(track);
    if (!apply(next)) return false;
    select(kNoTrack);
    return true;
}

bool TimelineEditor::set_key_at(u32 track, f32 t, const TimelineDocKey& key) {
    if (track >= doc_.tracks.size()) return false;
    cook::TimelineDocument next = doc_;
    TimelineDocKey k = key;
    k.t = t < 0.0f ? 0.0f : t;
    const u32 at = put_key(next.tracks[track], k);
    if (!apply(next)) return false;
    select(track, at);
    return true;
}

bool TimelineEditor::add_key(u32 track, f32 t) {
    if (track >= doc_.tracks.size()) return false;
    const TimelineDocTrack& tr = doc_.tracks[track];
    TimelineDocKey k;
    // Start from the nearest key before (or the first): cue tracks copy their payload.
    for (const TimelineDocKey& e : tr.keys)
        if (e.t <= t || &e == &tr.keys[0]) k = e;
    if (tr.type == T::Property) sample_timeline_track(tr, t, k.value);
    if (tr.type == T::Transform) {
        k.channels = 0;
        for (const TimelineDocKey& e : tr.keys) k.channels |= e.channels;
        sample_timeline_track(tr, t, k.position, cook::TimelineChannel::Position);
        sample_timeline_track(tr, t, k.rotation, cook::TimelineChannel::Rotation);
        sample_timeline_track(tr, t, k.scale, cook::TimelineChannel::Scale);
    }
    return set_key_at(track, t, k);
}

bool TimelineEditor::remove_key(u32 track, u32 key) {
    if (track >= doc_.tracks.size() || key >= doc_.tracks[track].keys.size()) return false;
    if (doc_.tracks[track].keys.size() == 1) return remove_track(track);
    cook::TimelineDocument next = doc_;
    next.tracks[track].keys.erase(key);
    if (!apply(next)) return false;
    select(track, kNoKey);
    return true;
}

bool TimelineEditor::set_key(u32 track, u32 key, const TimelineDocKey& value) {
    if (track >= doc_.tracks.size() || key >= doc_.tracks[track].keys.size()) return false;
    cook::TimelineDocument next = doc_;
    TimelineDocTrack& tr = next.tracks[track];
    tr.keys.erase(key);
    TimelineDocKey k = value;
    if (k.t < 0.0f) k.t = 0.0f;
    // A key moved onto another one replaces it.
    const u32 at = put_key(tr, k);
    if (!apply(next)) return false;
    select(track, at);
    return true;
}

bool TimelineEditor::move_key(u32 track, u32 key, f32 t) {
    if (track >= doc_.tracks.size() || key >= doc_.tracks[track].keys.size()) return false;
    TimelineDocKey k = doc_.tracks[track].keys[key];
    k.t = t;
    return set_key(track, key, k);
}

bool TimelineEditor::set_duration(f32 seconds) {
    cook::TimelineDocument next = doc_;
    next.duration = seconds > 0.0f ? seconds : 0.0f;
    if (next.duration > 0.0f && next.duration < next.keys_end()) {
        error_ = cook::String("the duration ends before the last key");
        return false;
    }
    return apply(next);
}

bool TimelineEditor::set_name(StringView name) {
    cook::TimelineDocument next = doc_;
    next.name.assign(name);
    return apply(next);
}

Result<void> TimelineEditor::save(EditorSession& session) {
    if (!dirty_) return ok();
    const cook::String t = text();
    MCE_TRY(session.write_asset(rel_.view(), t.view()));
    disk_ = t;
    dirty_ = false;
    return ok();
}

bool TimelineEditor::undo(EditorSession& session) {
    if (undo_.empty()) return false;
    cook::TimelineDocument d;
    if (!read_text(undo_.back().view(), d)) return false;
    redo_.push_back(text());
    undo_.pop_back();
    set_doc(d);
    dirty_ = true;
    return static_cast<bool>(save(session));
}

bool TimelineEditor::redo(EditorSession& session) {
    if (redo_.empty()) return false;
    cook::TimelineDocument d;
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

// ---------------------------------------------------------------- the scene side

EntityId TimelineEditor::player_entity(const EditorScene& scene) const {
    if (!is_open()) return kNoEntity;
    for (const DocEntity& e : scene.doc().entities()) {
        const DocComponent* c = e.find(player_type());
        if (c && static_cast<const kit::TimelinePlayer*>(c->data())->timeline.guid == guid_) return e.id;
    }
    return kNoEntity;
}

u8 TimelineEditor::target_of(const EditorScene& scene, EntityId player, EntityId id) {
    if (id == player) return asset::kTimelineSelf;
    const kit::TimelinePlayer* p = player_of(scene, player);
    if (!p) return kNoBinding;
    for (u32 i = 0; i < asset::kTimelineBindings; ++i)
        if (doc_entity_id(p->bindings[i]) == id) return static_cast<u8>(i);
    return kNoBinding;
}

EntityId TimelineEditor::entity_of(const EditorScene& scene, EntityId player, u8 target) {
    if (target == asset::kTimelineSelf) return player;
    const kit::TimelinePlayer* p = player_of(scene, player);
    return p && target < asset::kTimelineBindings ? doc_entity_id(p->bindings[target]) : kNoEntity;
}

bool TimelineEditor::record(EditorScene& scene, EntityId id, const refl::TypeRecord& type, const void* before, const void* after) {
    if (!recording_ || in_record_ || !is_open() || &type == &player_type()) return false;
    const EntityId player = player_entity(scene);
    if (player == kNoEntity) return false;

    // What the edit keys: transform channels, or float-valued fields.
    const u64 changed = changed_fields_mask(type, before, after);
    if (changed == 0) return false;
    const bool transform = &type == &refl::type_of<LocalTransform>();
    u8 channels = 0;
    struct Prop {
        const refl::FieldRecord* field;
        u32 count;
    };
    Array<Prop> props;
    for (u32 i = 0; i < type.fields.size() && i < 64; ++i) {
        if (!(changed & (u64(1) << i))) continue;
        const refl::FieldRecord& f = type.fields[i];
        if (transform) {
            const StringView n(f.name_str);
            channels |= n == "position" ? cook::TimelineChannel::Position : n == "rotation" ? cook::TimelineChannel::Rotation
                                                                          : n == "scale"    ? cook::TimelineChannel::Scale
                                                                                            : u8(0);
            continue;
        }
        const u32 n = floats_of(f.kind);
        if (n > 0 && f.count == 1 && f.size == n * sizeof(f32)) props.push_back(Prop{&f, n});
    }
    if (channels == 0 && props.empty()) return false;

    // The entity's target: bind it to the first free slot when the player does not drive it yet.
    u8 target = target_of(scene, player, id);
    if (target == kNoBinding) {
        const kit::TimelinePlayer* p = player_of(scene, player);
        kit::TimelinePlayer copy = *p;
        for (u32 i = 0; i < asset::kTimelineBindings && target == kNoBinding; ++i)
            if (copy.bindings[i].is_null()) {
                copy.bindings[i] = doc_entity_ref(id);
                target = static_cast<u8>(i);
            }
        if (target == kNoBinding) {
            error_ = cook::String("the TimelinePlayer's eight bindings are all taken");
            return false;
        }
        in_record_ = true;
        scene.set_component(player, player_type(), &copy);
        in_record_ = false;
    }

    cook::TimelineDocument next = doc_;
    u32 sel_track = kNoTrack, sel_key = kNoKey;
    const f32 at = playhead_;
    if (channels) {
        u32 ti = find_track(T::Transform, target, {});
        if (ti == kNoTrack) {
            TimelineDocTrack t;
            t.type = T::Transform;
            t.target = target;
            next.tracks.push_back(t);
            ti = static_cast<u32>(next.tracks.size()) - 1;
        }
        TimelineDocTrack& tr = next.tracks[ti];
        TimelineDocKey k;
        for (const TimelineDocKey& e : tr.keys)
            if (same_time(e.t, at)) k = e;  // a key already here keeps its other channels
        k.t = at;
        const auto* lt = static_cast<const LocalTransform*>(after);
        if (channels & cook::TimelineChannel::Position) std::memcpy(k.position, &lt->position, sizeof(k.position));
        if (channels & cook::TimelineChannel::Rotation) {
            k.rotation[0] = lt->rotation.x;
            k.rotation[1] = lt->rotation.y;
            k.rotation[2] = lt->rotation.z;
            k.rotation[3] = lt->rotation.w;
        }
        if (channels & cook::TimelineChannel::Scale) std::memcpy(k.scale, &lt->scale, sizeof(k.scale));
        k.channels |= channels;
        sel_key = put_key(tr, k);
        sel_track = ti;
    }
    for (const Prop& pr : props) {
        cook::String path(type.name);
        path += ".";
        path += StringView(pr.field->name_str);
        u32 ti = find_track(T::Property, target, path.view());
        if (ti == kNoTrack) {
            TimelineDocTrack t;
            t.type = T::Property;
            t.target = target;
            t.path = path;
            cook::String why;
            if (!cook::resolve_timeline_path(t, why)) continue;
            next.tracks.push_back(t);
            ti = static_cast<u32>(next.tracks.size()) - 1;
        }
        TimelineDocTrack& tr = next.tracks[ti];
        TimelineDocKey k;
        k.t = at;
        std::memcpy(k.value, static_cast<const byte*>(after) + pr.field->offset, sizeof(f32) * pr.count);
        sel_key = put_key(tr, k);
        sel_track = ti;
    }
    if (sel_track == kNoTrack || !apply(next)) return false;
    select(sel_track, sel_key);
    return true;
}

// ---------------------------------------------------------------- preview

bool TimelinePreview::running(const EditorSession& session) const {
    const World* w = session.play().world();
    return session.play().active() && w && !player_.is_null() && w->alive(player_);
}

bool TimelinePreview::start(EditorSession& session, const TimelineEditor& ed) {
    why_ = cook::String();
    if (!ed.is_open()) {
        why_ = cook::String("no timeline is open");
        return false;
    }
    PlaySession& play = session.play();
    if (!play.active()) {
        if (!session.enter_play()) {
            why_ = cook::String("play does not start (see the log)");
            for (const cook::Diagnostic& d : session.play_diags().all())
                if (d.severity == cook::Severity::Error) {
                    why_ = cook::String(d.message.view());
                    break;
                }
            return false;
        }
        play.pause();
    }
    player_ = Entity::null;
    World& w = *play.world();
    w.query<const kit::TimelinePlayer>().each([&](Entity e, const kit::TimelinePlayer& p) {
        if (player_.is_null() && p.timeline.guid == ed.guid()) player_ = e;
    });
    if (player_.is_null()) {
        why_ = cook::String("no entity in the scene plays this timeline: add a TimelinePlayer that names it");
        session.stop_play();
        return false;
    }
    revision_ = ~u64(0);
    playhead_ = -1.0f;
    return update(session, ed);
}

bool TimelinePreview::update(EditorSession& session, const TimelineEditor& ed) {
    if (!running(session) || !ed.is_open()) return false;
    World& w = *session.play().world();
    if (ed.revision() != revision_) {
        kit::TimelineRegistry* reg = w.try_resource<kit::TimelineRegistry>();
        Array<byte> payload;
        ed.chunk(payload);
        if (!reg || !reg->define(ed.guid(), payload.span())) {
            why_ = cook::String("the timeline does not load into the play world");
            return false;
        }
        revision_ = ed.revision();
        playhead_ = -1.0f;  // re-scrub with the new keys
    }
    if (ed.playhead() != playhead_) {
        playhead_ = ed.playhead();
        kit::timeline_scrub(w, player_, playhead_);
    }
    return true;
}

void TimelinePreview::stop(EditorSession& session) {
    if (session.play().active() && !player_.is_null()) session.stop_play();
    player_ = Entity::null;
    revision_ = ~u64(0);
    playhead_ = -1.0f;
}

}  // namespace mce::editor
