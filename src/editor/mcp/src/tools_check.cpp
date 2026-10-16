// M-AGENT quality gates (SBA-53): project_check runs the checks a person would do before shipping
// and returns them as findings an agent can act on: the cook (scenes and assets that fail to load),
// broken and unused references, missing credits, colour space of data textures, each scene run
// headless against a per-tick budget, and accessibility basics (text size and contrast in scenes and
// UI documents, subtitles for timelines that play sounds, captions for sounds).

#include <cmath>

#include "mce/asset/guid.h"
#include "mce/cook/fs.h"
#include "mce/cook/image.h"
#include "mce/cook/json.h"
#include "mce/cook/ui_text.h"
#include "mce/editor/project_settings.h"
#include "mce/mcp/headless_run.h"
#include "mce/platform/platform.h"
#include "mce/ui/document_data.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

namespace {

constexpr const char* kChecks[] = {"cook", "references", "unused", "credits", "srgb", "performance", "accessibility"};
constexpr usize kCheckCount = sizeof(kChecks) / sizeof(kChecks[0]);

enum class Sev : u8 { Error, Warning, Info };
const char* sev_name(Sev s) { return s == Sev::Error ? "error" : s == Sev::Warning ? "warning" : "info"; }

struct Finding {
    Sev sev;
    const char* check;
    cook::String file;  // project-relative ("assets/...") or empty
    u32 line = 0;
    cook::String message;
    cook::String fix;  // what to do about it (a tool call when there is one)
};

struct Report {
    Array<Finding> findings;
    u32 counts[kCheckCount][3] = {};
    void add(Sev sev, const char* check, StringView file, u32 line, StringView message, StringView fix = {}) {
        for (usize i = 0; i < kCheckCount; ++i)
            if (StringView(kChecks[i]) == check) ++counts[i][static_cast<u32>(sev)];
        Finding f{sev, check, cook::String(file), line, cook::String(message), cook::String(fix)};
        findings.push_back(static_cast<Finding&&>(f));
    }
};

bool ends_with(StringView s, StringView suffix) { return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix; }

StringView base_name(StringView p) {
    const usize slash = p.rfind('/');
    return slash == StringView::npos ? p : p.substr(slash + 1);
}

char lower(char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }

bool contains_ci(StringView hay, StringView needle) {
    for (usize i = 0; i + needle.size() <= hay.size(); ++i) {
        usize j = 0;
        while (j < needle.size() && lower(hay[i + j]) == lower(needle[j])) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

u32 line_of(StringView text, usize at) {
    u32 line = 1;
    for (usize i = 0; i < at && i < text.size(); ++i)
        if (text[i] == '\n') ++line;
    return line;
}

// Files under assets/ that hold references to other assets by GUID: the engine's JSON documents
// (.mcscene, .mcprefab, .mcmat, .mctilemap, .mcsheet, ... every ".mc*" but the sidecars) and .json.
bool is_text_asset(StringView rel) {
    const cook::String ext = cook::lower_extension(rel);
    if (ext.view() == ".mceasset") return false;
    return ext.view() == ".json" || (ext.size() > 3 && ext.view().substr(0, 3) == ".mc");
}

// Media that needs a credits row (code-made data files do not).
bool is_media(StringView rel) {
    const cook::String ext = cook::lower_extension(rel);
    for (const char* e : {".png", ".jpg", ".jpeg", ".wav", ".ogg", ".mp3", ".flac", ".ttf", ".otf", ".glb", ".gltf", ".hdr", ".exr"})
        if (ext.view() == e) return true;
    return false;
}

struct Ref {
    asset::Guid guid;
    u32 file;  // index into the scanned files
    u32 line;
};

// Every "guid:<uuid>" in `text`.
void scan_guids(StringView text, u32 file, Array<Ref>& out) {
    for (usize at = text.find("guid:"); at != StringView::npos; at = text.find("guid:", at + 5)) {
        if (at + 5 + 36 > text.size()) break;
        if (const Result<asset::Guid> g = asset::parse_guid(text.substr(at + 5, 36))) out.push_back({g.value(), file, line_of(text, at)});
    }
}

// ---------------------------------------------------------------- credits

// "*" matches within a path segment, "**" across segments.
bool glob_match(StringView pat, StringView s) {
    if (pat.empty()) return s.empty();
    if (pat.size() >= 2 && pat[0] == '*' && pat[1] == '*') {
        StringView rest = pat.substr(2);
        if (!rest.empty() && rest[0] == '/') rest = rest.substr(1);
        for (usize i = 0; i <= s.size(); ++i)
            if (glob_match(rest, s.substr(i))) return true;
        return false;
    }
    if (pat[0] == '*') {
        for (usize i = 0; i <= s.size(); ++i) {
            if (glob_match(pat.substr(1), s.substr(i))) return true;
            if (i < s.size() && s[i] == '/') break;
        }
        return false;
    }
    return !s.empty() && lower(pat[0]) == lower(s[0]) && glob_match(pat.substr(1), s.substr(1));
}

// The path patterns of CREDITS.md's table rows, relative to assets/: `assets/x/*.png` and the bare
// names after it in the same cell (`assets/sfx/jump.wav`, `pickup.wav`). A row that names a whole
// folder (`assets/models/*`) covers everything under it.
void credit_patterns(StringView text, Array<cook::String>& out) {
    for (usize at = 0; at < text.size();) {
        usize end = text.find('\n', at);
        if (end == StringView::npos) end = text.size();
        const StringView line = text.substr(at, end - at);
        at = end + 1;
        if (line.empty() || line[0] != '|') continue;
        // The first cell only: the files.
        const usize cell_end = line.find('|', 1);
        const StringView cell = line.substr(1, cell_end == StringView::npos ? line.size() - 1 : cell_end - 1);
        cook::String dir;
        for (usize t = cell.find('`'); t != StringView::npos; t = cell.find('`', t + 1)) {
            const usize close = cell.find('`', t + 1);
            if (close == StringView::npos) break;
            StringView tok = cell.substr(t + 1, close - t - 1);
            t = close;
            if (tok.size() > 7 && tok.substr(0, 7) == "assets/") tok = tok.substr(7);
            else if (tok.find('/') == StringView::npos && !dir.empty()) {
                cook::String p(dir.view());
                p += tok;
                out.push_back(p);
                continue;
            } else if (tok.find('/') == StringView::npos) {
                out.push_back(cook::String(tok));  // a bare name: any folder
                cook::String any("**/");
                any += tok;
                out.push_back(any);
                continue;
            }
            out.push_back(cook::String(tok));
            const usize slash = tok.rfind('/');
            dir = cook::String(slash == StringView::npos ? StringView() : tok.substr(0, slash + 1));
            if (ends_with(tok, "/*")) {
                cook::String deep(tok.substr(0, tok.size() - 1));
                deep += "**";
                out.push_back(deep);
            }
        }
    }
}

// ---------------------------------------------------------------- colour

f32 channel_lin(f32 c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
f32 luminance(f32 r, f32 g, f32 b) { return 0.2126f * channel_lin(r) + 0.7152f * channel_lin(g) + 0.0722f * channel_lin(b); }
f32 contrast(f32 l1, f32 l2) { return (l1 > l2 ? l1 + 0.05f : l2 + 0.05f) / (l1 > l2 ? l2 + 0.05f : l1 + 0.05f); }

bool read_color(cook::JsonValue v, f32 rgb[3]) {
    if (!v.is_array() || v.size() < 3) return false;
    for (u32 i = 0; i < 3; ++i) {
        if (!v.at(i).is_number()) return false;
        rgb[i] = static_cast<f32>(v.at(i).as_number());
    }
    return true;
}

f32 num_or(cook::JsonValue o, StringView key, f32 def) {
    const cook::JsonValue v = o.get(key);
    return v.is_number() ? static_cast<f32>(v.as_number()) : def;
}

f32 vec_or(cook::JsonValue o, StringView key, u32 lane, f32 def) {
    const cook::JsonValue v = o.get(key);
    return v.is_array() && lane < v.size() && v.at(lane).is_number() ? static_cast<f32>(v.at(lane).as_number()) : def;
}

// The Text, Button and Toggle labels of a .mcui document as the player draws them (M-UI-DATA2 3): the
// document read the way the cook reads it (kind defaults, "styles", "own_style"), its theme applied,
// the Normal state's text size and colour judged against the nearest opaque quad behind the label.
// False when the document or its theme cannot be read (the cook reports those).
bool check_ui_document(const cook::JsonValue& root, StringView rel_file, const editor::EditorProject& project, f32 min_px, Report& rep) {
    ui::DocumentData d;
    cook::DiagList diags;
    if (!cook::read_ui_document(root, rel_file, d, diags)) return false;
    if (!d.theme.is_null()) {
        const editor::ProjectAsset* a = project.find(d.theme);
        cook::String text;
        cook::JsonDoc tdoc;
        cook::JsonError je;
        ui::ThemeData theme;
        cook::DiagList tdiags;
        if (!a || !cook::fs::read_text(project.asset_path(a->path.view()).view(), text) || !tdoc.parse(text.view(), je) ||
            !cook::read_ui_theme(tdoc.root(), a->path.view(), theme, tdiags))
            return false;
        ui::apply_theme(theme, d);
    }
    constexpr u32 kNormal = static_cast<u32>(ui::WidgetState::Normal);
    // Per widget: the colour an opaque quad at or above it shows, if any.
    Array<u8> has_bg;
    Array<f32> bgs;
    has_bg.resize(d.widgets.size());
    bgs.resize(d.widgets.size() * 3);
    for (u32 i = 0; i < d.widgets.size(); ++i) {
        const ui::WidgetData& w = d.widgets[i];
        const ui::Style& st = w.styles[kNormal];
        const bool up = w.parent != ui::kNoWidgetIndex;
        has_bg[i] = up ? has_bg[w.parent] : 0;
        for (u32 k = 0; k < 3; ++k) bgs[i * 3 + k] = up ? bgs[w.parent * 3 + k] : 0.0f;
        // An image's tint colours the image, and a Text draws no quad: neither is a panel to read against.
        if (w.kind != ui::WidgetKind::Image && w.kind != ui::WidgetKind::Text && st.draw_quad && st.tint.a >= 0.5f) {
            has_bg[i] = 1;
            bgs[i * 3] = st.tint.r;
            bgs[i * 3 + 1] = st.tint.g;
            bgs[i * 3 + 2] = st.tint.b;
        }
        const bool labeled = w.kind == ui::WidgetKind::Text || w.kind == ui::WidgetKind::Button || w.kind == ui::WidgetKind::Toggle;
        if (!labeled || w.text.empty()) continue;
        // The widget's path from the root, as ui_list and ui_widget_set name it.
        cook::String path;
        {
            Array<u32> chain;
            for (u32 n = i; n != ui::kNoWidgetIndex; n = d.widgets[n].parent) chain.push_back(n);
            for (usize c = chain.size(); c-- > 0;) {
                if (!path.empty()) path += '/';
                path += d.widgets[chain[c]].name.view();
            }
        }
        if (st.text_size > 0 && st.text_size < min_px) {
            cook::String msg;
            cook::appendf(msg, "%.*s: UI text is %.0f px at UI scale 1 (at least %.0f reads on a phone or TV)",
                          static_cast<int>(path.size()), path.c_str(), static_cast<f64>(st.text_size), static_cast<f64>(min_px));
            cook::String fix;
            cook::appendf(fix, "raise its style.text_size to %.0f or more (players can also raise access.text_scale)", static_cast<f64>(min_px));
            rep.add(Sev::Warning, "accessibility", rel_file, 0, msg.view(), fix.view());
        }
        if (has_bg[i]) {
            const f32 ratio = contrast(luminance(st.text_color.r, st.text_color.g, st.text_color.b),
                                       luminance(bgs[i * 3], bgs[i * 3 + 1], bgs[i * 3 + 2]));
            if (ratio < 4.5f) {
                cook::String msg;
                cook::appendf(msg, "%.*s: UI text contrast against the panel behind it is %.1f:1 (WCAG AA asks 4.5:1)",
                              static_cast<int>(path.size()), path.c_str(), static_cast<f64>(ratio));
                rep.add(Sev::Warning, "accessibility", rel_file, 0, msg.view(),
                        "a lighter or darker style.text_color, or a darker or lighter panel tint");
            }
        }
    }
    return true;
}

// The colour a backing panel shows behind a label: the smallest Sprite whose rectangle holds the
// label's position, its tint times its texture's average colour, blended over `clear` by alpha.
// False when no sprite sits behind it.
bool panel_behind(const cook::JsonValue& ents, u32 label, const editor::EditorProject& project, const f32 clear[3], f32 out[3]) {
    const cook::JsonValue lt = ents.at(label).get("components").get("LocalTransform");
    const f32 lx = vec_or(lt, "position", 0, 0), ly = vec_or(lt, "position", 1, 0);
    f32 best = 1e30f;
    bool found = false;
    for (u32 e = 0; e < ents.size(); ++e) {
        if (e == label) continue;
        const cook::JsonValue comps = ents.at(e).get("components");
        const cook::JsonValue sp = comps.get("Sprite"), tr = comps.get("LocalTransform");
        if (!sp.is_object()) continue;
        const f32 w = vec_or(sp, "size", 0, 1) * std::fabs(vec_or(tr, "scale", 0, 1)), h = vec_or(sp, "size", 1, 1) * std::fabs(vec_or(tr, "scale", 1, 1));
        const f32 x0 = vec_or(tr, "position", 0, 0) - w * vec_or(sp, "pivot", 0, 0.5f), y0 = vec_or(tr, "position", 1, 0) - h * vec_or(sp, "pivot", 1, 0.5f);
        if (lx < x0 || lx > x0 + w || ly < y0 || ly > y0 + h || w * h >= best) continue;
        f32 rgb[3] = {1, 1, 1};
        (void)read_color(sp.get("tint"), rgb);
        f32 alpha = vec_or(sp, "tint", 3, 1);
        const cook::JsonValue tex = sp.get("texture");
        if (tex.is_string() && tex.as_string().size() >= 41 && tex.as_string().substr(0, 5) == "guid:") {
            const Result<asset::Guid> g = asset::parse_guid(tex.as_string().substr(5, 36));
            const editor::ProjectAsset* a = g ? project.find(g.value()) : nullptr;
            cook::String bytes;
            // A texture that cannot be read (missing, not a PNG) says nothing about the colour: no panel.
            if (!a || !cook::fs::read_text(project.asset_path(a->path.view()).view(), bytes)) continue;
            const Result<cook::Image> img = cook::decode_png(Span<const byte>(reinterpret_cast<const byte*>(bytes.c_str()), bytes.size()));
            if (!img || !img.value().width || !img.value().height) continue;
            f64 sum[4] = {};
            const Array<u8>& px = img.value().rgba;
            for (usize i = 0; i + 3 < px.size(); i += 4) {
                const f64 a8 = px[i + 3] / 255.0;
                for (u32 k = 0; k < 3; ++k) sum[k] += px[i + k] / 255.0 * a8;
                sum[3] += a8;
            }
            const f64 n = static_cast<f64>(px.size()) / 4.0;
            for (u32 k = 0; k < 3; ++k) rgb[k] *= sum[3] > 0 ? static_cast<f32>(sum[k] / sum[3]) : 0.f;
            alpha *= static_cast<f32>(sum[3] / n);
        }
        for (u32 k = 0; k < 3; ++k) out[k] = rgb[k] * alpha + clear[k] * (1 - alpha);
        best = w * h;
        found = true;
    }
    return found;
}

// Texture slots of a material that hold data, not colour (sampled linear).
constexpr const char* kDataSlots[] = {"normal", "metallic_roughness", "roughness", "metallic", "occlusion", "orm", "height", "mask"};
constexpr const char* kDataNames[] = {"_n.", "_normal", "normalmap", "_rough", "_metal", "_orm", "_ao.", "_height", "_mask", "_lut", "lut_"};

bool texture_is_srgb(StringView sidecar_text) {
    cook::JsonDoc doc;
    cook::JsonError je;
    if (!doc.parse(sidecar_text, je)) return true;
    const cook::JsonValue s = doc.root().get("settings").get("srgb");
    return !s.is_bool() || s.as_bool();
}

struct Options {
    bool on[kCheckCount] = {};
    u64 ticks = 300;
    f64 budget_ms = 4.0;   // the target budget's sim_ms unless the call gives budget_ms
    bool budget_ms_given = false;
    perf::Target target = perf::Target::Desktop;
    perf::Budget budget;
    f32 min_text_px = 16.0f;
    bool enabled(const char* name) const {
        for (usize i = 0; i < kCheckCount; ++i)
            if (StringView(kChecks[i]) == name) return on[i];
        return false;
    }
};

struct PerfRow {
    cook::String scene;
    u64 ticks = 0;
    u32 entities = 0;
    f64 avg_ms = 0, max_ms = 0;
    bool ok = false;
};

struct TickTimer {
    u64 start_us = 0;  // after the scene loaded
    u64 last_us = 0;
    u64 max_us = 0;
};

}  // namespace

bool project_check(ToolCall& c) {
    if (!need_project(c)) return false;
    Options opt;
    u64 max_findings = 200;
    const cook::JsonValue checks = arg(c, "checks");
    if (checks.valid()) {
        if (!checks.is_array()) return arg_missing(c, "checks", "an array of check names");
        for (u32 i = 0; i < checks.size(); ++i) {
            bool known = false;
            for (usize k = 0; k < kCheckCount; ++k)
                if (checks.at(i).is_string() && checks.at(i).as_string() == kChecks[k]) known = opt.on[k] = true;
            if (!known) return arg_missing(c, "checks", "names from: cook, references, unused, credits, srgb, performance, accessibility");
        }
    } else {
        for (bool& b : opt.on) b = true;
    }
    if (!arg_uint(c, "ticks", opt.ticks, 1, 36000, false) || !arg_uint(c, "max_findings", max_findings, 1, 2000, false)) return false;
    if (const cook::JsonValue b = arg(c, "budget_ms"); b.valid()) {
        if (!b.is_number() || b.as_number() <= 0 || b.as_number() > 1000) return arg_missing(c, "budget_ms", "milliseconds per tick, 0..1000");
        opt.budget_ms = b.as_number();
        opt.budget_ms_given = true;
    }
    if (const cook::JsonValue t = arg(c, "target"); t.valid())
        if (!t.is_string() || !perf::parse_target(t.as_string(), opt.target)) return arg_missing(c, "target", "desktop, mobile or web");

    editor::EditorProject& project = c.server.session().project();
    if (!editor::project_budget(project, opt.target, opt.budget, c.error)) return false;
    if (!opt.budget_ms_given) opt.budget_ms = opt.budget.sim_ms;
    project.rescan();
    Report rep;

    // ---- the cook: every asset and scene must import
    const cook::CookReport& cooked = c.server.session().cook(false);
    Array<cook::String> failed_files;
    if (opt.enabled("cook")) {
        for (const cook::Diagnostic& d : cooked.diags.all()) {
            if (d.severity == cook::Severity::Info) continue;
            rep.add(d.severity == cook::Severity::Error ? Sev::Error : Sev::Warning, "cook", d.file.view(), d.line, d.message.view(),
                    d.code == Errc::MissingSidecar ? StringView("cook_run with adopt: true gives it a sidecar") : StringView());
            if (d.severity == cook::Severity::Error) failed_files.push_back(d.file);
        }
    }

    // ---- every file under assets/
    const cook::String assets_dir = project.asset_path("");
    Array<cook::String> files;
    (void)cook::fs::list_files(assets_dir.view(), files);
    Array<cook::String> texts;  // contents of the text assets (index-aligned with text_files)
    Array<cook::String> text_files;
    Array<Ref> refs;
    for (const cook::String& f : files) {
        if (!is_text_asset(f.view())) continue;
        cook::String text;
        if (!cook::fs::read_text(project.asset_path(f.view()).view(), text)) continue;
        scan_guids(text.view(), static_cast<u32>(text_files.size()), refs);
        text_files.push_back(f);
        texts.push_back(static_cast<cook::String&&>(text));
    }

    // ---- broken references: a GUID no asset has (cook errors already name their own files)
    if (opt.enabled("references")) {
        const bool mod = !project.base_roots().empty();
        for (const Ref& r : refs) {
            if (project.find(r.guid)) continue;
            cook::String file("assets/");
            file += text_files[r.file].view();
            bool reported = false;
            for (const cook::String& ff : failed_files) reported = reported || ff.view() == file.view();
            if (reported) continue;
            cook::String msg("references guid:");
            msg += asset::to_string(r.guid).view();
            msg += mod ? ", which no asset of this project has (it may be the base game's)" : ", which no asset has";
            rep.add(mod ? Sev::Info : Sev::Error, "references", file.view(), r.line, msg.view(),
                    "asset_list lists the GUIDs; point the field at an existing asset or restore the file");
        }
    }

    // ---- unused assets: nothing references the GUID, the path or (models) the file name
    if (opt.enabled("unused")) {
        // Code may load assets by path or GUID: the project's sources count as references.
        Array<cook::String> code;
        Array<cook::String> project_files;
        (void)cook::fs::list_files(project.root(), project_files);
        for (const cook::String& f : project_files) {
            const StringView v = f.view();
            if (v.substr(0, 7) == "assets/" || v.substr(0, 6) == "build/" || v.substr(0, 7) == "cooked/" || v.substr(0, 5) == "dist/") continue;
            if (!ends_with(v, ".cpp") && !ends_with(v, ".h") && !ends_with(v, ".hpp") && !ends_with(v, ".mceproj") && !ends_with(v, ".json") &&
                !ends_with(v, ".py") && !ends_with(v, ".mctest"))
                continue;
            cook::String text;
            if (cook::fs::read_text(cook::path_join(project.root(), v).view(), text)) code.push_back(static_cast<cook::String&&>(text));
        }
        // Models name their textures by file name (glTF uri).
        Array<cook::String> models;
        for (const cook::String& f : files) {
            if (!ends_with(f.view(), ".glb") && !ends_with(f.view(), ".gltf")) continue;
            cook::String bytes;
            if (cook::fs::read_text(project.asset_path(f.view()).view(), bytes)) models.push_back(static_cast<cook::String&&>(bytes));
        }
        for (const editor::ProjectAsset& a : project.assets()) {
            const StringView p = a.path.view();
            // Entry points: scenes, and input maps (the player loads the pack's first .mcinput itself).
            if (ends_with(p, ".mcscene") || ends_with(p, ".mcinput")) continue;
            bool used = false;
            for (const Ref& r : refs) used = used || r.guid == a.guid;
            const asset::GuidString gs = asset::to_string(a.guid);
            for (usize i = 0; !used && i < code.size(); ++i) used = contains_ci(code[i].view(), gs.view()) || code[i].view().find(p) != StringView::npos;
            for (usize i = 0; !used && i < models.size(); ++i) used = models[i].view().find(base_name(p)) != StringView::npos;
            for (usize i = 0; !used && i < texts.size(); ++i) used = texts[i].view().find(p) != StringView::npos;
            if (used) continue;
            cook::String file("assets/");
            file += p;
            rep.add(Sev::Info, "unused", file.view(), 0, "no scene, prefab, material, model or project source references this asset",
                    "use it, or asset_delete it before shipping (a game that loads it by a computed name can ignore this)");
        }
    }

    // ---- credits: every media file has a CREDITS.md row
    if (opt.enabled("credits")) {
        cook::String credits;
        const bool have = cook::fs::read_text(cook::path_join(project.root(), "CREDITS.md").view(), credits);
        Array<cook::String> pats;
        if (have) credit_patterns(credits.view(), pats);
        u32 missing = 0;
        for (const cook::String& f : files) {
            if (!is_media(f.view())) continue;
            bool covered = false;
            for (const cook::String& p : pats) covered = covered || glob_match(p.view(), f.view());
            if (covered) continue;
            ++missing;
            cook::String file("assets/");
            file += f.view();
            rep.add(Sev::Warning, "credits", file.view(), 0, have ? "no CREDITS.md row names this file" : "the project has no CREDITS.md",
                    "add a row: | `assets/<path>` | source and author | license | (library_add and the gen_* tools write theirs)");
        }
        (void)missing;
        // A row naming a file the project no longer has (asset_delete leaves its row behind).
        for (const cook::String& p : pats) {
            const StringView pv = p.view();
            if (pv.find('*') != StringView::npos) continue;
            bool found = false;
            for (usize i = 0; !found && i < files.size(); ++i)
                found = glob_match(pv, files[i].view()) || (pv.find('/') == StringView::npos && base_name(files[i].view()) == pv);
            if (found) continue;
            cook::String msg("CREDITS.md names ");
            if (pv.find('/') != StringView::npos) msg += "assets/";
            msg += pv;
            msg += ", which the project no longer has";
            rep.add(Sev::Warning, "credits", "CREDITS.md", 0, msg.view(), "remove the row, or restore the file");
        }
    }

    // ---- colour space: data textures must not be sRGB, colour textures should be
    if (opt.enabled("srgb")) {
        for (const editor::ProjectAsset& a : project.assets()) {
            if (StringView(editor::asset_type_name(a.type)) != "Texture") continue;
            const cook::String ext = cook::lower_extension(a.path.view());
            if (ext.view() == ".hdr" || ext.view() == ".exr") continue;  // linear float data, never sRGB
            cook::String side = project.asset_path(a.path.view());
            side += ".mceasset";
            cook::String side_text;
            if (!cook::fs::read_text(side.view(), side_text)) continue;
            const bool srgb = texture_is_srgb(side_text.view());
            const asset::GuidString gs = asset::to_string(a.guid);
            // How materials use it.
            bool as_data = false, as_color = false;
            for (usize i = 0; i < texts.size(); ++i) {
                if (!ends_with(text_files[i].view(), ".mcmat")) continue;
                cook::JsonDoc doc;
                cook::JsonError je;
                if (!doc.parse(texts[i].view(), je)) continue;
                const cook::JsonValue slots = doc.root().get("textures");
                for (u32 s = 0; slots.is_object() && s < slots.size(); ++s) {
                    if (!slots.at(s).is_string() || slots.at(s).as_string().find(gs.view()) == StringView::npos) continue;
                    bool data = false;
                    for (const char* d : kDataSlots) data = data || slots.key_at(s) == d;
                    (data ? as_data : as_color) = true;
                }
            }
            bool named_data = false;
            for (const char* n : kDataNames) named_data = named_data || contains_ci(a.path.view(), n);
            cook::String file("assets/");
            file += a.path.view();
            cook::String fix("asset_set_import_settings {\"path\": \"");
            fix += a.path.view();
            if (srgb && (as_data || (named_data && !as_color))) {
                fix += "\", \"settings\": {\"srgb\": false}}";
                rep.add(Sev::Warning, "srgb", file.view(), 0,
                        as_data ? "a material samples this texture as data (normal, roughness, ...) but it is imported as sRGB"
                                : "the name says data (normal map, mask, LUT) but it is imported as sRGB",
                        fix.view());
            } else if (!srgb && as_color && !as_data) {
                fix += "\", \"settings\": {\"srgb\": true}}";
                rep.add(Sev::Warning, "srgb", file.view(), 0, "a material uses this texture as colour but it is imported linear (srgb false): it will look washed out",
                        fix.view());
            }
        }
    }

    // ---- accessibility: text size and contrast in 2D scenes (a 1280 x 720 screen, as headless runs)
    if (opt.enabled("accessibility")) {
        for (usize i = 0; i < texts.size(); ++i) {
            if (!ends_with(text_files[i].view(), ".mcscene") && !ends_with(text_files[i].view(), ".mcprefab")) continue;
            cook::JsonDoc doc;
            cook::JsonError je;
            if (!doc.parse(texts[i].view(), je)) continue;
            const cook::JsonValue ents = doc.root().get("entities");
            // The primary 2D camera: pixels per metre on screen and the clear colour behind the text.
            f32 px_per_m = 48.0f, bg[3] = {0.08f, 0.09f, 0.12f};
            bool camera = false;
            for (u32 e = 0; ents.is_array() && e < ents.size() && !camera; ++e) {
                const cook::JsonValue cam = ents.at(e).get("components").get("Camera2DComponent");
                if (!cam.is_object() || (cam.get("primary").is_bool() && !cam.get("primary").as_bool())) continue;
                camera = true;
                const f32 fw = num_or(cam, "fit_width", 0), fh = num_or(cam, "fit_height", 0);
                px_per_m = fh > 0 ? 720.0f / fh : fw > 0 ? 1280.0f / fw : num_or(cam, "pixels_per_meter", 48) * num_or(cam, "zoom", 1);
                (void)read_color(cam.get("clear_color"), bg);
            }
            if (!camera) continue;
            const f32 bg_l = luminance(bg[0], bg[1], bg[2]);
            cook::String file("assets/");
            file += text_files[i].view();
            for (u32 e = 0; ents.is_array() && e < ents.size(); ++e) {
                const cook::JsonValue label = ents.at(e).get("components").get("TextLabel");
                if (!label.is_object()) continue;
                const cook::JsonValue name = ents.at(e).get("name");
                const StringView who = name.is_string() ? name.as_string() : StringView("a TextLabel");
                const f32 px = num_or(label, "height", 0.5f) * px_per_m;
                const u32 line = label.line();
                if (px < opt.min_text_px) {
                    cook::String msg;
                    cook::appendf(msg, "%.*s: text is %.1f px high on a 1280x720 screen (at least %.0f reads on a phone or TV)",
                                  static_cast<int>(who.size()), who.data(), static_cast<f64>(px), static_cast<f64>(opt.min_text_px));
                    cook::String fix;
                    cook::appendf(fix, "raise TextLabel.height to %.2f m or more", static_cast<f64>(opt.min_text_px / px_per_m));
                    rep.add(Sev::Warning, "accessibility", file.view(), line, msg.view(), fix.view());
                }
                // Behind the text: a backing panel (a Sprite under it), else the camera's clear colour.
                f32 panel[3];
                const f32 behind_l = panel_behind(ents, e, project, bg, panel) ? luminance(panel[0], panel[1], panel[2]) : bg_l;
                const auto label_contrast = [&](const cook::JsonValue& l) {
                    f32 fg[3] = {1, 1, 1};
                    (void)read_color(l.get("color"), fg);
                    return contrast(luminance(fg[0], fg[1], fg[2]), behind_l);
                };
                const f32 ratio = label_contrast(label);
                // A drop shadow: another label with the same text reads well, and this one only sits behind it.
                bool shadow = false;
                const cook::JsonValue text = label.get("text");
                for (u32 o = 0; ratio < 4.5f && text.is_string() && o < ents.size() && !shadow; ++o) {
                    const cook::JsonValue other = ents.at(o).get("components").get("TextLabel");
                    shadow = o != e && other.is_object() && other.get("text").is_string() && other.get("text").as_string() == text.as_string() &&
                             label_contrast(other) >= 4.5f;
                }
                if (ratio < 4.5f && !shadow) {
                    cook::String msg;
                    cook::appendf(msg, "%.*s: text contrast against what is behind it (a backing sprite, else the clear colour) is %.1f:1 (WCAG AA asks 4.5:1)",
                                  static_cast<int>(who.size()), who.data(), static_cast<f64>(ratio));
                    rep.add(Sev::Warning, "accessibility", file.view(), line, msg.view(), "a lighter or darker TextLabel.color, or a backing panel");
                }
            }
        }
    }

    // ---- accessibility: UI documents (.mcui), and subtitle coverage of timelines and sounds
    if (opt.enabled("accessibility")) {
        for (usize i = 0; i < texts.size(); ++i) {
            const StringView rel = text_files[i].view();
            const bool ui_doc = ends_with(rel, ".mcui"), timeline = ends_with(rel, ".mctimeline");
            const bool scene = ends_with(rel, ".mcscene") || ends_with(rel, ".mcprefab");
            if (!ui_doc && !timeline && !scene) continue;
            cook::JsonDoc doc;
            cook::JsonError je;
            if (!doc.parse(texts[i].view(), je)) continue;
            cook::String file("assets/");
            file += rel;
            if (ui_doc) {
                (void)check_ui_document(doc.root(), file.view(), project, opt.min_text_px, rep);
            } else if (timeline) {
                // Sounds and voices with no line on screen: players who cannot hear them miss them.
                const cook::JsonValue tracks = doc.root().get("tracks");
                bool audio = false, subtitle = false;
                u32 audio_line = 0;
                for (u32 t = 0; tracks.is_array() && t < tracks.size(); ++t) {
                    const cook::JsonValue type = tracks.at(t).get("type");
                    if (!type.is_string()) continue;
                    if (type.as_string() == "audio" && !audio) {
                        audio = true;
                        audio_line = tracks.at(t).line();
                    }
                    subtitle = subtitle || type.as_string() == "subtitle";
                }
                if (audio && !subtitle)
                    rep.add(Sev::Warning, "accessibility", file.view(), audio_line,
                            "the timeline plays sounds but has no subtitle track: players who cannot hear them miss what they say",
                            "add a {\"type\": \"subtitle\"} track with the lines (and \"[sound]\" lines for sounds that matter)");
            } else {
                const cook::JsonValue ents = doc.root().get("entities");
                for (u32 e = 0; ents.is_array() && e < ents.size(); ++e) {
                    const cook::JsonValue comps = ents.at(e).get("components");
                    const cook::JsonValue sfx = comps.get("SfxTrigger"), src = comps.get("AudioSource");
                    const bool music = src.is_object() && src.get("bus").is_string() && src.get("bus").as_string() == "Music";
                    if ((!sfx.is_object() && (!src.is_object() || music)) || comps.get("SoundCaption").is_object()) continue;
                    const cook::JsonValue name = ents.at(e).get("name");
                    const StringView who = name.is_string() ? name.as_string() : StringView("an entity");
                    cook::String msg;
                    cook::appendf(msg, "%.*s plays a sound with no caption (shown when a player turns access.captions on)",
                                  static_cast<int>(who.size()), who.data());
                    rep.add(Sev::Info, "accessibility", file.view(), (sfx.is_object() ? sfx : src).line(), msg.view(),
                            "add SoundCaption {\"text\": \"[what it is]\"} if the sound tells the player something");
                }
            }
        }
    }

    // ---- performance, static (M-PERF step 2): textures larger than the target allows, and scenes whose
    // sprites use more distinct textures than it allows (each texture is its own draw batch).
    if (opt.enabled("performance")) {
        const char* target = perf::target_name(opt.target);
        if (opt.budget.texture_size > 0)
            for (const cook::String& f : files) {
                const cook::String ext = cook::lower_extension(f.view());
                if (ext.view() != ".png" && ext.view() != ".jpg" && ext.view() != ".jpeg") continue;
                cook::String bytes;
                u32 w = 0, h = 0;
                if (!cook::fs::read_text(project.asset_path(f.view()).view(), bytes) ||
                    !cook::image_size(Span<const byte>(reinterpret_cast<const byte*>(bytes.c_str()), bytes.size()), w, h))
                    continue;
                if (w <= opt.budget.texture_size && h <= opt.budget.texture_size) continue;
                cook::String file("assets/");
                file += f.view();
                cook::String m;
                cook::appendf(m, "the image is %ux%u; the %s budget allows %u pixels a side (texture_size)", w, h, target,
                              opt.budget.texture_size);
                rep.add(Sev::Warning, "performance", file.view(), 0, m.view(),
                        "scale it down, or raise budgets.<target>.texture_size with project_set if the target can take it");
            }
        if (opt.budget.sprite_textures > 0)
            for (usize i = 0; i < text_files.size(); ++i) {
                if (!ends_with(text_files[i].view(), ".mcscene")) continue;
                cook::JsonDoc doc;
                cook::JsonError je;
                if (!doc.parse(texts[i].view(), je)) continue;
                const cook::JsonValue ents = doc.root().get("entities");
                Array<StringView> seen;
                for (u32 e = 0; ents.is_array() && e < ents.size(); ++e) {
                    const cook::JsonValue tex = ents.at(e).get("components").get("Sprite").get("texture");
                    if (!tex.is_string() || tex.as_string().empty()) continue;
                    bool known = false;
                    for (StringView s : seen) known = known || s == tex.as_string();
                    if (!known) seen.push_back(tex.as_string());
                }
                if (seen.size() <= opt.budget.sprite_textures) continue;
                cook::String file("assets/");
                file += text_files[i].view();
                cook::String m;
                cook::appendf(m, "sprites here use %zu different textures, and each one is a separate draw batch; the %s budget allows %u "
                                 "(sprite_textures)",
                              seen.size(), target, opt.budget.sprite_textures);
                rep.add(Sev::Warning, "performance", file.view(), 0, m.view(),
                        "pack the small images into one spritesheet (.mcsheet) so the sprites share a texture and a batch");
            }
    }

    // ---- performance: each scene headless, the sim's time per tick against the budget
    Array<PerfRow> perf;
    if (opt.enabled("performance") && !cooked.ok())
        rep.add(Sev::Error, "performance", "", 0, "no scene was measured: the project does not cook", "project_check with the cook check lists why");
    if (opt.enabled("performance") && cooked.ok()) {
        const cook::String cooked_dir = cook::path_join(project.root(), "cooked");
        for (const editor::ProjectAsset& a : project.assets()) {
            if (!ends_with(a.path.view(), ".mcscene")) continue;
            PerfRow row;
            row.scene = a.path;
            TickTimer timer;
            RunHooks hooks;
            hooks.user = &timer;
            hooks.start = [](World&, Span<const Entity>, void* user) {
                auto* t = static_cast<TickTimer*>(user);
                t->start_us = t->last_us = platform::now_us();
            };
            hooks.tick = [](const World&, Span<const Entity>, u64, void* user) {
                auto* t = static_cast<TickTimer*>(user);
                const u64 now = platform::now_us();
                if (now - t->last_us > t->max_us) t->max_us = now - t->last_us;
                t->last_us = now;
                return true;
            };
            Result<HeadlessRun> run = run_headless(cooked_dir.view(), a.guid, opt.ticks, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                   project.base_roots(), &hooks);
            cook::String file("assets/");
            file += a.path.view();
            if (!run) {
                FixedString<512> msg;
                format_error(run.error(), msg);
                cook::String m("the scene does not run: ");
                m += msg.view();
                rep.add(Sev::Error, "performance", file.view(), 0, m.view(), "run_headless on it shows the log");
                perf.push_back(static_cast<PerfRow&&>(row));
                continue;
            }
            row.ok = true;
            row.ticks = run.value().ticks;
            row.entities = run.value().metrics.entities;
            row.avg_ms = row.ticks ? static_cast<f64>(timer.last_us - timer.start_us) / 1000.0 / static_cast<f64>(row.ticks) : 0;
            row.max_ms = static_cast<f64>(timer.max_us) / 1000.0;
            if (opt.budget_ms > 0 && row.avg_ms > opt.budget_ms) {
                cook::String m;
                cook::appendf(m, "the sim takes %.2f ms per tick on average over %llu ticks (budget %.2f ms; the slowest tick %.2f ms)", row.avg_ms,
                              static_cast<unsigned long long>(row.ticks), opt.budget_ms, row.max_ms);
                rep.add(Sev::Warning, "performance", file.view(), 0, m.view(), "fewer entities or cheaper systems; run_headless metrics count them");
            }
            perf.push_back(static_cast<PerfRow&&>(row));
        }
    }

    // ---- the result
    u32 errors = 0, warnings = 0, infos = 0;
    for (const Finding& f : rep.findings) (f.sev == Sev::Error ? errors : f.sev == Sev::Warning ? warnings : infos) += 1;
    c.out.begin_object();
    c.out.key("pass").boolean(errors == 0);
    c.out.key("errors").num_u(errors).key("warnings").num_u(warnings).key("infos").num_u(infos);
    c.out.key("checks").begin_object();
    for (usize i = 0; i < kCheckCount; ++i) {
        if (!opt.on[i]) continue;
        c.out.key(kChecks[i]).begin_object().key("errors").num_u(rep.counts[i][0]).key("warnings").num_u(rep.counts[i][1]);
        c.out.key("infos").num_u(rep.counts[i][2]).end_object();
    }
    c.out.end_object();
    c.out.key("findings").begin_array();
    // Errors first, then warnings, then infos, each in check order.
    usize written = 0;
    for (u32 s = 0; s < 3; ++s)
        for (const Finding& f : rep.findings) {
            if (static_cast<u32>(f.sev) != s || written == max_findings) continue;
            ++written;
            c.out.begin_object().key("severity").str(sev_name(f.sev)).key("check").str(f.check);
            if (!f.file.empty()) c.out.key("file").str(f.file.view());
            if (f.line) c.out.key("line").num_u(f.line);
            c.out.key("message").str(f.message.view());
            if (!f.fix.empty()) c.out.key("fix").str(f.fix.view());
            c.out.end_object();
        }
    c.out.end_array();
    if (written < rep.findings.size()) c.out.key("truncated").num_u(rep.findings.size() - written);
    if (opt.enabled("performance")) {
        c.out.key("performance").begin_array();
        for (const PerfRow& r : perf) {
            c.out.begin_object().key("scene").str(r.scene.view()).key("ok").boolean(r.ok);
            if (r.ok) {
                c.out.key("ticks").num_u(r.ticks).key("entities").num_u(r.entities);
                c.out.key("avg_ms").num_f(std::round(r.avg_ms * 1000.0) / 1000.0).key("max_ms").num_f(std::round(r.max_ms * 1000.0) / 1000.0);
            }
            c.out.end_object();
        }
        c.out.end_array();
        c.out.key("budget_ms").num_f(opt.budget_ms);
        c.out.key("target").str(perf::target_name(opt.target));
        c.out.key("budget").begin_object();
        for (const perf::BudgetField& f : perf::budget_fields()) {
            if (f.integer) c.out.key(f.name).num_u(static_cast<u64>(f.get(opt.budget)));
            else c.out.key(f.name).num_f(std::round(f.get(opt.budget) * 1000.0) / 1000.0);
        }
        c.out.end_object();
    }
    c.out.end_object();
    return true;
}

}  // namespace mce::mcp
