// .mcseq -> CHNK_SEQ: one action-list sequence (FR-ENT-100, scripting.md §2.6). Every step is an
// object with exactly one action key; the closing End step is appended by the cook. Sound and
// prefab GUIDs are dependencies (a missing or wrongly typed reference is a cook error naming
// file:line:col).
//
// {
//   "format": "mce-sequence", "version": 1, "guid": "<uuid>", "name": "intro", "loop": false,
//   "steps": [
//     {"wait": 0.5},                                       // seconds
//     {"wait_event": "door_open"},                         // a seq::Signal with this id
//     {"move_to": [4, 2], "speed": 3, "relative": true},   // metres, metres/second
//     {"anim": "attack"},                                  // a seq::AnimRequest
//     {"sound": "guid:<sfx uuid>", "gain_db": -3},
//     {"stop_sound": true},                                // stops the entity's AudioSource
//     {"emit": "beat_done"},                               // a seq::Signal
//     {"call": "shake_camera"},                            // a registered code hook
//     {"spawn": "guid:<prefab uuid>", "at": [0, 1], "relative": true},
//     {"destroy": true}                                    // destroys the playing entity
//   ]
// }
//
// A looping sequence must contain a step that waits (wait > 0, wait_event or move_to): a loop of
// instant steps would never yield the tick, so the cook rejects it.

#include <cstdio>
#include <cstring>
#include <utility>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/string_id.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.sequence";
constexpr u32 kSequenceFormatVersion = 1;
constexpr u32 kMaxSteps = 4096;

using asset::SequenceStep;
using asset::SequenceStepKind;

struct Action {
    const char* key;
    SequenceStepKind kind;
    const char* extras[3];  // other keys allowed on the step
};

constexpr Action kActions[] = {
    {"wait", SequenceStepKind::Wait, {}},
    {"wait_event", SequenceStepKind::WaitForEvent, {}},
    {"move_to", SequenceStepKind::MoveTo, {"speed", "relative", "tolerance"}},
    {"anim", SequenceStepKind::PlayAnim, {}},
    {"sound", SequenceStepKind::PlaySound, {"gain_db"}},
    {"stop_sound", SequenceStepKind::StopSound, {}},
    {"emit", SequenceStepKind::Emit, {}},
    {"call", SequenceStepKind::Call, {}},
    {"spawn", SequenceStepKind::Spawn, {"at", "relative"}},
    {"destroy", SequenceStepKind::Destroy, {}},
};

const Action* find_action(StringView key) {
    for (const Action& a : kActions)
        if (key == a.key) return &a;
    return nullptr;
}

bool allowed_extra(const Action& a, StringView key) {
    for (const char* e : a.extras)
        if (e && key == e) return true;
    return false;
}

// The step's action: the one key that names an action (others must be its extras).
const Action* step_action(JsonValue step, StringView file, DiagList& diags) {
    if (!step.is_object()) {
        detail::error_at(diags, Errc::ParseError, file, step, kStage, "a step must be an object with one action key");
        return nullptr;
    }
    const Action* found = nullptr;
    for (u32 i = 0; i < step.size(); ++i) {
        const StringView key = step.key_at(i);
        if (const Action* a = find_action(key)) {
            if (found) {
                detail::error_at(diags, Errc::ParseError, file, step, kStage, "a step has exactly one action key");
                return nullptr;
            }
            found = a;
        }
    }
    if (!found) {
        detail::error_at(diags, Errc::ParseError, file, step, kStage,
                         "unknown step: expected wait, wait_event, move_to, anim, sound, stop_sound, emit, call, spawn or destroy");
        return nullptr;
    }
    for (u32 i = 0; i < step.size(); ++i) {
        const StringView key = step.key_at(i);
        if (key != found->key && !allowed_extra(*found, key)) {
            String msg;
            appendf(msg, "'%.*s' is not a key of a %s step", static_cast<int>(key.size()), key.data(), found->key);
            detail::error_at(diags, Errc::ParseError, file, step.get(key), kStage, msg);
            return nullptr;
        }
    }
    return found;
}

// [x, y] or [x, y, z] into pos (z defaults to 0).
bool read_position(JsonValue v, f32 pos[3], StringView file, DiagList& diags) {
    if (!v.is_array() || v.size() < 2 || v.size() > 3) {
        detail::error_at(diags, Errc::ParseError, file, v, kStage, "a position is [x, y] or [x, y, z]");
        return false;
    }
    for (u32 i = 0; i < v.size(); ++i) {
        if (!v.at(i).is_number()) {
            detail::error_at(diags, Errc::ParseError, file, v.at(i), kStage, "expected a number");
            return false;
        }
        pos[i] = static_cast<f32>(v.at(i).as_number());
    }
    return true;
}

bool read_name(JsonValue v, u64& out, StringView file, DiagList& diags) {
    if (!v.is_string() || v.as_string().empty()) {
        detail::error_at(diags, Errc::ParseError, file, v, kStage, "expected a non-empty name");
        return false;
    }
    out = StringId(v.as_string()).value;
    return true;
}

bool read_guid(JsonValue v, SequenceStep& s, StringView file, DiagList& diags) {
    Guid g;
    if (!v.is_string() || !parse_guid_ref(v.as_string(), g)) {
        detail::error_at(diags, Errc::ParseError, file, v, kStage, "expected \"guid:<uuid>\"");
        return false;
    }
    s.guid[0] = g.hi;
    s.guid[1] = g.lo;
    return true;
}

class SequenceImporter final : public IImporter {
public:
    StringView id() const override { return "sequence"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Sequence; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcseq"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        const JsonValue steps = in.settings.get("steps");
        if (!steps.is_array()) return;
        for (u32 i = 0; i < steps.size(); ++i) {
            const JsonValue step = steps.at(i);
            for (const auto& [key, type] : {std::pair<const char*, ChunkType>{"sound", ChunkType::Sfx}, {"spawn", ChunkType::Prefab}}) {
                const JsonValue v = step.is_object() ? step.get(key) : JsonValue();
                if (!v.valid()) continue;
                Guid g;
                if (!v.is_string() || !parse_guid_ref(v.as_string(), g)) {
                    detail::error_at(diags, Errc::ParseError, in.display_path, v, kStage, "expected \"guid:<uuid>\"");
                    continue;
                }
                DepRef d;
                d.guid = g;
                d.expected = type;
                d.line = v.line();
                d.col = v.col();
                char where[64];
                std::snprintf(where, sizeof(where), "steps[%u].%s", i, key);
                d.where.assign(where);
                out.push_back(d);
            }
        }
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "loop", "steps"}, file, kStage, diags)) return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-sequence") {
            detail::error_at(diags, Errc::ParseError, file, fmt.valid() ? fmt : doc, kStage, "\"format\" must be \"mce-sequence\"");
            return false;
        }
        if (!detail::check_format_version(doc, kSequenceFormatVersion, "sequence", file, kStage, diags)) return false;

        asset::SequenceHeader header{};
        if (const JsonValue name = doc.get("name"); name.valid()) {
            if (!name.is_string() || name.as_string().size() >= sizeof(header.name))
                detail::error_at(diags, Errc::ParseError, file, name, kStage, "'name' must be a string of at most 31 bytes");
            else std::memcpy(header.name, name.as_string().data(), name.as_string().size());
        }
        bool loop = false;
        detail::read_bool(doc, "loop", false, loop, file, kStage, diags);
        if (loop) header.flags |= 1u;

        const JsonValue steps = doc.get("steps");
        if (!steps.is_array() || steps.size() == 0 || steps.size() >= kMaxSteps) {
            String msg;
            appendf(msg, "'steps' must be an array of 1 to %u steps", kMaxSteps - 1);
            detail::error_at(diags, Errc::ParseError, file, steps.valid() ? steps : doc, kStage, msg);
            return false;
        }
        Array<SequenceStep> cooked;
        bool waits = false;
        for (u32 i = 0; i < steps.size(); ++i) {
            const JsonValue step = steps.at(i);
            const Action* a = step_action(step, file, diags);
            if (!a) continue;
            const JsonValue v = step.get(a->key);
            SequenceStep s{};
            s.kind = a->kind;
            switch (a->kind) {
            case SequenceStepKind::Wait:
                if (!v.is_number() || v.as_number() < 0) detail::error_at(diags, Errc::ParseError, file, v, kStage, "'wait' must be seconds >= 0");
                else s.a = static_cast<f32>(v.as_number());
                waits |= s.a > 0.0f;
                break;
            case SequenceStepKind::WaitForEvent:
            case SequenceStepKind::PlayAnim:
            case SequenceStepKind::Emit:
            case SequenceStepKind::Call:
                read_name(v, s.id, file, diags);
                waits |= a->kind == SequenceStepKind::WaitForEvent;
                break;
            case SequenceStepKind::MoveTo: {
                read_position(v, s.pos, file, diags);
                f64 speed = 1.0, tolerance = 0.01;
                detail::read_number(step, "speed", 1.0, speed, file, kStage, diags);
                detail::read_number(step, "tolerance", 0.01, tolerance, file, kStage, diags);
                if (speed <= 0) detail::error_at(diags, Errc::ParseError, file, step.get("speed"), kStage, "'speed' must be > 0");
                if (tolerance < 0) detail::error_at(diags, Errc::ParseError, file, step.get("tolerance"), kStage, "'tolerance' must be >= 0");
                s.a = static_cast<f32>(speed);
                s.b = static_cast<f32>(tolerance);
                bool relative = false;
                detail::read_bool(step, "relative", false, relative, file, kStage, diags);
                if (relative) s.flags |= asset::SequenceStepFlags::Relative;
                waits = true;
                break;
            }
            case SequenceStepKind::PlaySound: {
                read_guid(v, s, file, diags);
                f64 gain = 0.0;
                detail::read_number(step, "gain_db", 0.0, gain, file, kStage, diags);
                s.a = static_cast<f32>(gain);
                break;
            }
            case SequenceStepKind::Spawn: {
                read_guid(v, s, file, diags);
                if (const JsonValue at = step.get("at"); at.valid()) read_position(at, s.pos, file, diags);
                bool relative = false;
                detail::read_bool(step, "relative", false, relative, file, kStage, diags);
                if (relative) s.flags |= asset::SequenceStepFlags::Relative;
                break;
            }
            case SequenceStepKind::StopSound:
            case SequenceStepKind::Destroy:
                if (!v.is_bool() || !v.as_bool()) detail::error_at(diags, Errc::ParseError, file, v, kStage, "expected true");
                break;
            case SequenceStepKind::End: break;
            }
            cooked.push_back(s);
        }
        if (loop && !waits)
            detail::error_at(diags, Errc::ParseError, file, doc.get("loop"), kStage,
                             "a looping sequence needs a step that waits (wait > 0, wait_event or move_to)");
        if (diags.error_count() != errors_before) return false;

        SequenceStep end{};
        end.kind = SequenceStepKind::End;
        cooked.push_back(end);
        header.step_count = static_cast<u32>(cooked.size());

        Array<byte> payload;
        payload.resize(sizeof(header) + cooked.size() * sizeof(SequenceStep));
        std::memcpy(payload.data(), &header, sizeof(header));
        std::memcpy(payload.data() + sizeof(header), cooked.data(), cooked.size() * sizeof(SequenceStep));
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Sequence;
        d.version = asset::chunk_version(asset::SEQUENCE_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_sequence_importer() { return mem::make_unique<SequenceImporter>(mem::heap()); }

}  // namespace mce::cook
