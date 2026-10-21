// M-AGENT content (SBA-52): procedural generators (palette, pixel sprite, tile set, noise and sky
// texture, sfxr sound effect, music loop) and the bundled CC0/OFL library. Every tool returns a
// preview image (a sound's waveform) and, with `save`, writes the file under assets/ with its
// .mceasset sidecar and a CREDITS.md row, so the next cook_run takes it with no adopt step.

#include <cmath>
#include <cstring>

#include "agent_gen.h"
#include "agent_image.h"
#include "mce/asset/guid.h"
#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/cook/importer.h"
#include "tool_args.h"
#include "tools_agent.h"
#include "tools_docs.h"

namespace mce::mcp {

namespace {

constexpr u64 kMaxSeed = 0xFFFFFFFFull;

// ---------------------------------------------------------------- arguments

bool arg_seed(ToolCall& c, u64& seed) {
    seed = 1;
    return arg_uint(c, "seed", seed, 0, kMaxSeed, false);
}

bool arg_f32(ToolCall& c, const char* name, f32& out, f32 lo, f32 hi) {
    const cook::JsonValue v = arg(c, name);
    if (!v.valid()) return true;
    if (!v.is_number() || v.as_number() < lo || v.as_number() > hi) {
        cook::String e;
        cook::appendf(e, "a number from %g to %g", static_cast<f64>(lo), static_cast<f64>(hi));
        return arg_missing(c, name, e.c_str());
    }
    out = static_cast<f32>(v.as_number());
    return true;
}

// `colors`: an array of "#rrggbb" strings.
bool arg_colors(ToolCall& c, const char* name, Array<u32>& out, usize max) {
    const cook::JsonValue v = arg(c, name);
    if (!v.valid()) return true;
    if (!v.is_array() || v.size() > max) return arg_missing(c, name, "an array of \"#rrggbb\" colors");
    for (u32 i = 0; i < v.size(); ++i) {
        u32 rgba = 0;
        if (!v.at(i).is_string() || !gen::parse_hex(v.at(i).as_string(), rgba)) return arg_missing(c, name, "an array of \"#rrggbb\" colors");
        out.push_back(rgba);
    }
    return true;
}

bool arg_choice(ToolCall& c, const char* name, StringView& out, bool (*known)(StringView), const char* expected) {
    if (!arg_string(c, name, out, false)) return false;
    if (!known(out)) return arg_missing(c, name, expected);
    return true;
}

void write_colors(JsonWriter& w, Span<const u32> colors) {
    w.begin_array();
    for (const u32 rgba : colors) {
        cook::String hex;
        gen::to_hex(rgba, hex);
        w.str(hex.view());
    }
    w.end_array();
}

// ---------------------------------------------------------------- previews

bool attach(ToolCall& c, const cook::Image& img) {
    if (!c.content) return true;
    ToolContent item;
    item.mime = cook::String("image/png");
    if (!png_base64(img, item.base64)) return fail(c, "png encode failed");
    c.content->push_back(static_cast<ToolContent&&>(item));
    return true;
}

// Pixel art shown big enough to judge: nearest-neighbour upscale to about `target` pixels, on a
// checkerboard so transparency reads.
cook::Image enlarge(const cook::Image& src, u32 target) {
    const u32 longest = src.width > src.height ? src.width : src.height;
    u32 s = longest ? target / longest : 1;
    if (s < 1) s = 1;
    if (s > 16) s = 16;
    cook::Image out = make_image(src.width * s, src.height * s, 0);
    for (u32 y = 0; y < out.height; ++y) {
        for (u32 x = 0; x < out.width; ++x) {
            const u8* p = &src.rgba[(usize(y / s) * src.width + x / s) * 4];
            const u32 check = ((x / 8 + y / 8) % 2) ? 0x3A3A3AFFu : 0x2A2A2AFFu;
            const f32 a = static_cast<f32>(p[3]) / 255.0f;
            u8* q = &out.rgba[(usize(y) * out.width + x) * 4];
            for (u32 ch = 0; ch < 3; ++ch) {
                const f32 bg = static_cast<f32>((check >> (24 - 8 * ch)) & 255);
                q[ch] = static_cast<u8>(std::lround(static_cast<f32>(p[ch]) * a + bg * (1.0f - a)));
            }
            q[3] = 255;
        }
    }
    return out;
}

struct SoundStats {
    f64 seconds = 0, peak = 0, rms = 0;
};

SoundStats sound_stats(Span<const i16> s) {
    SoundStats st;
    st.seconds = static_cast<f64>(s.size()) / gen::kSampleRate;
    f64 sum = 0;
    for (const i16 v : s) {
        const f64 a = std::fabs(static_cast<f64>(v) / 32768.0);
        if (a > st.peak) st.peak = a;
        sum += a * a;
    }
    st.rms = s.empty() ? 0 : std::sqrt(sum / static_cast<f64>(s.size()));
    return st;
}

// The waveform (min/max per column) with the length written on it: what an agent can "hear".
cook::Image waveform(Span<const i16> s) {
    constexpr u32 kW = 640, kH = 160;
    cook::Image img = make_image(kW, kH, 0x1E1E24FFu);
    fill_rect(img, 0, kH / 2, kW, 1, 0x50505AFFu);
    const usize n = s.size();
    for (u32 x = 0; x < kW && n; ++x) {
        const usize a = n * x / kW, b = n * (x + 1) / kW > a ? n * (x + 1) / kW : a + 1;
        i32 lo = 0, hi = 0;
        for (usize i = a; i < b && i < n; ++i) {
            if (s[i] < lo) lo = s[i];
            if (s[i] > hi) hi = s[i];
        }
        const i32 y0 = static_cast<i32>(kH / 2) - hi * static_cast<i32>(kH / 2 - 4) / 32768;
        const i32 y1 = static_cast<i32>(kH / 2) - lo * static_cast<i32>(kH / 2 - 4) / 32768;
        fill_rect(img, static_cast<i32>(x), y0, 1, y1 - y0 + 1, 0x6FD3FFFFu);
    }
    cook::String label;
    cook::appendf(label, "%.2fs", static_cast<f64>(n) / gen::kSampleRate);
    draw_label(img, 4, 4, label.view(), 2, 0xFFFFFFFFu);
    return img;
}

// ---------------------------------------------------------------- saving under assets/

bool valid_asset_rel(StringView rel) {
    if (rel.empty() || rel.size() > 400 || rel[0] == '/' || rel.find('\\') != StringView::npos || rel.find(':') != StringView::npos)
        return false;
    for (usize start = 0; start <= rel.size();) {
        usize end = rel.find('/', start);
        if (end == StringView::npos) end = rel.size();
        const StringView seg = rel.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == ".." || seg[0] == '.') return false;
        start = end + 1;
    }
    return true;
}

bool ends_with(StringView s, StringView suffix) { return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix; }

StringView extension(StringView rel) {
    const usize dot = rel.rfind('.');
    const usize slash = rel.rfind('/');
    if (dot == StringView::npos || (slash != StringView::npos && dot < slash)) return {};
    return rel.substr(dot);
}

// The project's CREDITS.md row for `asset_rel` (assets/...): replaced when there is one, else
// added after the table's last row (a new file gets the template's header).
bool credit(ToolCall& c, StringView asset_rel, StringView source, StringView license) {
    const cook::String path = cook::path_join(c.server.session().project().root(), "CREDITS.md");
    cook::String text;
    if (!cook::fs::read_text(path.view(), text))
        text = cook::String("# Content credits\n\n| Files | Source | License |\n|---|---|---|\n\n`mce ship` copies this file into the shipped folder.\n");
    cook::String row("| `assets/");
    row += asset_rel;
    row += "` |";
    cook::String line(row.view());
    line += ' ';
    line += source;
    line += " | ";
    line += license;
    line += " |";
    // Split into lines, replace or insert, join.
    Array<cook::String> lines;
    const StringView all = text.view();
    for (usize start = 0; start < all.size();) {
        usize end = all.find('\n', start);
        if (end == StringView::npos) end = all.size();
        lines.push_back(cook::String(all.substr(start, end - start)));
        start = end + 1;
    }
    bool done = false;
    usize last_row = lines.size();
    for (usize i = 0; i < lines.size(); ++i) {
        const StringView l = lines[i].view();
        if (l.size() >= row.size() && l.substr(0, row.size()) == row.view()) {
            lines[i] = line;
            done = true;
            break;
        }
        if (!l.empty() && l[0] == '|') last_row = i;
    }
    if (!done) {
        if (last_row == lines.size()) {
            lines.push_back(cook::String());
            lines.push_back(cook::String("| Files | Source | License |"));
            lines.push_back(cook::String("|---|---|---|"));
            last_row = lines.size() - 1;
        }
        lines.insert(last_row + 1, line);
    }
    cook::String out;
    for (const cook::String& l : lines) {
        out += l.view();
        out += '\n';
    }
    return static_cast<bool>(cook::fs::write_file_atomic(path.view(), Span<const byte>(reinterpret_cast<const byte*>(out.c_str()), out.size())));
}

struct Saved {
    cook::String rel;   // under assets/
    cook::String guid;  // "guid:<uuid>"
    bool replaced = false;
};

// Writes `bytes` at assets/<rel> with its .mceasset sidecar: an existing sidecar keeps its GUID (so
// scenes that reference the asset still find it) and gets `settings`. Adds the CREDITS.md row.
bool save_asset(ToolCall& c, StringView rel, Span<const byte> bytes, StringView settings, StringView source, StringView license, Saved& out,
                const char* sidecar_from = nullptr) {
    editor::EditorProject& project = c.server.session().project();
    const cook::String abs = project.asset_path(rel);
    const cook::IImporter* imp = project.importers().for_extension(cook::lower_extension(rel).view());
    if (!imp) return fail(c, "no importer takes that file type");
    cook::String sidecar_path(abs.view());
    sidecar_path += ".mceasset";
    asset::Guid guid = cook::generate_guid();
    out.replaced = cook::fs::exists(abs.view());
    cook::String sidecar_text;
    if (cook::fs::read_text(sidecar_path.view(), sidecar_text)) {
        cook::JsonDoc doc;
        cook::JsonError je;
        if (doc.parse(sidecar_text.view(), je) && doc.root().get("guid").is_string())
            if (const Result<asset::Guid> g = asset::parse_guid(doc.root().get("guid").as_string())) guid = g.value();
    }
    // A copied library file keeps its source sidecar's settings (a fresh GUID all the same).
    cook::String copied_settings;
    if (sidecar_from) {
        cook::String src_text;
        cook::JsonDoc doc;
        cook::JsonError je;
        if (cook::fs::read_text(sidecar_from, src_text) && doc.parse(src_text.view(), je) && doc.root().get("settings").is_object()) {
            JsonWriter sw(copied_settings);
            sw.copy(doc.root().get("settings"));
            settings = copied_settings.view();
        }
    }
    if (!cook::fs::write_file_atomic(abs.view(), bytes)) return fail(c, "could not write the file under assets/");
    cook::String s("{\n  \"guid\": \"");
    s += asset::to_string(guid).view();
    s += "\",\n  \"importer\": \"";
    s += imp->id();
    cook::appendf(s, "\",\n  \"importer_version\": %u,\n  \"settings\": ", imp->version());
    s += settings.empty() ? StringView("{}") : settings;
    s += "\n}\n";
    if (!cook::fs::write_file_atomic(sidecar_path.view(), Span<const byte>(reinterpret_cast<const byte*>(s.c_str()), s.size())))
        return fail(c, "could not write the .mceasset sidecar");
    if (!credit(c, rel, source, license)) return fail(c, "could not update CREDITS.md");
    project.rescan();
    out.rel = cook::String(rel);
    out.guid = cook::String("guid:");
    out.guid += asset::to_string(guid).view();
    return true;
}

// The optional `save` argument: a path under assets/ ending in `ext`. Empty: not saving.
bool arg_save(ToolCall& c, const char* ext, StringView& rel) {
    if (!arg_string(c, "save", rel, false)) return false;
    if (rel.empty()) return true;
    if (!need_project(c)) return false;
    if (!valid_asset_rel(rel) || !ends_with(rel, ext)) {
        c.error += "argument 'save' must be a path under assets/ ending in ";
        c.error += ext;
        c.error += ", like \"textures/hero";
        c.error += ext;
        c.error += "\" (forward slashes, no '..')";
        return false;
    }
    bool overwrite = false;
    if (!arg_bool(c, "overwrite", overwrite)) return false;
    if (!overwrite && cook::fs::exists(c.server.session().project().asset_path(rel).view()))
        return fail(c, "that file already exists under assets/: pass overwrite: true to replace it (its GUID is kept)");
    return true;
}

void write_saved(JsonWriter& w, const Saved& s) {
    w.key("saved").begin_object().key("path").str(s.rel.view()).key("guid").str(s.guid.view()).key("replaced").boolean(s.replaced).end_object();
}

bool save_png(ToolCall& c, StringView rel, const cook::Image& img, StringView settings, StringView source, Saved& out) {
    Array<byte> png;
    if (!cook::encode_png(img.width, img.height, img.rgba.span(), png)) return fail(c, "png encode failed");
    return save_asset(c, rel, png.span(), settings, source, "the project's (generated)", out);
}

bool save_wav(ToolCall& c, StringView rel, Span<const i16> samples, StringView source, Saved& out) {
    Array<byte> wav;
    gen::wav_bytes(samples, gen::kSampleRate, wav);
    return save_asset(c, rel, wav.span(), "{}", source, "the project's (generated)", out);
}

constexpr const char* kPixelSettings = "{\"srgb\": true, \"mips\": false, \"filter\": \"nearest\"}";
constexpr const char* kSmoothSettings = "{\"srgb\": true, \"mips\": true, \"filter\": \"linear\"}";

// The colors a sprite or tile set uses: `colors`, else a palette of `scheme` from the seed.
bool sprite_colors(ToolCall& c, u64 seed, u32 count, Array<u32>& colors) {
    if (!arg_colors(c, "colors", colors, 16)) return false;
    if (!colors.empty()) return true;
    StringView scheme = "analogous";
    if (!arg_choice(c, "scheme", scheme, gen::palette_scheme_known, "a palette scheme: analogous, complementary, triad, pastel, earth, neon or mono"))
        return false;
    colors = gen::palette(seed * 31 + 7, count, scheme);
    return true;
}

// ---------------------------------------------------------------- sfx parameters by name

struct SfxField {
    const char* name;
    f32 gen::SfxParams::* member;
    f32 lo, hi;
};

const SfxField kSfxFields[] = {
    {"base_freq", &gen::SfxParams::base_freq, 0, 1},       {"freq_limit", &gen::SfxParams::freq_limit, 0, 1},
    {"freq_ramp", &gen::SfxParams::freq_ramp, -1, 1},      {"freq_dramp", &gen::SfxParams::freq_dramp, -1, 1},
    {"duty", &gen::SfxParams::duty, 0, 1},                 {"duty_ramp", &gen::SfxParams::duty_ramp, -1, 1},
    {"vib_strength", &gen::SfxParams::vib_strength, 0, 1}, {"vib_speed", &gen::SfxParams::vib_speed, 0, 1},
    {"env_attack", &gen::SfxParams::env_attack, 0, 1},     {"env_sustain", &gen::SfxParams::env_sustain, 0, 1},
    {"env_punch", &gen::SfxParams::env_punch, 0, 1},       {"env_decay", &gen::SfxParams::env_decay, 0, 1},
    {"lpf_freq", &gen::SfxParams::lpf_freq, 0, 1},         {"lpf_ramp", &gen::SfxParams::lpf_ramp, -1, 1},
    {"lpf_resonance", &gen::SfxParams::lpf_resonance, 0, 1}, {"hpf_freq", &gen::SfxParams::hpf_freq, 0, 1},
    {"hpf_ramp", &gen::SfxParams::hpf_ramp, -1, 1},        {"pha_offset", &gen::SfxParams::pha_offset, -1, 1},
    {"pha_ramp", &gen::SfxParams::pha_ramp, -1, 1},        {"repeat_speed", &gen::SfxParams::repeat_speed, 0, 1},
    {"arp_speed", &gen::SfxParams::arp_speed, 0, 1},       {"arp_mod", &gen::SfxParams::arp_mod, -1, 1},
    {"volume", &gen::SfxParams::volume, 0, 1},
};

constexpr const char* kWaves[] = {"square", "saw", "sine", "noise"};

bool apply_sfx_params(ToolCall& c, cook::JsonValue v, gen::SfxParams& p) {
    if (!v.valid()) return true;
    if (!v.is_object()) return arg_missing(c, "params", "an object of sfxr parameters");
    for (u32 i = 0; i < v.size(); ++i) {
        const StringView k = v.key_at(i);
        const cook::JsonValue x = v.at(i);
        if (k == "wave") {
            bool ok = false;
            for (i32 w = 0; w < 4; ++w)
                if (x.is_string() && x.as_string() == kWaves[w]) {
                    p.wave = w;
                    ok = true;
                }
            if (!ok) return fail(c, "params.wave must be square, saw, sine or noise");
            continue;
        }
        const SfxField* f = nullptr;
        for (const SfxField& sf : kSfxFields)
            if (k == sf.name) f = &sf;
        if (!f) {
            c.error += "params: unknown sfxr parameter '";
            c.error += k;
            c.error += "' (the result's 'params' lists them)";
            return false;
        }
        if (!x.is_number() || x.as_number() < f->lo || x.as_number() > f->hi) {
            cook::appendf(c.error, "params.%s must be a number from %g to %g", f->name, static_cast<f64>(f->lo), static_cast<f64>(f->hi));
            return false;
        }
        p.*(f->member) = static_cast<f32>(x.as_number());
    }
    return true;
}

void write_sfx_params(JsonWriter& w, const gen::SfxParams& p) {
    w.begin_object();
    w.key("wave").str(kWaves[p.wave >= 0 && p.wave < 4 ? p.wave : 0]);
    for (const SfxField& f : kSfxFields) w.key(f.name).num_f(std::round(static_cast<f64>(p.*(f.member)) * 10000.0) / 10000.0);
    w.end_object();
}

// ---------------------------------------------------------------- the library

struct LibraryEntry {
    cook::String id, kind, title, author, source, license, tool;
    Array<cook::String> tags, files, deps;
    cook::String args;  // a recipe's generator arguments (JSON)
};

struct Library {
    cook::String root;  // the folder holding library.json
    cook::String error;
    Array<LibraryEntry> entries;
};

cook::String json_str(cook::JsonValue o, StringView key) {
    const cook::JsonValue v = o.get(key);
    return v.is_string() ? cook::String(v.as_string()) : cook::String();
}

void json_strs(cook::JsonValue o, StringView key, Array<cook::String>& out) {
    const cook::JsonValue v = o.get(key);
    for (u32 i = 0; v.is_array() && i < v.size(); ++i)
        if (v.at(i).is_string()) out.push_back(cook::String(v.at(i).as_string()));
}

// Loaded once per library folder.
const Library& library(const Server& server) {
    static Library lib;
    static bool loaded = false;
    const cook::String root = content_root(server, "library");
    if (loaded && lib.root.view() == root.view()) return lib;
    loaded = true;
    lib = Library();
    lib.root = root;
    const cook::String path = cook::path_join(lib.root.view(), "library.json");
    cook::String text;
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!cook::fs::read_text(path.view(), text)) {
        lib.error = cook::String("the content library (library.json) is not installed beside this server");
        return lib;
    }
    if (!doc.parse(text.view(), je) || !doc.root().get("entries").is_array()) {
        lib.error = cook::String("library.json is not valid");
        return lib;
    }
    const cook::JsonValue entries = doc.root().get("entries");
    for (u32 i = 0; i < entries.size(); ++i) {
        const cook::JsonValue o = entries.at(i);
        LibraryEntry e;
        e.id = json_str(o, "id");
        e.kind = json_str(o, "kind");
        e.title = json_str(o, "title");
        e.author = json_str(o, "author");
        e.source = json_str(o, "source");
        e.license = json_str(o, "license");
        e.tool = json_str(o, "tool");
        json_strs(o, "tags", e.tags);
        json_strs(o, "files", e.files);
        json_strs(o, "deps", e.deps);
        if (o.get("args").is_object()) {
            JsonWriter w(e.args);
            w.copy(o.get("args"));
        }
        lib.entries.push_back(static_cast<LibraryEntry&&>(e));
    }
    return lib;
}

// "samples/04_diorama/x.glb" -> the installed (or source) file.
cook::String library_file(const Server& server, StringView path) {
    const usize slash = path.find('/');
    if (slash == StringView::npos) return {};
    const cook::String root = content_root(server, path.substr(0, slash));
    if (root.empty()) return {};
    return cook::path_join(root.view(), path.substr(slash + 1));
}

char lower(char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }

bool contains_ci(StringView hay, StringView needle) {
    if (needle.empty()) return true;
    for (usize i = 0; i + needle.size() <= hay.size(); ++i) {
        usize j = 0;
        while (j < needle.size() && lower(hay[i + j]) == lower(needle[j])) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

u32 entry_score(const LibraryEntry& e, StringView query) {
    u32 score = 0;
    for (usize start = 0; start < query.size();) {
        usize end = query.find(' ', start);
        if (end == StringView::npos) end = query.size();
        const StringView word = query.substr(start, end - start);
        start = end + 1;
        if (word.empty()) continue;
        u32 s = 0;
        if (contains_ci(e.id.view(), word)) s = 3;
        if (contains_ci(e.title.view(), word)) s = s > 3 ? s : 3;
        for (const cook::String& t : e.tags)
            if (contains_ci(t.view(), word)) s = s > 2 ? s : 2;
        if (contains_ci(e.kind.view(), word) || contains_ci(e.author.view(), word)) s = s > 1 ? s : 1;
        if (!s) return 0;  // every word must match somewhere
        score += s;
    }
    return query.empty() ? 1 : score;
}

const char* default_folder(StringView kind) {
    if (kind == "model") return "models/";
    if (kind == "sprite") return "sprites/";
    if (kind == "sound") return "sfx/";
    if (kind == "music") return "music/";
    if (kind == "font") return "fonts/";
    return "textures/";
}

// The tools a recipe may run (library.json "tool").
ToolFn recipe_tool(StringView name);

}  // namespace

// ---------------------------------------------------------------- the tools

bool gen_palette(ToolCall& c) {
    u64 seed = 1, count = 6;
    StringView scheme = "analogous", save;
    if (!arg_seed(c, seed) || !arg_uint(c, "count", count, 2, 32, false) ||
        !arg_choice(c, "scheme", scheme, gen::palette_scheme_known, "analogous, complementary, triad, pastel, earth, neon or mono") ||
        !arg_save(c, ".png", save))
        return false;
    const Array<u32> colors = gen::palette(seed, static_cast<u32>(count), scheme);
    cook::Image strip = make_image(static_cast<u32>(count), 1, 0);
    for (u32 i = 0; i < colors.size(); ++i) fill_rect(strip, static_cast<i32>(i), 0, 1, 1, colors[i]);
    Saved saved;
    if (!save.empty()) {
        cook::String src;
        cook::appendf(src, "generated by gen_palette (seed %llu, %.*s, %llu colors)", static_cast<unsigned long long>(seed),
                      static_cast<int>(scheme.size()), scheme.data(), static_cast<unsigned long long>(count));
        if (!save_png(c, save, strip, kPixelSettings, src.view(), saved)) return false;
    }
    c.out.begin_object().key("seed").num_u(seed).key("scheme").str(scheme).key("colors");
    write_colors(c.out, colors.span());
    if (!save.empty()) write_saved(c.out, saved);
    c.out.end_object();
    cook::Image swatch = make_image(static_cast<u32>(count) * 48, 48, 0);
    for (u32 i = 0; i < colors.size(); ++i) fill_rect(swatch, static_cast<i32>(i * 48), 0, 48, 48, colors[i]);
    return attach(c, swatch);
}

bool gen_sprite(ToolCall& c) {
    u64 seed = 1, w = 16, h = 16, variants = 1;
    StringView kind = "creature", save;
    if (!arg_seed(c, seed) || !arg_choice(c, "kind", kind, gen::sprite_kind_known, "creature, character, ship, item or blob") ||
        !arg_uint(c, "width", w, 4, 128, false) || !arg_uint(c, "height", h, 4, 128, false) || !arg_uint(c, "variants", variants, 1, 16, false) ||
        !arg_save(c, ".png", save))
        return false;
    Array<u32> colors;
    if (!sprite_colors(c, seed, 4, colors)) return false;
    if (colors.size() < 3) return fail(c, "argument 'colors' needs at least 3 colors (dark to light; the darkest outlines)");
    const u32 sw = static_cast<u32>(w), sh = static_cast<u32>(h), n = static_cast<u32>(variants);
    cook::Image sheet = make_image(sw * n, sh, 0);
    for (u32 i = 0; i < n; ++i) blit(sheet, gen::pixel_sprite(seed + i, kind, sw, sh, colors.span()), static_cast<i32>(i * sw), 0);
    Saved saved;
    if (!save.empty()) {
        cook::String src;
        cook::appendf(src, "generated by gen_sprite (seed %llu, %.*s %ux%u", static_cast<unsigned long long>(seed), static_cast<int>(kind.size()),
                      kind.data(), sw, sh);
        if (n > 1) cook::appendf(src, ", %u variants", n);
        src += ')';
        if (!save_png(c, save, sheet, kPixelSettings, src.view(), saved)) return false;
    }
    c.out.begin_object().key("seed").num_u(seed).key("kind").str(kind).key("width").num_u(sw).key("height").num_u(sh);
    c.out.key("colors");
    write_colors(c.out, colors.span());
    c.out.key("frames").begin_array();
    for (u32 i = 0; i < n; ++i)
        c.out.begin_object().key("seed").num_u(seed + i).key("x").num_u(i * sw).key("y").num_u(0).key("w").num_u(sw).key("h").num_u(sh).end_object();
    c.out.end_array();
    if (!save.empty()) write_saved(c.out, saved);
    c.out.end_object();
    return attach(c, enlarge(sheet, n > 1 ? 768 : 256));
}

// A sprite drawn as text: one string per row, one character per pixel, `palette` maps characters
// to colors ('.' and ' ' are transparent). For the shapes gen_sprite's seeds will not give.
bool gen_pixels(ToolCall& c) {
    StringView save;
    if (!arg_save(c, ".png", save)) return false;
    const cook::JsonValue rows = arg(c, "rows");
    const cook::JsonValue pal = arg(c, "palette");
    if (!rows.is_array() || rows.size() == 0 || rows.size() > 256 || !rows.at(0).is_string())
        return arg_missing(c, "rows", "1-256 strings of equal length, one character per pixel");
    const usize w = rows.at(0).as_string().size();
    if (w == 0 || w > 256) return arg_missing(c, "rows", "rows of 1-256 characters");
    if (!pal.is_object() || pal.size() == 0) return arg_missing(c, "palette", "an object of one-character keys to \"#rrggbb\" or \"#rrggbbaa\"");
    u32 table[256] = {};
    bool known[256] = {};
    known[static_cast<u8>('.')] = known[static_cast<u8>(' ')] = true;
    for (u32 i = 0; i < pal.size(); ++i) {
        const StringView k = pal.key_at(i);
        u32 rgba = 0;
        if (k.size() != 1 || !pal.at(i).is_string() || !gen::parse_hex(pal.at(i).as_string(), rgba))
            return arg_missing(c, "palette", "an object of one-character keys to \"#rrggbb\" or \"#rrggbbaa\"");
        table[static_cast<u8>(k[0])] = rgba;
        known[static_cast<u8>(k[0])] = true;
    }
    const u32 iw = static_cast<u32>(w), ih = rows.size();
    cook::Image img = make_image(iw, ih, 0);
    for (u32 y = 0; y < ih; ++y) {
        const cook::JsonValue r = rows.at(y);
        if (!r.is_string() || r.as_string().size() != w) {
            cook::appendf(c.error, "argument 'rows': row %u is not a string of %u characters like row 0", y, iw);
            return false;
        }
        const StringView line = r.as_string();
        for (u32 x = 0; x < iw; ++x) {
            const u8 ch = static_cast<u8>(line[x]);
            if (!known[ch]) {
                cook::appendf(c.error, "argument 'rows': '%c' (row %u, column %u) is not in 'palette' ('.' and ' ' are transparent)",
                              static_cast<char>(ch), y, x);
                return false;
            }
            const u32 rgba = table[ch];
            u8* px = &img.rgba[(static_cast<usize>(y) * iw + x) * 4];
            px[0] = static_cast<u8>(rgba >> 24);
            px[1] = static_cast<u8>(rgba >> 16);
            px[2] = static_cast<u8>(rgba >> 8);
            px[3] = static_cast<u8>(rgba);
        }
    }
    Saved saved;
    if (!save.empty()) {
        cook::String src;
        cook::appendf(src, "drawn with gen_pixels (%ux%u)", iw, ih);
        if (!save_png(c, save, img, kPixelSettings, src.view(), saved)) return false;
    }
    c.out.begin_object().key("width").num_u(iw).key("height").num_u(ih);
    if (!save.empty()) write_saved(c.out, saved);
    c.out.end_object();
    return attach(c, enlarge(img, 256));
}

bool gen_tileset(ToolCall& c) {
    u64 seed = 1, tile = 16;
    StringView style = "grass", save;
    if (!arg_seed(c, seed) ||
        !arg_choice(c, "style", style, gen::tile_style_known, "grass, stone, brick, dirt, sand, snow, water, wood or metal") ||
        !arg_uint(c, "tile", tile, 8, 64, false) || !arg_save(c, ".png", save))
        return false;
    Array<u32> colors;
    if (!arg_colors(c, "colors", colors, 7)) return false;
    if (!colors.empty() && colors.size() != 4 && colors.size() != 7)
        return fail(c, "argument 'colors' takes 4 fill colors (dark to light), or 4 fill + 3 surface colors");
    const u32 t = static_cast<u32>(tile);
    const cook::Image img = gen::tileset(seed, style, t, colors.span());
    Saved saved;
    if (!save.empty()) {
        cook::String src;
        cook::appendf(src, "generated by gen_tileset (seed %llu, %.*s, %upx tiles)", static_cast<unsigned long long>(seed),
                      static_cast<int>(style.size()), style.data(), t);
        if (!save_png(c, save, img, kPixelSettings, src.view(), saved)) return false;
    }
    c.out.begin_object().key("seed").num_u(seed).key("style").str(style).key("tile").num_u(t);
    c.out.key("tiles").begin_array();
    for (u32 i = 0; i < gen::kTileCount; ++i)
        c.out.begin_object().key("name").str(gen::kTileNames[i]).key("x").num_u(i * t).key("y").num_u(0).key("w").num_u(t).key("h").num_u(t).end_object();
    c.out.end_array();
    if (!save.empty()) write_saved(c.out, saved);
    c.out.end_object();
    return attach(c, enlarge(img, 1024));
}

bool gen_texture(ToolCall& c) {
    u64 seed = 1, w = 256, h = 256, octaves = 5;
    gen::NoiseDesc d;
    StringView kind = "fbm", save;
    if (!arg_seed(c, seed) || !arg_choice(c, "kind", kind, gen::noise_kind_known, "value, perlin, fbm, ridged, worley or sky") ||
        !arg_uint(c, "width", w, 16, 2048, false) || !arg_uint(c, "height", h, 16, 2048, false) || !arg_uint(c, "octaves", octaves, 1, 8, false) ||
        !arg_f32(c, "scale", d.scale, 0.5f, 64.0f) || !arg_bool(c, "tileable", d.tileable) || !arg_f32(c, "stars", d.stars, 0, 1) ||
        !arg_f32(c, "clouds", d.clouds, 0, 1) || !arg_colors(c, "colors", d.colors, 8) || !arg_save(c, ".png", save))
        return false;
    d.kind = cook::String(kind);
    d.width = static_cast<u32>(w);
    d.height = static_cast<u32>(h);
    d.octaves = static_cast<u32>(octaves);
    if (kind == "sky" && arg(c, "width").valid() && arg(c, "height").valid() && d.width != d.height * 2)
        return fail(c, "a sky is an equirect panorama: width must be twice the height (e.g. 1024 x 512)");
    if (kind == "sky" && !arg(c, "width").valid()) {
        d.width = 1024;
        d.height = 512;
    }
    const cook::Image img = gen::noise_texture(seed, d);
    Saved saved;
    if (!save.empty()) {
        cook::String src;
        cook::appendf(src, "generated by gen_texture (seed %llu, %.*s %ux%u)", static_cast<unsigned long long>(seed), static_cast<int>(kind.size()),
                      kind.data(), d.width, d.height);
        if (!save_png(c, save, img, kSmoothSettings, src.view(), saved)) return false;
    }
    c.out.begin_object().key("seed").num_u(seed).key("kind").str(kind).key("width").num_u(d.width).key("height").num_u(d.height);
    c.out.key("tileable").boolean(kind != "sky" && d.tileable);
    if (!save.empty()) write_saved(c.out, saved);
    c.out.end_object();
    if (img.width <= 512 && img.height <= 512) return attach(c, img);
    // Preview at most 512 wide (nearest sampling is enough to judge it).
    const u32 step = (img.width + 511) / 512;
    cook::Image small = make_image(img.width / step, img.height / step, 0);
    for (u32 y = 0; y < small.height; ++y)
        for (u32 x = 0; x < small.width; ++x)
            for (u32 ch = 0; ch < 4; ++ch)
                small.rgba[(usize(y) * small.width + x) * 4 + ch] = img.rgba[(usize(y * step) * img.width + x * step) * 4 + ch];
    return attach(c, small);
}

bool gen_sfx(ToolCall& c) {
    u64 seed = 1;
    StringView preset = "pickup", save;
    if (!arg_seed(c, seed) ||
        !arg_choice(c, "preset", preset, gen::sfx_preset_known, "pickup, laser, explosion, powerup, hit, jump, blip or random") ||
        !arg_save(c, ".wav", save))
        return false;
    gen::SfxParams p = gen::sfx_preset(seed, preset);
    if (!apply_sfx_params(c, arg(c, "params"), p)) return false;
    const Array<i16> samples = gen::synth_sfx(p, seed);
    Saved saved;
    if (!save.empty()) {
        cook::String src;
        cook::appendf(src, "generated by gen_sfx (seed %llu, %.*s)", static_cast<unsigned long long>(seed), static_cast<int>(preset.size()),
                      preset.data());
        if (!save_wav(c, save, samples.span(), src.view(), saved)) return false;
    }
    const SoundStats st = sound_stats(samples.span());
    c.out.begin_object().key("seed").num_u(seed).key("preset").str(preset);
    c.out.key("seconds").num_f(std::round(st.seconds * 1000.0) / 1000.0).key("peak").num_f(std::round(st.peak * 1000.0) / 1000.0);
    c.out.key("rms").num_f(std::round(st.rms * 1000.0) / 1000.0).key("sample_rate").num_u(gen::kSampleRate);
    c.out.key("params");
    write_sfx_params(c.out, p);
    if (!save.empty()) write_saved(c.out, saved);
    c.out.end_object();
    return attach(c, waveform(samples.span()));
}

bool gen_music(ToolCall& c) {
    u64 seed = 1, bpm = 120, bars = 8, root = 0;
    gen::MusicDesc d;
    StringView scale = "major", save;
    if (!arg_seed(c, seed) || !arg_uint(c, "bpm", bpm, 60, 200, false) || !arg_uint(c, "bars", bars, 1, 32, false) ||
        !arg_uint(c, "root", root, 0, 11, false) ||
        !arg_choice(c, "scale", scale, gen::music_scale_known, "major, minor, dorian, pentatonic or blues") || !arg_bool(c, "drums", d.drums) ||
        !arg_f32(c, "volume", d.volume, 0, 1) || !arg_save(c, ".wav", save))
        return false;
    d.bpm = static_cast<u32>(bpm);
    d.bars = static_cast<u32>(bars);
    d.root = static_cast<u32>(root);
    d.scale = cook::String(scale);
    const f64 seconds = static_cast<f64>(d.bars) * 4.0 * 60.0 / static_cast<f64>(d.bpm);
    if (seconds > 60.0) return fail(c, "the loop would be longer than 60 seconds: fewer bars or a faster bpm");
    const Array<i16> samples = gen::music_loop(seed, d);
    Saved saved;
    if (!save.empty()) {
        cook::String src;
        cook::appendf(src, "generated by gen_music (seed %llu, %.*s, %u bpm, %u bars)", static_cast<unsigned long long>(seed),
                      static_cast<int>(scale.size()), scale.data(), d.bpm, d.bars);
        if (!save_wav(c, save, samples.span(), src.view(), saved)) return false;
    }
    const SoundStats st = sound_stats(samples.span());
    c.out.begin_object().key("seed").num_u(seed).key("scale").str(scale).key("root").num_u(d.root).key("bpm").num_u(d.bpm).key("bars").num_u(d.bars);
    c.out.key("seconds").num_f(std::round(st.seconds * 1000.0) / 1000.0).key("peak").num_f(std::round(st.peak * 1000.0) / 1000.0);
    c.out.key("loop").boolean(true);
    if (!save.empty()) write_saved(c.out, saved);
    c.out.end_object();
    return attach(c, waveform(samples.span()));
}

bool library_search(ToolCall& c) {
    StringView query, kind, license;
    u64 limit = 20;
    if (!arg_string(c, "query", query, false) || !arg_string(c, "kind", kind, false) || !arg_string(c, "license", license, false) ||
        !arg_uint(c, "limit", limit, 1, 100, false))
        return false;
    const Library& lib = library(c.server);
    if (!lib.error.empty()) return fail(c, lib.error.view());
    struct Hit {
        u32 score;
        usize index;
    };
    Array<Hit> hits;
    for (usize i = 0; i < lib.entries.size(); ++i) {
        const LibraryEntry& e = lib.entries[i];
        if (!kind.empty() && e.kind.view() != kind && !(kind == "recipe" && !e.tool.empty())) continue;
        if (!license.empty() && !contains_ci(e.license.view(), license)) continue;
        if (const u32 s = entry_score(e, query)) hits.push_back({s, i});
    }
    // Best first, library order among equals (a stable insertion sort; the library is small).
    for (usize i = 1; i < hits.size(); ++i)
        for (usize j = i; j > 0 && hits[j].score > hits[j - 1].score; --j) {
            const Hit t = hits[j];
            hits[j] = hits[j - 1];
            hits[j - 1] = t;
        }
    c.out.begin_object().key("total").num_u(hits.size()).key("entries").begin_array();
    for (usize i = 0; i < hits.size() && i < limit; ++i) {
        const LibraryEntry& e = lib.entries[hits[i].index];
        c.out.begin_object().key("id").str(e.id.view()).key("kind").str(e.kind.view()).key("title").str(e.title.view());
        c.out.key("license").str(e.license.view());
        if (!e.author.empty()) c.out.key("author").str(e.author.view());
        c.out.key("tags").begin_array();
        for (const cook::String& t : e.tags) c.out.str(t.view());
        c.out.end_array();
        if (!e.tool.empty()) c.out.key("tool").str(e.tool.view()).key("args").raw(e.args.empty() ? StringView("{}") : e.args.view());
        else {
            c.out.key("files").begin_array();
            for (const cook::String& f : e.files) {
                const usize slash = f.view().rfind('/');
                c.out.str(slash == StringView::npos ? f.view() : f.view().substr(slash + 1));
            }
            c.out.end_array();
        }
        c.out.end_object();
    }
    c.out.end_array().end_object();
    return true;
}

bool library_add(ToolCall& c) {
    StringView id, dest;
    bool overwrite = false;
    if (!need_project(c) || !arg_string(c, "id", id, true) || !arg_string(c, "path", dest, false) || !arg_bool(c, "overwrite", overwrite)) return false;
    const Library& lib = library(c.server);
    if (!lib.error.empty()) return fail(c, lib.error.view());
    const LibraryEntry* e = nullptr;
    for (const LibraryEntry& x : lib.entries)
        if (x.id.view() == id) e = &x;
    if (!e) return fail(c, "no library entry with that id (library_search lists them)");

    if (!e->tool.empty()) {
        // A recipe: run its generator with `save` set.
        const ToolFn fn = recipe_tool(e->tool.view());
        if (!fn) return fail(c, "library.json names an unknown generator");
        cook::String rel(dest);
        if (rel.empty()) {
            rel = cook::String(default_folder(e->kind.view()));
            rel += e->id.view();
            rel += e->tool.view() == "gen_sfx" || e->tool.view() == "gen_music" ? ".wav" : ".png";
        }
        // The recipe's object without its closing brace, then save and overwrite.
        const StringView recipe = e->args.empty() ? StringView("{}") : e->args.view();
        cook::String args_text(recipe.substr(0, recipe.size() - 1));
        if (recipe.size() > 2) args_text += ',';
        cook::String extra;
        JsonWriter w(extra);
        w.begin_object().key("save").str(rel.view()).key("overwrite").boolean(overwrite).end_object();
        args_text += extra.view().substr(1);
        cook::JsonDoc doc;
        cook::JsonError je;
        if (!doc.parse(args_text.view(), je)) return fail(c, "library.json has a bad recipe");
        ToolCall inner{c.server, doc.root(), cook::JsonValue(), c.out, c.error, c.content};
        return fn(inner);
    }

    if (e->files.empty()) return fail(c, "library.json lists no files for that entry");
    const StringView primary = e->files[0].view();
    const usize pslash = primary.rfind('/');
    const StringView primary_dir = pslash == StringView::npos ? StringView() : primary.substr(0, pslash + 1);
    // Destination folder: `path` (a folder, or the primary file's new name) or the kind's folder.
    cook::String folder, first_name;
    if (dest.empty()) folder = cook::String(default_folder(e->kind.view()));
    else if (ends_with(dest, "/")) folder = cook::String(dest);
    else {
        const usize ds = dest.rfind('/');
        folder = cook::String(ds == StringView::npos ? StringView() : dest.substr(0, ds + 1));
        first_name = cook::String(ds == StringView::npos ? dest : dest.substr(ds + 1));
        if (extension(first_name.view()) != extension(primary)) return fail(c, "argument 'path' must keep the file's extension, or end in '/' for a folder");
    }
    if (!folder.empty() && !valid_asset_rel(folder.view().substr(0, folder.size() - 1)))
        return fail(c, "argument 'path' must be under assets/ (forward slashes, no '..')");

    editor::EditorProject& project = c.server.session().project();
    cook::String source(e->title.view());
    if (!e->author.empty()) {
        source += " by ";
        source += e->author.view();
    }
    if (!e->source.empty()) {
        source += " (";
        source += e->source.view();
        source += ')';
    }
    source += ", from the engine's content library (";
    source += e->id.view();
    source += ')';

    // The license cell names a license text that comes along (asset_delete removes it with the last font).
    cook::String license(e->license.view());
    for (const cook::String& f : e->deps) {
        const StringView n = f.view().substr(f.view().rfind('/') + 1);
        if (cook::lower_extension(n).view() != ".txt") continue;
        license += " (`";
        license += n;
        license += "`)";
    }
    c.out.begin_object().key("id").str(e->id.view()).key("license").str(e->license.view()).key("files").begin_array();
    usize index = 0;
    auto copy_one = [&](StringView from, bool is_dep) -> bool {
        // Files keep their place relative to the primary (a model's Textures/ folder).
        StringView rel_name = from.substr(from.rfind('/') + 1);
        if (from.size() > primary_dir.size() && from.substr(0, primary_dir.size()) == primary_dir) rel_name = from.substr(primary_dir.size());
        cook::String rel(folder.view());
        rel += index == 0 && !first_name.empty() ? first_name.view() : rel_name;
        ++index;
        const cook::String abs = project.asset_path(rel.view());
        if (cook::fs::exists(abs.view()) && (is_dep || !overwrite)) {
            if (!is_dep) {
                c.error += "assets/";
                c.error += rel.view();
                c.error += " already exists: pass overwrite: true to replace it (its GUID is kept)";
                return false;
            }
            c.out.begin_object().key("path").str(rel.view()).key("kept").boolean(true).end_object();
            return true;  // a shared dependency already there (two models, one colormap)
        }
        const cook::String src = library_file(c.server, from);
        cook::String bytes;
        if (src.empty() || !cook::fs::read_text(src.view(), bytes)) {
            c.error += "the library file is missing: ";
            c.error += from;
            return false;
        }
        const Span<const byte> data(reinterpret_cast<const byte*>(bytes.c_str()), bytes.size());
        if (!project.importers().for_extension(cook::lower_extension(rel.view()).view())) {
            // A license text or another file no importer takes: copied as it is.
            if (!cook::fs::write_file_atomic(abs.view(), data)) return fail(c, "could not write the file under assets/");
            c.out.begin_object().key("path").str(rel.view()).end_object();
            return true;
        }
        cook::String src_sidecar(src.view());
        src_sidecar += ".mceasset";
        Saved saved;
        if (!save_asset(c, rel.view(), data, "{}", source.view(), license.view(), saved,
                        cook::fs::exists(src_sidecar.view()) ? src_sidecar.c_str() : nullptr))
            return false;
        c.out.begin_object().key("path").str(rel.view()).key("guid").str(saved.guid.view()).end_object();
        return true;
    };
    for (const cook::String& f : e->files)
        if (!copy_one(f.view(), false)) return false;
    for (const cook::String& f : e->deps)
        if (!copy_one(f.view(), true)) return false;
    c.out.end_array().end_object();
    project.rescan();
    return true;
}

namespace {

ToolFn recipe_tool(StringView name) {
    if (name == "gen_palette") return gen_palette;
    if (name == "gen_sprite") return gen_sprite;
    if (name == "gen_tileset") return gen_tileset;
    if (name == "gen_texture") return gen_texture;
    if (name == "gen_sfx") return gen_sfx;
    if (name == "gen_music") return gen_music;
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------- gen_mesh

namespace {

// A primitive as glTF arrays: flat faces (box, ramp) duplicate their corners, round surfaces share
// them. Y is up; the shape is centred on the origin and `size` is its full extent.
struct MeshData {
    Array<f32> pos, nrm, uv;  // 3, 3, 2 per vertex
    Array<u32> idx;
    u32 vertex(f32 x, f32 y, f32 z, f32 nx, f32 ny, f32 nz, f32 u, f32 v) {
        const u32 i = static_cast<u32>(pos.size() / 3);
        pos.push_back(x), pos.push_back(y), pos.push_back(z);
        nrm.push_back(nx), nrm.push_back(ny), nrm.push_back(nz);
        uv.push_back(u), uv.push_back(v);
        return i;
    }
    void tri(u32 a, u32 b, u32 c) { idx.push_back(a), idx.push_back(b), idx.push_back(c); }
    // A flat quad a b c d (counter-clockwise seen from outside) with its own normal.
    void quad(const f32* a, const f32* b, const f32* c, const f32* d) {
        const f32 ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = d[0] - a[0], vy = d[1] - a[1], vz = d[2] - a[2];
        f32 nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 0) nx /= len, ny /= len, nz /= len;
        const u32 i0 = vertex(a[0], a[1], a[2], nx, ny, nz, 0, 1), i1 = vertex(b[0], b[1], b[2], nx, ny, nz, 1, 1);
        const u32 i2 = vertex(c[0], c[1], c[2], nx, ny, nz, 1, 0), i3 = vertex(d[0], d[1], d[2], nx, ny, nz, 0, 0);
        tri(i0, i1, i2), tri(i0, i2, i3);
    }
    void flat_tri(const f32* a, const f32* b, const f32* c) {
        const f32 ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        f32 nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 0) nx /= len, ny /= len, nz /= len;
        tri(vertex(a[0], a[1], a[2], nx, ny, nz, 0, 1), vertex(b[0], b[1], b[2], nx, ny, nz, 1, 1), vertex(c[0], c[1], c[2], nx, ny, nz, 0.5f, 0));
    }
};

bool mesh_shape_known(StringView s) {
    return s == "box" || s == "plane" || s == "sphere" || s == "cylinder" || s == "cone" || s == "disc" || s == "ramp" ||
           s == "humanoid";
}

// An axis-aligned box centred on (cx, cy, cz).
void add_box(MeshData& m, f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz) {
    const f32 p[8][3] = {{cx - hx, cy - hy, cz - hz}, {cx + hx, cy - hy, cz - hz}, {cx + hx, cy + hy, cz - hz}, {cx - hx, cy + hy, cz - hz},
                         {cx - hx, cy - hy, cz + hz}, {cx + hx, cy - hy, cz + hz}, {cx + hx, cy + hy, cz + hz}, {cx - hx, cy + hy, cz + hz}};
    m.quad(p[4], p[5], p[6], p[7]);  // +Z
    m.quad(p[1], p[0], p[3], p[2]);  // -Z
    m.quad(p[5], p[1], p[2], p[6]);  // +X
    m.quad(p[0], p[4], p[7], p[3]);  // -X
    m.quad(p[7], p[6], p[2], p[3]);  // +Y
    m.quad(p[0], p[1], p[5], p[4]);  // -Y
}

// ---- the humanoid: a skinned figure of 13 boxes, one bone each (rigid skinning), with Idle and
// Walk clips. Centred like the other shapes (the feet at -height/2, so a CharacterController3D's
// capsule centre is its origin); forward is -Z. Sized from [width (shoulders), height, depth].
struct HumanoidBone {
    const char* name;
    i32 parent;
    f32 x, y;                     // the joint, model space: x in widths, y in heights from the feet
    f32 cx, cy, cz, hx, hy, hz;   // its box: centre (x in widths, y and z in heights), half extents (heights)
};
constexpr HumanoidBone kHumanoid[] = {
    {"hips", -1, 0.0f, 0.50f, 0.0f, 0.545f, 0.0f, 0.0f, 0.075f, 0.0f},  // hx, hz from width and depth below
    {"spine", 0, 0.0f, 0.62f, 0.0f, 0.72f, 0.0f, 0.0f, 0.10f, 0.0f},
    {"head", 1, 0.0f, 0.84f, 0.0f, 0.91f, 0.0f, 0.065f, 0.07f, 0.065f},
    {"upper_arm_l", 1, 0.5f, 0.80f, 0.5f, 0.715f, 0.0f, 0.035f, 0.085f, 0.035f},
    {"forearm_l", 3, 0.5f, 0.63f, 0.5f, 0.555f, 0.0f, 0.03f, 0.075f, 0.03f},
    {"upper_arm_r", 1, -0.5f, 0.80f, -0.5f, 0.715f, 0.0f, 0.035f, 0.085f, 0.035f},
    {"forearm_r", 5, -0.5f, 0.63f, -0.5f, 0.555f, 0.0f, 0.03f, 0.075f, 0.03f},
    {"thigh_l", 0, 0.2f, 0.48f, 0.2f, 0.375f, 0.0f, 0.045f, 0.105f, 0.045f},
    {"shin_l", 7, 0.2f, 0.27f, 0.2f, 0.16f, 0.0f, 0.04f, 0.11f, 0.04f},
    {"foot_l", 8, 0.2f, 0.05f, 0.2f, 0.025f, -0.03f, 0.04f, 0.025f, 0.07f},
    {"thigh_r", 0, -0.2f, 0.48f, -0.2f, 0.375f, 0.0f, 0.045f, 0.105f, 0.045f},
    {"shin_r", 10, -0.2f, 0.27f, -0.2f, 0.16f, 0.0f, 0.04f, 0.11f, 0.04f},
    {"foot_r", 11, -0.2f, 0.05f, -0.2f, 0.025f, -0.03f, 0.04f, 0.025f, 0.07f},
};
constexpr u32 kHumanoidBones = sizeof(kHumanoid) / sizeof(kHumanoid[0]);

void humanoid_joint(u32 b, f32 w, f32 h, f32 out[3]) {
    out[0] = kHumanoid[b].x * w;
    out[1] = (kHumanoid[b].y - 0.5f) * h;
    out[2] = 0.0f;
}

// The boxes; `joints` gets the bone of every vertex.
void build_humanoid(f32 w, f32 h, f32 d, MeshData& m, Array<u8>& joints) {
    for (u32 b = 0; b < kHumanoidBones; ++b) {
        const HumanoidBone& k = kHumanoid[b];
        f32 hx = k.hx * h, hz = k.hz * h;
        if (b == 0) hx = 0.32f * w, hz = 0.4f * d;  // the pelvis and the chest follow width and depth
        if (b == 1) hx = 0.42f * w, hz = 0.45f * d;
        const usize before = m.pos.size() / 3;
        add_box(m, k.cx * w, (k.cy - 0.5f) * h, k.cz * h, hx, k.hy * h, hz);
        for (usize v = before; v < m.pos.size() / 3; ++v) joints.push_back(static_cast<u8>(b));
    }
}

void build_mesh(StringView shape, f32 sx, f32 sy, f32 sz, u32 seg, MeshData& m) {
    constexpr f32 kPi = 3.14159265358979f;
    const f32 hx = sx * 0.5f, hy = sy * 0.5f, hz = sz * 0.5f;
    if (shape == "box") {
        add_box(m, 0, 0, 0, hx, hy, hz);
    } else if (shape == "plane") {
        const f32 a[3] = {-hx, 0, hz}, b[3] = {hx, 0, hz}, c[3] = {hx, 0, -hz}, d[3] = {-hx, 0, -hz};
        m.quad(a, b, c, d);
    } else if (shape == "ramp") {
        // A wedge: the full height at +X, nothing at -X; the slope faces up and towards -X.
        const f32 a[3] = {-hx, -hy, hz}, b[3] = {hx, -hy, hz}, c[3] = {hx, hy, hz};
        const f32 d[3] = {-hx, -hy, -hz}, e[3] = {hx, -hy, -hz}, f[3] = {hx, hy, -hz};
        m.flat_tri(a, b, c);      // +Z side
        m.flat_tri(e, d, f);      // -Z side
        m.quad(b, e, f, c);       // the +X back
        m.quad(d, e, b, a);       // the bottom
        m.quad(a, c, f, d);       // the slope
    } else if (shape == "sphere") {
        const u32 rings = math::max(seg / 2, 3u);
        for (u32 r = 0; r <= rings; ++r) {
            const f32 v = static_cast<f32>(r) / static_cast<f32>(rings), th = v * kPi;
            for (u32 s = 0; s <= seg; ++s) {
                const f32 u = static_cast<f32>(s) / static_cast<f32>(seg), ph = u * 2 * kPi;
                const f32 nx = std::sin(th) * std::cos(ph), ny = std::cos(th), nz = -std::sin(th) * std::sin(ph);
                m.vertex(nx * hx, ny * hy, nz * hz, nx, ny, nz, u, v);
            }
        }
        for (u32 r = 0; r < rings; ++r)
            for (u32 s = 0; s < seg; ++s) {
                const u32 a = r * (seg + 1) + s, b = a + seg + 1;
                if (r != 0) m.tri(a, b, a + 1);
                if (r != rings - 1) m.tri(a + 1, b, b + 1);
            }
    } else {  // cylinder, cone, disc: a round side (none for a disc) and caps
        const bool cone = shape == "cone", disc = shape == "disc";
        const f32 top = disc ? 0.0f : hy, bottom = disc ? 0.0f : -hy;
        if (!disc) {
            const f32 slope = cone ? hx / sy : 0.0f;  // the side normal tilts up on a cone
            for (u32 s = 0; s <= seg; ++s) {
                const f32 u = static_cast<f32>(s) / static_cast<f32>(seg), ph = u * 2 * kPi;
                const f32 cx = std::cos(ph), cz = -std::sin(ph);
                const f32 l = std::sqrt(1 + slope * slope);
                m.vertex(cx * hx, bottom, cz * hz, cx / l, slope / l, cz / l, u, 1);
                m.vertex(cone ? 0.0f : cx * hx, top, cone ? 0.0f : cz * hz, cx / l, slope / l, cz / l, u, 0);
            }
            for (u32 s = 0; s < seg; ++s) {
                const u32 a = s * 2;
                m.tri(a, a + 2, a + 1);
                if (!cone) m.tri(a + 1, a + 2, a + 3);
            }
        }
        for (const int side : {1, -1}) {
            if (cone && side == 1) continue;
            if (disc && side == -1) continue;
            const f32 y = side == 1 ? top : bottom;
            const u32 centre = m.vertex(0, y, 0, 0, static_cast<f32>(side), 0, 0.5f, 0.5f);
            for (u32 s = 0; s <= seg; ++s) {
                const f32 ph = static_cast<f32>(s) / static_cast<f32>(seg) * 2 * kPi;
                m.vertex(std::cos(ph) * hx, y, -std::sin(ph) * hz, 0, static_cast<f32>(side), 0, 0.5f + 0.5f * std::cos(ph), 0.5f + 0.5f * std::sin(ph));
            }
            for (u32 s = 0; s < seg; ++s) {
                if (side == 1) m.tri(centre, centre + 1 + s, centre + 2 + s);
                else m.tri(centre, centre + 2 + s, centre + 1 + s);
            }
        }
    }
}

// The .glb container around a JSON chunk (padded here) and a binary chunk (already 4-aligned).
void pack_glb(cook::String& j, const Array<byte>& bin, Array<byte>& out) {
    while (j.size() % 4) j += ' ';
    const auto u32le = [&](u32 v) {
        const u8 b[4] = {static_cast<u8>(v), static_cast<u8>(v >> 8), static_cast<u8>(v >> 16), static_cast<u8>(v >> 24)};
        const usize at = out.size();
        out.resize(at + 4);
        std::memcpy(out.data() + at, b, 4);
    };
    out.clear();
    u32le(0x46546C67u);  // "glTF"
    u32le(2);
    u32le(static_cast<u32>(12 + 8 + j.size() + 8 + bin.size()));
    u32le(static_cast<u32>(j.size()));
    u32le(0x4E4F534Au);  // "JSON"
    usize at = out.size();
    out.resize(at + j.size());
    std::memcpy(out.data() + at, j.c_str(), j.size());
    u32le(static_cast<u32>(bin.size()));
    u32le(0x004E4942u);  // "BIN\0"
    at = out.size();
    out.resize(at + bin.size());
    if (!bin.empty()) std::memcpy(out.data() + at, bin.data(), bin.size());
}

// Appends bytes to a glTF binary chunk, 4-aligned, and records a bufferView and an accessor for them.
struct GlbBuilder {
    Array<byte> bin;
    cook::String views, accessors;
    u32 count = 0;
    u32 add(const void* p, usize bytes, const char* accessor_tail, i32 target) {
        while (bin.size() % 4) bin.push_back(byte{0});
        const usize at = bin.size();
        bin.resize(at + bytes);
        std::memcpy(bin.data() + at, p, bytes);
        if (count) views += ',', accessors += ',';
        cook::appendf(views, R"({"buffer":0,"byteOffset":%zu,"byteLength":%zu)", at, bytes);
        if (target) cook::appendf(views, R"(,"target":%d)", target);
        views += '}';
        cook::appendf(accessors, R"({"bufferView":%u,%s})", count, accessor_tail);
        return count++;
    }
};

// The humanoid as a skinned .glb: a root node, the skinned mesh node, 13 joint nodes, two clips.
void write_humanoid_glb(const MeshData& m, const Array<u8>& joint_of, f32 w, f32 h, const f32 color[4], f32 roughness, Array<byte>& out) {
    constexpr f32 kDeg = 3.14159265358979f / 180.0f;
    const usize n_vert = m.pos.size() / 3;
    GlbBuilder g;
    f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (usize i = 0; i < n_vert; ++i)
        for (usize k = 0; k < 3; ++k) {
            lo[k] = math::min(lo[k], m.pos[i * 3 + k]);
            hi[k] = math::max(hi[k], m.pos[i * 3 + k]);
        }
    cook::String tail;
    cook::appendf(tail, R"("componentType":5126,"count":%zu,"type":"VEC3","min":[%g,%g,%g],"max":[%g,%g,%g])", n_vert, static_cast<f64>(lo[0]),
                  static_cast<f64>(lo[1]), static_cast<f64>(lo[2]), static_cast<f64>(hi[0]), static_cast<f64>(hi[1]), static_cast<f64>(hi[2]));
    const u32 a_pos = g.add(m.pos.data(), m.pos.size() * 4, tail.c_str(), 34962);
    tail.clear();
    cook::appendf(tail, R"("componentType":5126,"count":%zu,"type":"VEC3")", n_vert);
    const u32 a_nrm = g.add(m.nrm.data(), m.nrm.size() * 4, tail.c_str(), 34962);
    tail.clear();
    cook::appendf(tail, R"("componentType":5126,"count":%zu,"type":"VEC2")", n_vert);
    const u32 a_uv = g.add(m.uv.data(), m.uv.size() * 4, tail.c_str(), 34962);
    Array<u8> j4;
    Array<f32> w4;
    for (usize v = 0; v < n_vert; ++v) {
        j4.push_back(joint_of[v]), j4.push_back(0), j4.push_back(0), j4.push_back(0);
        w4.push_back(1.0f), w4.push_back(0.0f), w4.push_back(0.0f), w4.push_back(0.0f);
    }
    tail.clear();
    cook::appendf(tail, R"("componentType":5121,"count":%zu,"type":"VEC4")", n_vert);
    const u32 a_joints = g.add(j4.data(), j4.size(), tail.c_str(), 34962);
    tail.clear();
    cook::appendf(tail, R"("componentType":5126,"count":%zu,"type":"VEC4")", n_vert);
    const u32 a_weights = g.add(w4.data(), w4.size() * 4, tail.c_str(), 34962);
    tail.clear();
    cook::appendf(tail, R"("componentType":5125,"count":%zu,"type":"SCALAR")", m.idx.size());
    const u32 a_idx = g.add(m.idx.data(), m.idx.size() * 4, tail.c_str(), 34963);
    // Bind pose: no rotations, so each inverse bind matrix is a translation by minus the joint.
    Array<f32> ibm;
    for (u32 b = 0; b < kHumanoidBones; ++b) {
        f32 p[3];
        humanoid_joint(b, w, h, p);
        const f32 mtx[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -p[0], -p[1], -p[2], 1};
        for (const f32 x : mtx) ibm.push_back(x);
    }
    tail.clear();
    cook::appendf(tail, R"("componentType":5126,"count":%u,"type":"MAT4")", kHumanoidBones);
    const u32 a_ibm = g.add(ibm.data(), ibm.size() * 4, tail.c_str(), 0);

    // Clips: rotations about X (positive swings a limb forward, towards -Z) at five keys a loop.
    struct Track {
        u32 bone;
        f32 deg[5];
    };
    const Track walk[] = {
        {7, {25, 0, -25, 0, 25}},    {10, {-25, 0, 25, 0, -25}}, {8, {-5, -35, -5, -5, -5}}, {11, {-5, -5, -5, -35, -5}},
        {3, {-18, 0, 18, 0, -18}},   {5, {18, 0, -18, 0, 18}},   {4, {15, 15, 15, 15, 15}},  {6, {15, 15, 15, 15, 15}},
        {1, {4, 2, 4, 2, 4}},
    };
    const Track idle[] = {{1, {0, 2, 0, -1, 0}}, {2, {0, -2, 0, 1, 0}}, {4, {8, 10, 8, 10, 8}}, {6, {8, 10, 8, 10, 8}}};
    const auto clip = [&](const char* name, const Track* tracks, usize n, f32 period, cook::String& anim) {
        const f32 times[5] = {0, period * 0.25f, period * 0.5f, period * 0.75f, period};
        cook::String t;
        cook::appendf(t, R"("componentType":5126,"count":5,"type":"SCALAR","min":[0],"max":[%g])", static_cast<f64>(period));
        const u32 a_time = g.add(times, sizeof times, t.c_str(), 0);
        cook::appendf(anim, R"({"name":"%s","samplers":[)", name);
        cook::String channels;
        for (usize i = 0; i < n; ++i) {
            f32 q[20];
            for (u32 k = 0; k < 5; ++k) {
                const f32 half = tracks[i].deg[k] * kDeg * 0.5f;
                q[k * 4] = std::sin(half), q[k * 4 + 1] = 0.0f, q[k * 4 + 2] = 0.0f, q[k * 4 + 3] = std::cos(half);
            }
            const u32 a_rot = g.add(q, sizeof q, R"("componentType":5126,"count":5,"type":"VEC4")", 0);
            if (i) anim += ',', channels += ',';
            cook::appendf(anim, R"({"input":%u,"output":%u,"interpolation":"LINEAR"})", a_time, a_rot);
            cook::appendf(channels, R"({"sampler":%zu,"target":{"node":%u,"path":"rotation"}})", i, tracks[i].bone + 2);
        }
        anim += R"(],"channels":[)";
        anim += channels.view();
        anim += "]}";
    };
    cook::String anims;
    clip("Idle", idle, sizeof idle / sizeof idle[0], 2.0f, anims);
    anims += ',';
    clip("Walk", walk, sizeof walk / sizeof walk[0], 1.0f, anims);

    // Nodes: 0 the root, 1 the skinned mesh, 2.. the joints (bone b is node b + 2).
    cook::String j;
    j += R"({"asset":{"version":"2.0","generator":"mce gen_mesh"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[)";
    j += R"({"name":"humanoid","children":[1,2]},{"name":"humanoid_mesh","mesh":0,"skin":0})";
    for (u32 b = 0; b < kHumanoidBones; ++b) {
        f32 p[3], q[3] = {0, 0, 0};
        humanoid_joint(b, w, h, p);
        if (kHumanoid[b].parent >= 0) humanoid_joint(static_cast<u32>(kHumanoid[b].parent), w, h, q);
        cook::appendf(j, R"(,{"name":"%s","translation":[%g,%g,%g])", kHumanoid[b].name, static_cast<f64>(p[0] - q[0]),
                      static_cast<f64>(p[1] - q[1]), static_cast<f64>(p[2] - q[2]));
        bool first = true;
        for (u32 c = 0; c < kHumanoidBones; ++c)
            if (kHumanoid[c].parent == static_cast<i32>(b)) {
                j += first ? R"(,"children":[)" : ",";
                cook::appendf(j, "%u", c + 2);
                first = false;
            }
        if (!first) j += ']';
        j += '}';
    }
    j += R"(],"skins":[{"name":"humanoid_skin","inverseBindMatrices":)";
    cook::appendf(j, "%u", a_ibm);
    j += R"(,"skeleton":2,"joints":[)";
    for (u32 b = 0; b < kHumanoidBones; ++b) cook::appendf(j, "%s%u", b ? "," : "", b + 2);
    cook::appendf(j, R"(]}],"meshes":[{"name":"humanoid","primitives":[{"attributes":{"POSITION":%u,"NORMAL":%u,"TEXCOORD_0":%u,"JOINTS_0":%u,"WEIGHTS_0":%u},"indices":%u,"material":0}]}],)",
                  a_pos, a_nrm, a_uv, a_joints, a_weights, a_idx);
    cook::appendf(j, R"("materials":[{"name":"humanoid","pbrMetallicRoughness":{"baseColorFactor":[%.4f,%.4f,%.4f,%.4f],"metallicFactor":0,"roughnessFactor":%.3f}}],)",
                  static_cast<f64>(color[0]), static_cast<f64>(color[1]), static_cast<f64>(color[2]), static_cast<f64>(color[3]),
                  static_cast<f64>(roughness));
    j += R"("animations":[)";
    j += anims.view();
    while (g.bin.size() % 4) g.bin.push_back(byte{0});
    cook::appendf(j, R"(],"buffers":[{"byteLength":%zu}],"bufferViews":[)", g.bin.size());
    j += g.views.view();
    j += R"(],"accessors":[)";
    j += g.accessors.view();
    j += "]}";
    pack_glb(j, g.bin, out);
}

// The mesh as a .glb: one node, one primitive, one inline material.
void write_glb(const MeshData& m, const f32 color[4], f32 roughness, StringView name, Array<byte>& out) {
    Array<byte> bin;
    const auto put = [&](const void* p, usize n) {
        const usize at = bin.size();
        bin.resize(at + n);
        std::memcpy(bin.data() + at, p, n);
    };
    const usize n_vert = m.pos.size() / 3;
    const usize pos_off = 0;
    put(m.pos.data(), m.pos.size() * 4);
    const usize nrm_off = bin.size();
    put(m.nrm.data(), m.nrm.size() * 4);
    const usize uv_off = bin.size();
    put(m.uv.data(), m.uv.size() * 4);
    const usize idx_off = bin.size();
    put(m.idx.data(), m.idx.size() * 4);
    while (bin.size() % 4) bin.push_back(byte{0});
    f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (usize i = 0; i < n_vert; ++i)
        for (usize k = 0; k < 3; ++k) {
            lo[k] = math::min(lo[k], m.pos[i * 3 + k]);
            hi[k] = math::max(hi[k], m.pos[i * 3 + k]);
        }
    cook::String j;
    cook::appendf(j, R"({"asset":{"version":"2.0","generator":"mce gen_mesh"},"scene":0,"scenes":[{"nodes":[0]}],)");
    j += R"("nodes":[{"mesh":0,"name":)";
    cook::append_json_string(j, name);
    j += R"(}],"meshes":[{"name":)";
    cook::append_json_string(j, name);
    j += R"(,"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],)";
    cook::appendf(j, R"("materials":[{"name":"%.*s","pbrMetallicRoughness":{"baseColorFactor":[%.4f,%.4f,%.4f,%.4f],"metallicFactor":0,"roughnessFactor":%.3f}%s}],)",
                  static_cast<int>(name.size()), name.data(), static_cast<f64>(color[0]), static_cast<f64>(color[1]), static_cast<f64>(color[2]),
                  static_cast<f64>(color[3]), static_cast<f64>(roughness), color[3] < 1.0f ? R"(,"alphaMode":"BLEND")" : "");
    cook::appendf(j, R"("buffers":[{"byteLength":%zu}],"bufferViews":[)", bin.size());
    cook::appendf(j, R"({"buffer":0,"byteOffset":%zu,"byteLength":%zu,"target":34962},)", pos_off, m.pos.size() * 4);
    cook::appendf(j, R"({"buffer":0,"byteOffset":%zu,"byteLength":%zu,"target":34962},)", nrm_off, m.nrm.size() * 4);
    cook::appendf(j, R"({"buffer":0,"byteOffset":%zu,"byteLength":%zu,"target":34962},)", uv_off, m.uv.size() * 4);
    cook::appendf(j, R"({"buffer":0,"byteOffset":%zu,"byteLength":%zu,"target":34963}],"accessors":[)", idx_off, m.idx.size() * 4);
    cook::appendf(j, R"({"bufferView":0,"componentType":5126,"count":%zu,"type":"VEC3","min":[%g,%g,%g],"max":[%g,%g,%g]},)", n_vert,
                  static_cast<f64>(lo[0]), static_cast<f64>(lo[1]), static_cast<f64>(lo[2]), static_cast<f64>(hi[0]), static_cast<f64>(hi[1]),
                  static_cast<f64>(hi[2]));
    cook::appendf(j, R"({"bufferView":1,"componentType":5126,"count":%zu,"type":"VEC3"},)", n_vert);
    cook::appendf(j, R"({"bufferView":2,"componentType":5126,"count":%zu,"type":"VEC2"},)", n_vert);
    cook::appendf(j, R"({"bufferView":3,"componentType":5125,"count":%zu,"type":"SCALAR"}]})", m.idx.size());
    pack_glb(j, bin, out);
}

// A small shaded preview: orthographic, from above and to the side, flat Lambert, z-buffered.
cook::Image mesh_preview(const MeshData& m, const f32 color[4], u32 px) {
    cook::Image img = make_image(px, px, 0x1a1d24ffu);
    Array<f32> depth;
    depth.resize(static_cast<usize>(px) * px, 1e30f);
    // View: yaw 35 degrees, pitch 30 degrees down.
    const f32 cy = std::cos(0.61f), sy = std::sin(0.61f), cp = std::cos(0.52f), sp = std::sin(0.52f);
    const usize n = m.pos.size() / 3;
    Array<f32> sx, syv, sz;
    sx.resize(n), syv.resize(n), sz.resize(n);
    f32 extent = 1e-6f;
    for (usize i = 0; i < n; ++i) {
        const f32 x = m.pos[i * 3], y = m.pos[i * 3 + 1], z = m.pos[i * 3 + 2];
        const f32 rx = cy * x - sy * z, rz = sy * x + cy * z;
        const f32 vy = cp * y - sp * rz, vz = sp * y + cp * rz;
        sx[i] = rx, syv[i] = vy, sz[i] = vz;
        extent = math::max(extent, math::max(std::fabs(rx), std::fabs(vy)));
    }
    const f32 scale = static_cast<f32>(px) * 0.42f / extent, mid = static_cast<f32>(px) * 0.5f;
    const f32 lx = -0.4f, ly = 0.8f, lz = 0.45f;
    for (usize t = 0; t + 2 < m.idx.size(); t += 3) {
        const u32 a = m.idx[t], b = m.idx[t + 1], c = m.idx[t + 2];
        const f32 ax = mid + sx[a] * scale, ay = mid - syv[a] * scale, bx = mid + sx[b] * scale, by = mid - syv[b] * scale;
        const f32 cx2 = mid + sx[c] * scale, cy2 = mid - syv[c] * scale;
        const f32 area = (bx - ax) * (cy2 - ay) - (by - ay) * (cx2 - ax);
        if (std::fabs(area) < 1e-6f) continue;
        const f32 nx = (m.nrm[a * 3] + m.nrm[b * 3] + m.nrm[c * 3]) / 3, ny = (m.nrm[a * 3 + 1] + m.nrm[b * 3 + 1] + m.nrm[c * 3 + 1]) / 3,
                  nz = (m.nrm[a * 3 + 2] + m.nrm[b * 3 + 2] + m.nrm[c * 3 + 2]) / 3;
        const f32 lit = 0.25f + 0.75f * math::max(0.0f, nx * lx + ny * ly + nz * lz);
        const i32 x0 = math::max(0, static_cast<i32>(math::min(ax, math::min(bx, cx2)))), x1 = math::min(static_cast<i32>(px) - 1, static_cast<i32>(math::max(ax, math::max(bx, cx2))) + 1);
        const i32 y0 = math::max(0, static_cast<i32>(math::min(ay, math::min(by, cy2)))), y1 = math::min(static_cast<i32>(px) - 1, static_cast<i32>(math::max(ay, math::max(by, cy2))) + 1);
        for (i32 y = y0; y <= y1; ++y)
            for (i32 x = x0; x <= x1; ++x) {
                const f32 fx = static_cast<f32>(x) + 0.5f, fy = static_cast<f32>(y) + 0.5f;
                const f32 w0 = ((bx - fx) * (cy2 - fy) - (by - fy) * (cx2 - fx)) / area;
                const f32 w1 = ((cx2 - fx) * (ay - fy) - (cy2 - fy) * (ax - fx)) / area;
                const f32 w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const f32 d = w0 * sz[a] + w1 * sz[b] + w2 * sz[c];
                f32& z = depth[static_cast<usize>(y) * px + static_cast<usize>(x)];
                if (d >= z) continue;
                z = d;
                u8* p = &img.rgba[(static_cast<usize>(y) * px + static_cast<usize>(x)) * 4];
                for (int k = 0; k < 3; ++k) p[k] = static_cast<u8>(std::lround(math::clamp(color[k] * lit, 0.0f, 1.0f) * 255.0f));
                p[3] = 255;
            }
    }
    return img;
}

}  // namespace

// Simple meshes for blocking out a 3D level: box, plane, sphere, cylinder, cone, disc and ramp; and a
// skinned, animated humanoid for characters, crowds and ragdolls.
bool gen_mesh(ToolCall& c) {
    StringView save;
    if (!arg_save(c, ".glb", save)) return false;
    StringView shape = "box";
    if (!arg_choice(c, "shape", shape, mesh_shape_known, "one of box, plane, sphere, cylinder, cone, disc, ramp, humanoid")) return false;
    const bool humanoid = shape == "humanoid";
    f32 size[3] = {1, 1, 1};
    if (humanoid) size[0] = 0.5f, size[1] = 1.8f, size[2] = 0.28f;
    if (const cook::JsonValue s = arg(c, "size"); s.valid()) {
        if (!s.is_array() || s.size() != 3) return arg_missing(c, "size", "[x, y, z] in metres, each 0.01 to 1000");
        for (u32 i = 0; i < 3; ++i) {
            if (!s.at(i).is_number() || s.at(i).as_number() < 0.01 || s.at(i).as_number() > 1000)
                return arg_missing(c, "size", "[x, y, z] in metres, each 0.01 to 1000");
            size[i] = static_cast<f32>(s.at(i).as_number());
        }
    }
    u64 segments = 24;
    if (!arg_uint(c, "segments", segments, 3, 128, false)) return false;
    f32 color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    StringView hex;
    if (!arg_string(c, "color", hex, false)) return false;
    if (!hex.empty()) {
        u32 rgba = 0;
        if (!gen::parse_hex(hex, rgba)) return arg_missing(c, "color", "\"#rrggbb\" or \"#rrggbbaa\"");
        for (int k = 0; k < 4; ++k) color[k] = static_cast<f32>((rgba >> (24 - 8 * k)) & 0xFFu) / 255.0f;
    }
    f32 roughness = 0.8f;
    if (!arg_f32(c, "roughness", roughness, 0, 1)) return false;
    MeshData m;
    Array<u8> joints;
    if (humanoid)
        build_humanoid(size[0], size[1], size[2], m, joints);
    else
        build_mesh(shape, size[0], size[1], size[2], static_cast<u32>(segments), m);
    Saved saved;
    if (!save.empty()) {
        Array<byte> glb;
        if (humanoid)
            write_humanoid_glb(m, joints, size[0], size[1], color, roughness, glb);
        else
            write_glb(m, color, roughness, shape, glb);
        cook::String src;
        cook::appendf(src, "made with gen_mesh (%.*s %gx%gx%g m)", static_cast<int>(shape.size()), shape.data(), static_cast<f64>(size[0]),
                      static_cast<f64>(size[1]), static_cast<f64>(size[2]));
        if (!save_asset(c, save, glb.span(), "{}", src.view(), "the project's (generated)", saved)) return false;
    }
    c.out.begin_object().key("shape").str(shape).key("vertices").num_u(m.pos.size() / 3).key("triangles").num_u(m.idx.size() / 3);
    c.out.key("size").begin_array().num_f(size[0]).num_f(size[1]).num_f(size[2]).end_array();
    if (humanoid) {
        c.out.key("bones").begin_array();
        for (const HumanoidBone& b : kHumanoid) c.out.str(StringView(b.name));
        c.out.end_array();
        c.out.key("clips").begin_array().str("Idle").str("Walk").end_array();
    }
    if (!save.empty()) write_saved(c.out, saved);
    c.out.end_object();
    return attach(c, mesh_preview(m, color, 256));
}

}  // namespace mce::mcp
