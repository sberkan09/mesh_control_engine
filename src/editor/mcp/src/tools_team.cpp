// Several agents on one project (SBA-54). Every agent runs its own server on the same project
// folder; this file is how they see each other there:
//   .mce/events.jsonl   the event feed: one JSON line per write (who, which tool, which file)
//   .mce/locks/*.lock   asset locks: a write to a file another agent locked is refused
//   tasks.mctasks       the shared task list agents add to and claim work from
// and the three-way merge scene_save uses when another agent saved the same scene meanwhile.
// Files are created exclusively ("wx") where two agents may race, so a claim or a lock is never
// granted twice.

#include <cstdio>
#include <ctime>

#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/platform/platform.h"
#include "tool_args.h"
#include "tools_agent.h"
#include "tools_team.h"

namespace mce::mcp {

namespace {

constexpr u64 kDefaultLockTtl = 1800;  // seconds

i64 now_unix() { return static_cast<i64>(std::time(nullptr)); }

cook::String team_path(Server& server, StringView rel) { return cook::path_join(cook::path_join(server.session().project().root(), ".mce").view(), rel); }

bool ends_with(StringView s, StringView suffix) { return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix; }
bool starts_with(StringView s, StringView prefix) { return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix; }

bool write_text(StringView path, StringView text) {
    return static_cast<bool>(cook::fs::write_file_atomic(path, Span<const byte>(reinterpret_cast<const byte*>(text.data()), text.size())));
}

// Creates `path` with `text` only when it does not exist yet (false when it does, or on an error).
bool create_exclusive(StringView path, StringView text) {
    const usize slash = path.rfind('/');
    if (slash != StringView::npos) (void)cook::fs::create_directories(path.substr(0, slash));
    const cook::String p(path);
    std::FILE* f = std::fopen(p.c_str(), "wx");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    return ok;
}

// ---------------------------------------------------------------- the event feed

void append_event(Server& server, StringView tool, StringView path, StringView detail) {
    if (server.session().project().root().empty()) return;
    const cook::String file = team_path(server, "events.jsonl");
    (void)cook::fs::create_directories(team_path(server, "").view());
    cook::String line;
    JsonWriter w(line);
    w.begin_object().key("time").num_i(now_unix()).key("agent").str(server.agent()).key("tool").str(tool);
    if (!path.empty()) w.key("path").str(path);
    if (!detail.empty()) w.key("detail").str(detail);
    w.end_object();
    line += '\n';
    // One write of one line in append mode: lines from several agents do not interleave.
    std::FILE* f = std::fopen(file.c_str(), "ab");
    if (!f) return;
    (void)std::fwrite(line.c_str(), 1, line.size(), f);
    std::fclose(f);
}

// The file a writing tool writes (path under assets/), empty when the tool writes none.
cook::String write_target(ToolCall& c, StringView tool, bool& writes) {
    writes = false;
    auto str_arg = [&](const char* name) {
        const cook::JsonValue v = arg(c, name);
        return v.is_string() ? cook::String(v.as_string()) : cook::String();
    };
    if (tool == "scene_save") {
        writes = true;
        cook::String p = str_arg("path");
        return p.empty() ? cook::String(c.server.scene_file().rel.view()) : p;
    }
    if (starts_with(tool, "gen_")) {
        cook::String p = str_arg("save");
        writes = !p.empty();
        return p;
    }
    if (tool == "library_add" || tool == "module_build" || tool == "ship_build") {
        writes = true;
        cook::String p = str_arg("path");
        return ends_with(p.view(), "/") ? cook::String() : p;
    }
    // The asset, prefab and tilemap tools that change a file (the read-only ones list, get, read or describe).
    if (starts_with(tool, "asset_") || starts_with(tool, "prefab_") || starts_with(tool, "tilemap_")) {
        if (tool == "asset_lock" || tool == "asset_unlock" || tool == "asset_locks") return {};
        for (const char* ro : {"_list", "_get", "_describe", "_info", "_search", "_find", "_read", "_schema"})
            if (ends_with(tool, ro)) return {};
        writes = true;
        return str_arg("path");
    }
    return {};
}

// ---------------------------------------------------------------- locks

cook::String lock_file(Server& server, StringView rel) {
    cook::String name;
    for (const char ch : rel) name += ch == '/' ? '%' : ch;
    name += ".lock";
    return team_path(server, cook::path_join("locks", name.view()).view());
}

struct LockInfo {
    cook::String path, agent, note;
    i64 since = 0, ttl = 0;
    bool expired() const { return ttl > 0 && now_unix() - since > ttl; }
};

bool read_lock(StringView file, LockInfo& out) {
    cook::String text;
    if (!cook::fs::read_text(file, text)) return false;
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!doc.parse(text.view(), je)) return false;
    const cook::JsonValue r = doc.root();
    out.path = cook::String(r.get("path").is_string() ? r.get("path").as_string() : StringView());
    out.agent = cook::String(r.get("agent").is_string() ? r.get("agent").as_string() : StringView());
    out.note = cook::String(r.get("note").is_string() ? r.get("note").as_string() : StringView());
    out.since = r.get("since").is_number() ? static_cast<i64>(r.get("since").as_number()) : 0;
    out.ttl = r.get("ttl").is_number() ? static_cast<i64>(r.get("ttl").as_number()) : 0;
    return true;
}

void lock_text(Server& server, StringView rel, StringView note, u64 ttl, cook::String& out) {
    JsonWriter w(out);
    w.begin_object().key("path").str(rel).key("agent").str(server.agent()).key("since").num_i(now_unix()).key("ttl").num_u(ttl);
    if (!note.empty()) w.key("note").str(note);
    w.end_object();
}

void write_lock(JsonWriter& w, const LockInfo& l) {
    w.begin_object().key("path").str(l.path.view()).key("agent").str(l.agent.view()).key("since").num_i(l.since);
    w.key("age_s").num_i(now_unix() - l.since).key("expired").boolean(l.expired());
    if (!l.note.empty()) w.key("note").str(l.note.view());
    w.end_object();
}

// ---------------------------------------------------------------- tasks

// The task file's mutex: held for one read-modify-write.
struct TaskMutex {
    cook::String file;
    bool held = false;
    explicit TaskMutex(Server& server) : file(team_path(server, "locks/tasks.mutex")) {
        for (u32 attempt = 0; attempt < 200 && !held; ++attempt) {
            held = create_exclusive(file.view(), "tasks");
            if (held) break;
            if (attempt == 100) {
                (void)cook::fs::remove(file.view());  // half a second of waiting: a crashed agent's mutex
                continue;
            }
            platform::sleep_ms(5);
        }
    }
    ~TaskMutex() {
        if (held) (void)cook::fs::remove(file.view());
    }
    TaskMutex(const TaskMutex&) = delete;
    TaskMutex& operator=(const TaskMutex&) = delete;
};

struct Task {
    i64 id = 0;
    cook::String title, status{"open"}, owner, notes;
    Array<i64> depends;
    Array<cook::String> files;
    i64 updated = 0;
};

cook::String tasks_path(Server& server) { return cook::path_join(server.session().project().root(), "tasks.mctasks"); }

bool load_tasks(Server& server, Array<Task>& out, cook::String& err) {
    cook::String text;
    if (!cook::fs::read_text(tasks_path(server).view(), text)) return true;  // no file yet: no tasks
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!doc.parse(text.view(), je) || !doc.root().get("tasks").is_array()) {
        err = cook::String("tasks.mctasks is not a task file ({\"format\": \"mce-tasks\", \"tasks\": [...]})");
        return false;
    }
    const cook::JsonValue tasks = doc.root().get("tasks");
    for (u32 i = 0; i < tasks.size(); ++i) {
        const cook::JsonValue o = tasks.at(i);
        Task t;
        t.id = o.get("id").is_number() ? static_cast<i64>(o.get("id").as_number()) : 0;
        auto s = [&](const char* k) { return o.get(k).is_string() ? cook::String(o.get(k).as_string()) : cook::String(); };
        t.title = s("title");
        t.status = s("status");
        if (t.status.empty()) t.status = cook::String("open");
        t.owner = s("owner");
        t.notes = s("notes");
        t.updated = o.get("updated").is_number() ? static_cast<i64>(o.get("updated").as_number()) : 0;
        const cook::JsonValue d = o.get("depends");
        for (u32 j = 0; d.is_array() && j < d.size(); ++j)
            if (d.at(j).is_number()) t.depends.push_back(static_cast<i64>(d.at(j).as_number()));
        const cook::JsonValue f = o.get("files");
        for (u32 j = 0; f.is_array() && j < f.size(); ++j)
            if (f.at(j).is_string()) t.files.push_back(cook::String(f.at(j).as_string()));
        out.push_back(static_cast<Task&&>(t));
    }
    return true;
}

void write_task(JsonWriter& w, const Task& t) {
    w.begin_object().key("id").num_i(t.id).key("title").str(t.title.view()).key("status").str(t.status.view());
    if (!t.owner.empty()) w.key("owner").str(t.owner.view());
    if (!t.notes.empty()) w.key("notes").str(t.notes.view());
    if (!t.depends.empty()) {
        w.key("depends").begin_array();
        for (const i64 d : t.depends) w.num_i(d);
        w.end_array();
    }
    if (!t.files.empty()) {
        w.key("files").begin_array();
        for (const cook::String& f : t.files) w.str(f.view());
        w.end_array();
    }
    w.key("updated").num_i(t.updated).end_object();
}

bool save_tasks(Server& server, const Array<Task>& tasks) {
    cook::String text;
    JsonWriter w(text);
    w.begin_object().key("format").str("mce-tasks").key("version").num_u(1).key("tasks").begin_array();
    for (const Task& t : tasks) write_task(w, t);
    w.end_array().end_object();
    text += '\n';
    return write_text(tasks_path(server).view(), text.view());
}

bool known_status(StringView s) { return s == "open" || s == "claimed" || s == "done" || s == "blocked"; }

bool deps_done(const Array<Task>& all, const Task& t) {
    for (const i64 d : t.depends)
        for (const Task& o : all)
            if (o.id == d && o.status.view() != "done") return false;
    return true;
}

// ---------------------------------------------------------------- the merge

struct Field {
    cook::String key;   // "name", "components/Sprite", or a scene-level key
    cook::String text;  // its value as compact JSON
};

struct Ent {
    i64 id = 0;
    Array<Field> fields;
};

struct Side {
    Array<cook::String> top_order;  // scene-level keys in file order, "entities" included
    Array<Field> top;
    Array<Ent> ents;
};

cook::String canon(cook::JsonValue v) {
    cook::String s;
    JsonWriter w(s);
    w.copy(v);
    return s;
}

bool parse_side(StringView text, Side& out) {
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!doc.parse(text, je) || !doc.root().is_object()) return false;
    const cook::JsonValue root = doc.root();
    for (u32 i = 0; i < root.size(); ++i) {
        const StringView k = root.key_at(i);
        out.top_order.push_back(cook::String(k));
        if (k != "entities") {
            out.top.push_back({cook::String(k), canon(root.at(i))});
            continue;
        }
        const cook::JsonValue ents = root.at(i);
        for (u32 e = 0; ents.is_array() && e < ents.size(); ++e) {
            const cook::JsonValue o = ents.at(e);
            if (!o.is_object() || !o.get("id").is_number()) return false;
            Ent ent;
            ent.id = static_cast<i64>(o.get("id").as_number());
            for (u32 f = 0; f < o.size(); ++f) {
                const StringView fk = o.key_at(f);
                if (fk == "id") continue;
                if (fk == "components" && o.at(f).is_object()) {
                    const cook::JsonValue comps = o.at(f);
                    for (u32 cidx = 0; cidx < comps.size(); ++cidx) {
                        cook::String key("components/");
                        key += comps.key_at(cidx);
                        ent.fields.push_back({key, canon(comps.at(cidx))});
                    }
                    continue;
                }
                ent.fields.push_back({cook::String(fk), canon(o.at(f))});
            }
            out.ents.push_back(static_cast<Ent&&>(ent));
        }
    }
    return true;
}

const cook::String* find_field(const Array<Field>* fields, StringView key) {
    if (!fields) return nullptr;
    for (const Field& f : *fields)
        if (f.key.view() == key) return &f.text;
    return nullptr;
}

bool same(const cook::String* a, const cook::String* b) {
    if (!a || !b) return a == b;
    return a->view() == b->view();
}

const Ent* find_ent(const Side& s, i64 id) {
    for (const Ent& e : s.ents)
        if (e.id == id) return &e;
    return nullptr;
}

bool same_fields(const Array<Field>& a, const Array<Field>& b) {
    if (a.size() != b.size()) return false;
    for (const Field& f : a)
        if (!same(&f.text, find_field(&b, f.key.view()))) return false;
    return true;
}

cook::String ent_name(const Ent* a, const Ent* b) {
    for (const Ent* e : {a, b}) {
        const cook::String* n = e ? find_field(&e->fields, "name") : nullptr;
        if (n && n->size() >= 2) return cook::String(n->view().substr(1, n->size() - 2));
    }
    return {};
}

// Field-wise three-way merge (any pointer may be null: absent on that side).
void merge_fields(const Array<Field>* b, const Array<Field>* o, const Array<Field>* t, StringView resolve, i64 id, StringView name,
                  MergeResult& res, Array<Field>& out) {
    Array<cook::String> keys;
    auto add_keys = [&](const Array<Field>* fs) {
        if (!fs) return;
        for (const Field& f : *fs) {
            bool have = false;
            for (const cook::String& k : keys) have = have || k.view() == f.key.view();
            if (!have) keys.push_back(f.key);
        }
    };
    add_keys(t);
    add_keys(o);
    for (const cook::String& k : keys) {
        const cook::String* pb = find_field(b, k.view());
        const cook::String* po = find_field(o, k.view());
        const cook::String* pt = find_field(t, k.view());
        const cook::String* take = nullptr;
        if (same(po, pt)) take = po;
        else if (same(po, pb)) {
            take = pt;
            ++res.taken_from_theirs;
        } else if (same(pt, pb)) take = po;
        else {
            if (resolve.empty()) {
                MergeConflict mc;
                mc.entity = id;
                mc.name = cook::String(name);
                mc.field = k;
                mc.what = cook::String(!po ? "you removed it, they changed it" : !pt ? "you changed it, they removed it" : "both changed it");
                res.conflicts.push_back(static_cast<MergeConflict&&>(mc));
            }
            take = resolve == "ours" ? po : pt;
        }
        if (take) out.push_back({k, *take});
    }
}

void replace_all(cook::String& s, StringView from, StringView to) {
    cook::String out;
    const StringView v = s.view();
    for (usize i = 0; i < v.size();) {
        if (v.substr(i, from.size()) == from) {
            out += to;
            i += from.size();
        } else {
            out += v[i++];
        }
    }
    s = static_cast<cook::String&&>(out);
}

void emit_entity(JsonWriter& w, i64 id, const Array<Field>& fields) {
    w.begin_object().key("id").num_i(id);
    bool comps = false;
    for (const Field& f : fields) {
        if (starts_with(f.key.view(), "components/")) {
            comps = true;
            continue;
        }
        w.key(f.key.view()).raw(f.text.view());
    }
    if (comps) {
        w.key("components").begin_object();
        for (const Field& f : fields)
            if (starts_with(f.key.view(), "components/")) w.key(f.key.view().substr(11)).raw(f.text.view());
        w.end_object();
    }
    w.end_object();
}

}  // namespace

bool merge_entity_files(StringView base, StringView ours, StringView theirs, StringView resolve, MergeResult& res, cook::String& err) {
    Side B, O, T;
    if (!parse_side(base, B) || !parse_side(ours, O) || !parse_side(theirs, T)) {
        err = cook::String("a version of the file does not parse as a scene (entities with ids)");
        return false;
    }
    i64 max_id = 0;
    for (const Side* s : {&B, &O, &T})
        for (const Ent& e : s->ents) max_id = e.id > max_id ? e.id : max_id;

    struct Out {
        i64 id;
        Array<Field> fields;
    };
    Array<Out> ents;
    Array<i64> renumber;  // our new entities whose id they also used for another
    for (const Ent& t : T.ents) {
        const Ent* b = find_ent(B, t.id);
        const Ent* o = find_ent(O, t.id);
        const cook::String name = ent_name(o, &t);
        Out out{t.id, {}};
        if (b && o) {
            merge_fields(&b->fields, &o->fields, &t.fields, resolve, t.id, name.view(), res, out.fields);
        } else if (b && !o) {  // we deleted it
            if (same_fields(t.fields, b->fields)) continue;
            if (resolve.empty()) res.conflicts.push_back({t.id, name, cook::String("entity"), cook::String("you deleted it, they changed it")});
            if (resolve == "ours") continue;
            out.fields = t.fields;
        } else {
            if (o && !same_fields(o->fields, t.fields)) renumber.push_back(t.id);
            if (!o) ++res.taken_from_theirs;
            out.fields = t.fields;
        }
        ents.push_back(static_cast<Out&&>(out));
    }
    for (const Ent& b : B.ents) {  // they deleted it
        if (find_ent(T, b.id)) continue;
        const Ent* o = find_ent(O, b.id);
        if (!o || same_fields(o->fields, b.fields)) continue;
        const cook::String name = ent_name(o, &b);
        if (resolve.empty()) res.conflicts.push_back({b.id, name, cook::String("entity"), cook::String("you changed it, they deleted it")});
        if (resolve == "theirs") continue;
        ents.push_back({b.id, o->fields});
    }
    // Our new entities, after theirs; an id they took for something else moves to a free one, and
    // references to it ("$<id>") inside our new entities follow.
    Array<Out> added;
    Array<i64> from, to;
    for (const Ent& o : O.ents) {
        if (find_ent(B, o.id)) continue;
        bool clash = false;
        for (const i64 r : renumber) clash = clash || r == o.id;
        if (find_ent(T, o.id) && !clash) continue;  // the same entity on both sides
        i64 id = o.id;
        if (clash) {
            id = ++max_id;
            from.push_back(o.id);
            to.push_back(id);
            ++res.renumbered;
        }
        added.push_back({id, o.fields});
    }
    for (Out& a : added) {
        for (Field& f : a.fields)
            for (usize i = 0; i < from.size(); ++i) {
                cook::String x, y;
                cook::appendf(x, "\"$%lld\"", static_cast<long long>(from[i]));
                cook::appendf(y, "\"$%lld\"", static_cast<long long>(to[i]));
                replace_all(f.text, x.view(), y.view());
            }
        ents.push_back(static_cast<Out&&>(a));
    }

    // Scene-level keys (name, guid, settings, ...).
    Array<Field> top;
    merge_fields(&B.top, &O.top, &T.top, resolve, 0, "", res, top);
    JsonWriter w(res.text);
    w.begin_object();
    bool wrote_entities = false;
    auto emit_entities = [&] {
        w.key("entities").begin_array();
        for (const Out& e : ents) emit_entity(w, e.id, e.fields);
        w.end_array();
        wrote_entities = true;
    };
    for (const cook::String& k : T.top_order) {
        if (k.view() == "entities") {
            emit_entities();
            continue;
        }
        if (const cook::String* v = find_field(&top, k.view())) w.key(k.view()).raw(v->view());
    }
    for (const Field& f : top) {  // keys only ours has
        bool in_t = false;
        for (const cook::String& k : T.top_order) in_t = in_t || k.view() == f.key.view();
        if (!in_t) w.key(f.key.view()).raw(f.text.view());
    }
    if (!wrote_entities) emit_entities();
    w.end_object();
    res.text += '\n';
    return true;
}

// Appends which entities the file on disk has that `ours` lacks, and the other way round (saving
// `ours` over the file would drop the first group), to an error message.
void append_entity_diff(cook::String& err, StringView ours, StringView theirs) {
    Side O, T;
    if (!parse_side(ours, O) || !parse_side(theirs, T)) return;
    auto list = [&](const char* head, const Side& from, const Side& other) {
        usize n = 0;
        for (const Ent& e : from.ents) {
            if (find_ent(other, e.id)) continue;
            if (n == 0) {
                err += '\n';
                err += head;
            }
            if (n++ == 12) {
                err += " ...";
                break;
            }
            const cook::String name = ent_name(&e, nullptr);
            cook::appendf(err, " %lld", static_cast<long long>(e.id));
            if (!name.empty()) {
                err += " \"";
                err += name.view();
                err += '"';
            }
            err += ';';
        }
    };
    list("on disk, not in your copy (saving over the file would drop them):", T, O);
    list("in your copy, not on disk:", O, T);
}

bool merge_scene_from_disk(ToolCall& c, StringView abs) {
    Server::SceneFile& file = c.server.scene_file();
    StringView resolve;
    if (!arg_string(c, "resolve", resolve, false)) return false;
    if (!resolve.empty() && resolve != "ours" && resolve != "theirs") return arg_missing(c, "resolve", "\"ours\" or \"theirs\"");
    cook::String theirs, ours;
    if (!cook::fs::read_text(abs, theirs)) return fail(c, "cannot read the scene file on disk");
    c.server.session().scene().serialize(ours);
    if (file.base.empty()) {
        c.error += "the file changed on disk since it was opened or saved and cannot be merged (no common version): ";
        c.error += "scene_open {\"path\": \"";
        c.error += file.rel.view();
        c.error += "\", \"force\": true} reloads it (dropping your unsaved edits), or pass overwrite: true to save yours over it anyway";
        append_entity_diff(c.error, ours.view(), theirs.view());
        return false;
    }
    MergeResult res;
    cook::String err;
    if (!merge_entity_files(file.base.view(), ours.view(), theirs.view(), resolve, res, err)) {
        c.error += "the scene changed on disk and cannot be merged (";
        c.error += err.view();
        c.error += "): scene_open {force: true} reloads it (dropping your unsaved edits), or pass overwrite: true to save yours over it anyway";
        append_entity_diff(c.error, ours.view(), theirs.view());
        return false;
    }
    if (!res.conflicts.empty()) {
        cook::appendf(c.error, "another agent saved this scene meanwhile and %u change(s) conflict with yours; nothing was written:",
                      static_cast<u32>(res.conflicts.size()));
        usize n = 0;
        for (const MergeConflict& m : res.conflicts) {
            if (n++ == 12) {
                c.error += "\n  ...";
                break;
            }
            c.error += "\n  ";
            if (m.entity) cook::appendf(c.error, "entity %lld ", static_cast<long long>(m.entity));
            if (!m.name.empty()) {
                c.error += '"';
                c.error += m.name.view();
                c.error += "\" ";
            }
            c.error += m.field.view();
            c.error += ": ";
            c.error += m.what.view();
        }
        c.error += "\nscene_save with resolve: \"ours\" keeps your side of each conflict, \"theirs\" theirs (everything else merges); "
                   "overwrite: true saves your whole copy over the file (their changes are lost); "
                   "project_watch says who saved it";
        return false;
    }
    cook::DiagList diags;
    if (!c.server.session().scene().recover(res.text.view(), diags)) {
        c.error += "the merged scene does not load:";
        append_diags(c.error, diags, 10);
        return false;
    }
    file.base = theirs;  // the merge's result now descends from their version
    cook::String detail;
    cook::appendf(detail, "merged another agent's save: %u change(s) from theirs, %u entity id(s) renumbered", res.taken_from_theirs, res.renumbered);
    append_event(c.server, "scene_merge", file.rel.view(), detail.view());
    return true;
}

bool team_before_tool(ToolCall& c, StringView tool) {
    if (c.server.session().project().root().empty()) return true;
    bool writes = false;
    const cook::String target = write_target(c, tool, writes);
    if (!writes || target.empty()) return true;
    LockInfo l;
    if (!read_lock(lock_file(c.server, target.view()).view(), l) || l.agent.view() == c.server.agent() || l.expired()) return true;
    c.error += "assets/";
    c.error += target.view();
    c.error += " is locked by ";
    c.error += l.agent.view();
    if (!l.note.empty()) {
        c.error += " (";
        c.error += l.note.view();
        c.error += ')';
    }
    cook::appendf(c.error, " since %lld s ago: work on something else (task_list) or wait; project_watch shows the unlock",
                  static_cast<long long>(now_unix() - l.since));
    return false;
}

void team_after_tool(ToolCall& c, StringView tool) {
    bool writes = false;
    const cook::String target = write_target(c, tool, writes);
    if (writes) append_event(c.server, tool, target.view(), {});
}

// ---------------------------------------------------------------- the tools

bool project_watch(ToolCall& c) {
    if (!need_project(c)) return false;
    u64 since = 0, limit = 50;
    bool others = false;
    if (!arg_uint(c, "since", since, 0, ~0ull >> 12, false) || !arg_uint(c, "limit", limit, 1, 500, false) || !arg_bool(c, "others", others)) return false;
    cook::String text;
    (void)cook::fs::read_text(team_path(c.server, "events.jsonl").view(), text);
    c.out.begin_object().key("agent").str(c.server.agent()).key("events").begin_array();
    u64 seq = 0, shown = 0;
    const StringView all = text.view();
    for (usize at = 0; at < all.size();) {
        usize end = all.find('\n', at);
        if (end == StringView::npos) end = all.size();
        const StringView line = all.substr(at, end - at);
        at = end + 1;
        if (line.empty()) continue;
        ++seq;
        if (seq <= since || shown == limit) continue;
        cook::JsonDoc doc;
        cook::JsonError je;
        if (!doc.parse(line, je)) continue;
        if (others && doc.root().get("agent").is_string() && doc.root().get("agent").as_string() == c.server.agent()) continue;
        ++shown;
        c.out.begin_object().key("seq").num_u(seq);
        const cook::JsonValue r = doc.root();
        for (u32 i = 0; i < r.size(); ++i) c.out.key(r.key_at(i)).copy(r.at(i));
        c.out.end_object();
    }
    c.out.end_array();
    // `next` is the since for the next call: everything up to the last event read.
    c.out.key("next").num_u(shown == limit ? since + shown : seq).end_object();
    return true;
}

bool asset_lock(ToolCall& c) {
    StringView rel, note;
    u64 ttl = kDefaultLockTtl;
    if (!need_project(c) || !arg_string(c, "path", rel, true) || !arg_string(c, "note", note, false) ||
        !arg_uint(c, "ttl", ttl, 10, 7 * 24 * 3600, false))
        return false;
    if (rel.empty() || rel[0] == '/' || rel.find("..") != StringView::npos || rel.find('\\') != StringView::npos)
        return arg_missing(c, "path", "a file path under assets/ (forward slashes, no '..')");
    const cook::String file = lock_file(c.server, rel);
    cook::String text;
    lock_text(c.server, rel, note, ttl, text);
    LockInfo held;
    bool took_over = false;
    if (!create_exclusive(file.view(), text.view())) {
        if (!read_lock(file.view(), held)) return fail(c, "cannot read the lock file");
        if (held.agent.view() != c.server.agent() && !held.expired()) {
            c.error += "already locked by ";
            c.error += held.agent.view();
            cook::appendf(c.error, " %lld s ago", static_cast<long long>(now_unix() - held.since));
            if (!held.note.empty()) {
                c.error += " (";
                c.error += held.note.view();
                c.error += ')';
            }
            return false;
        }
        took_over = held.agent.view() != c.server.agent();
        if (!took_over && note.empty() && !held.note.empty()) {  // a refresh keeps what the lock was for
            text.clear();
            lock_text(c.server, rel, held.note.view(), ttl, text);
        }
        if (!write_text(file.view(), text.view())) return fail(c, "cannot write the lock file");  // ours (refresh) or expired
    }
    append_event(c.server, "asset_lock", rel, took_over ? StringView("took over an expired lock") : note);
    LockInfo now;
    (void)read_lock(file.view(), now);
    c.out.begin_object().key("locked").boolean(true).key("took_over").boolean(took_over).key("lock");
    write_lock(c.out, now);
    c.out.end_object();
    return true;
}

bool asset_unlock(ToolCall& c) {
    StringView rel;
    bool force = false;
    if (!need_project(c) || !arg_string(c, "path", rel, true) || !arg_bool(c, "force", force)) return false;
    const cook::String file = lock_file(c.server, rel);
    LockInfo l;
    if (!read_lock(file.view(), l)) return fail(c, "that file is not locked");
    if (l.agent.view() != c.server.agent() && !force && !l.expired()) {
        c.error += "the lock is ";
        c.error += l.agent.view();
        c.error += "'s: only they unlock it (force: true breaks it, and the feed records that you did)";
        return false;
    }
    (void)cook::fs::remove(file.view());
    append_event(c.server, "asset_unlock", rel, l.agent.view() != c.server.agent() ? StringView("broke another agent's lock") : StringView());
    c.out.begin_object().key("unlocked").str(rel).end_object();
    return true;
}

bool asset_locks(ToolCall& c) {
    if (!need_project(c)) return false;
    Array<cook::String> files;
    (void)cook::fs::list_files(team_path(c.server, "locks").view(), files);
    c.out.begin_object().key("agent").str(c.server.agent()).key("locks").begin_array();
    for (const cook::String& f : files) {
        if (!ends_with(f.view(), ".lock")) continue;
        LockInfo l;
        if (read_lock(cook::path_join(team_path(c.server, "locks").view(), f.view()).view(), l)) write_lock(c.out, l);
    }
    c.out.end_array().end_object();
    return true;
}

bool task_list(ToolCall& c) {
    StringView status;
    if (!need_project(c) || !arg_string(c, "status", status, false)) return false;
    if (!status.empty() && !known_status(status)) return arg_missing(c, "status", "open, claimed, done or blocked");
    Array<Task> tasks;
    cook::String err;
    if (!load_tasks(c.server, tasks, err)) return fail(c, err.view());
    c.out.begin_object().key("agent").str(c.server.agent()).key("tasks").begin_array();
    for (const Task& t : tasks)
        if (status.empty() || t.status.view() == status) write_task(c.out, t);
    c.out.end_array();
    static constexpr const char* kStatuses[4] = {"open", "claimed", "done", "blocked"};
    u32 counts[4] = {};
    for (const Task& t : tasks)
        for (u32 i = 0; i < 4; ++i)
            if (t.status.view() == kStatuses[i]) ++counts[i];
    c.out.key("open").num_u(counts[0]).key("claimed").num_u(counts[1]).key("done").num_u(counts[2]).key("blocked").num_u(counts[3]);
    c.out.end_object();
    return true;
}

bool task_add(ToolCall& c) {
    StringView title, notes;
    if (!need_project(c) || !arg_string(c, "title", title, true) || !arg_string(c, "notes", notes, false)) return false;
    if (title.empty() || title.size() > 200) return arg_missing(c, "title", "1-200 characters");
    Task t;
    t.title = cook::String(title);
    t.notes = cook::String(notes);
    const cook::JsonValue deps = arg(c, "depends");
    for (u32 i = 0; deps.is_array() && i < deps.size(); ++i)
        if (deps.at(i).is_number()) t.depends.push_back(static_cast<i64>(deps.at(i).as_number()));
    const cook::JsonValue files = arg(c, "files");
    for (u32 i = 0; files.is_array() && i < files.size(); ++i)
        if (files.at(i).is_string()) t.files.push_back(cook::String(files.at(i).as_string()));
    TaskMutex mutex(c.server);
    if (!mutex.held) return fail(c, "the task file is busy (another agent holds it): try again");
    Array<Task> tasks;
    cook::String err;
    if (!load_tasks(c.server, tasks, err)) return fail(c, err.view());
    i64 next = 1;
    for (const Task& o : tasks) next = o.id >= next ? o.id + 1 : next;
    for (const i64 d : t.depends) {
        bool found = false;
        for (const Task& o : tasks) found = found || o.id == d;
        if (!found) return arg_missing(c, "depends", "ids of existing tasks");
    }
    t.id = next;
    t.updated = now_unix();
    tasks.push_back(static_cast<Task&&>(t));
    if (!save_tasks(c.server, tasks)) return fail(c, "cannot write tasks.mctasks");
    append_event(c.server, "task_add", "tasks.mctasks", title);
    c.out.begin_object().key("task");
    write_task(c.out, tasks.back());
    c.out.end_object();
    return true;
}

bool task_claim(ToolCall& c) {
    u64 id = 0;
    if (!need_project(c) || !arg_uint(c, "id", id, 1, ~0ull >> 12, false)) return false;
    TaskMutex mutex(c.server);
    if (!mutex.held) return fail(c, "the task file is busy (another agent holds it): try again");
    Array<Task> tasks;
    cook::String err;
    if (!load_tasks(c.server, tasks, err)) return fail(c, err.view());
    Task* pick = nullptr;
    for (Task& t : tasks) {
        if (id ? t.id != static_cast<i64>(id) : (t.status.view() != "open" || !deps_done(tasks, t))) continue;
        pick = &t;
        break;
    }
    if (!pick) return fail(c, id ? "no task with that id (task_list)" : "no open task whose dependencies are done (task_list shows the rest)");
    if (pick->status.view() == "claimed" && pick->owner.view() != c.server.agent()) {
        c.error += "task already claimed by ";
        c.error += pick->owner.view();
        return false;
    }
    if (pick->status.view() == "done") return fail(c, "that task is done");
    if (!deps_done(tasks, *pick)) return fail(c, "that task depends on tasks that are not done yet");
    pick->status = cook::String("claimed");
    pick->owner = cook::String(c.server.agent());
    pick->updated = now_unix();
    if (!save_tasks(c.server, tasks)) return fail(c, "cannot write tasks.mctasks");
    cook::String detail;
    cook::appendf(detail, "#%lld %s", static_cast<long long>(pick->id), pick->title.c_str());
    append_event(c.server, "task_claim", "tasks.mctasks", detail.view());
    c.out.begin_object().key("task");
    write_task(c.out, *pick);
    c.out.end_object();
    return true;
}

bool task_update(ToolCall& c) {
    u64 id = 0;
    StringView status, notes;
    if (!need_project(c) || !arg_uint(c, "id", id, 1, ~0ull >> 12, true) || !arg_string(c, "status", status, false) ||
        !arg_string(c, "notes", notes, false))
        return false;
    if (!status.empty() && !known_status(status)) return arg_missing(c, "status", "open, claimed, done or blocked");
    TaskMutex mutex(c.server);
    if (!mutex.held) return fail(c, "the task file is busy (another agent holds it): try again");
    Array<Task> tasks;
    cook::String err;
    if (!load_tasks(c.server, tasks, err)) return fail(c, err.view());
    Task* t = nullptr;
    for (Task& x : tasks)
        if (x.id == static_cast<i64>(id)) t = &x;
    if (!t) return fail(c, "no task with that id (task_list)");
    if (t->status.view() == "claimed" && t->owner.view() != c.server.agent() && !status.empty()) {
        c.error += "task claimed by ";
        c.error += t->owner.view();
        c.error += ": only they change its status";
        return false;
    }
    if (!status.empty()) {
        t->status = cook::String(status);
        if (status == "open") t->owner.clear();
    }
    if (!notes.empty()) {
        if (!t->notes.empty()) t->notes += "\n";
        t->notes += c.server.agent();
        t->notes += ": ";
        t->notes += notes;
    }
    t->updated = now_unix();
    if (!save_tasks(c.server, tasks)) return fail(c, "cannot write tasks.mctasks");
    cook::String detail;
    cook::appendf(detail, "#%lld %s", static_cast<long long>(t->id), t->status.c_str());
    append_event(c.server, "task_update", "tasks.mctasks", detail.view());
    c.out.begin_object().key("task");
    write_task(c.out, *t);
    c.out.end_object();
    return true;
}

bool scene_switch(ToolCall& c) {
    StringView rel;
    if (!need_project(c) || !arg_string(c, "path", rel, true)) return false;
    Server::SceneFile& cur = c.server.scene_file();
    Array<Server::ParkedScene>& parked = c.server.parked_scenes();
    if (rel == cur.rel.view()) {
        c.out.begin_object().key("path").str(rel).key("restored_edits").boolean(false).end_object();
        return true;
    }
    editor::EditorScene& s = c.server.session().scene();
    if (cur.rel.empty() && s.dirty()) return fail(c, "the open scene was never saved: scene_save it with a path first, then switch");
    // Park the current scene (its text, unsaved edits included).
    Server::ParkedScene park;
    if (!cur.rel.empty()) {
        park.file = cur;
        s.serialize(park.text);
        park.dirty = s.dirty();
    }
    // Open the target: a parked one comes back with its edits and its merge base.
    usize found = parked.size();
    for (usize i = 0; i < parked.size(); ++i)
        if (parked[i].file.rel.view() == rel) found = i;
    if (!open_scene_file(c.server, rel, c.error)) return false;
    bool restored = false;
    if (found < parked.size()) {
        Server::ParkedScene p = static_cast<Server::ParkedScene&&>(parked[found]);
        parked.erase(found);
        if (p.dirty) {
            cook::DiagList diags;
            if (s.recover(p.text.view(), diags)) restored = true;
        }
        c.server.scene_file() = static_cast<Server::SceneFile&&>(p.file);
    }
    if (!park.file.rel.empty()) {
        for (usize i = 0; i < parked.size(); ++i)
            if (parked[i].file.rel.view() == park.file.rel.view()) {
                parked.erase(i);
                break;
            }
        parked.push_back(static_cast<Server::ParkedScene&&>(park));
    }
    c.out.begin_object().key("path").str(rel).key("restored_edits").boolean(restored).key("entities").num_u(s.doc().size());
    c.out.key("open").begin_array();
    c.out.begin_object().key("path").str(c.server.scene_file().rel.view()).key("current").boolean(true).key("dirty").boolean(s.dirty()).end_object();
    for (const Server::ParkedScene& p : parked)
        c.out.begin_object().key("path").str(p.file.rel.view()).key("current").boolean(false).key("dirty").boolean(p.dirty).end_object();
    c.out.end_array().end_object();
    return true;
}

bool scene_documents(ToolCall& c) {
    editor::EditorScene& s = c.server.session().scene();
    c.out.begin_object().key("open").begin_array();
    c.out.begin_object().key("path").str(c.server.scene_file().rel.view()).key("current").boolean(true).key("dirty").boolean(s.dirty()).end_object();
    for (const Server::ParkedScene& p : c.server.parked_scenes())
        c.out.begin_object().key("path").str(p.file.rel.view()).key("current").boolean(false).key("dirty").boolean(p.dirty).end_object();
    c.out.end_array().key("agent").str(c.server.agent()).end_object();
    return true;
}

}  // namespace mce::mcp
