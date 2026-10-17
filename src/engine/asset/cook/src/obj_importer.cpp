// Wavefront OBJ (+ MTL) -> CHNK_MESH (M-IMPORT3D step 2), on the shared mesh back end (mesh_build.h).
//
// Statements read: v (x y z, optionally w or r g b), vt, vn, f (triangles, quads and n-gons, fan-triangulated;
// negative indices count back from the last element defined at that line), o / g (a node each, created at its
// first face), usemtl (a submesh per node and material), mtllib. Ignored with one warning per kind and file
// (first line and count): l and p, free-form curves and surfaces, s smoothing groups, other attribute statements,
// vertex colours. Anything else is an error naming file:line.
//
// MTL keys: Kd, d / Tr, Ns (roughness = (2 / (Ns + 2))^(1/4), perceptual), Pr, Pm, Ke, map_Kd, map_Bump / bump / norm (a normal map),
// map_Ke, map_Pr + map_Pm (both naming one packed texture: G = roughness, B = metallic). Textures resolve to texture
// GUIDs relative to the .mtl, like glTF images; a missing one is an error naming the .mtl line. Other keys warn.
//
// The cook sees registered assets only, so a .mtl is an asset too (importer "mtl", cooked to the CHNK_MAT of its first
// material): the .obj declares it as a dependency and reads its bytes through IAssetLookup::source_bytes. A .mtl that
// is not there (or not adopted) is a warning and the materials fall back to a default.
//
// OBJ has no axes or units: sidecar settings "up": "y" (default) | "z" (+Z up is turned into +Y up before the back end
// sees it, handedness kept) and "scale" (applied to the root nodes, as in glTF). V of a texture coordinate is flipped
// (OBJ's origin is the bottom left). Without vn, normals are smooth per position, weighted by the corner angle.
// Also: "max_triangles", "lod_levels", "lod_ratio", "lod_error" as for glTF.
//
// The file is untrusted. Every number is parsed locale-free (std::from_chars) and must be finite and within 1e9; every
// index is range-checked at its line; lines, vertices, faces and nodes have fixed limits (kMax* below) that fail naming
// the limit before anything of that size is allocated. No recursion; CRLF, LF and CR line ends give the same bytes.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/asset/xxhash.h"
#include "mce/containers/hash_map.h"
#include "mesh_build.h"
#include "obj_importer.h"

namespace mce::cook {

namespace {

using detail::Geometry;

constexpr const char* kObjStage = "cook.obj";
constexpr const char* kMtlStage = "cook.mtl";

// ---- limits on untrusted input ----
constexpr usize kMaxFileBytes = usize(256) << 20;  // an .obj / .mtl file
constexpr usize kMaxLineBytes = usize(16) << 20;   // one logical line (continuations joined)
constexpr u32 kMaxVertices = 1u << 24;             // v, vt and vn each
constexpr u32 kMaxFaceCorners = 1u << 20;          // one n-gon
constexpr u64 kMaxCorners = u64(1) << 25;          // face corners of the file
constexpr u64 kMaxTriangles = u64(1) << 24;        // fan triangles of the file
constexpr u32 kMaxNodes = 1u << 20;                // o / g nodes that hold faces
constexpr u32 kMaxSubmeshes = 1u << 18;            // (node, usemtl) pairs that hold faces
constexpr u32 kMaxMaterials = 1u << 16;            // distinct usemtl / newmtl names
constexpr usize kMaxNameBytes = 4096;              // o / g / usemtl / newmtl / mtllib / texture path
constexpr u32 kMaxMtlLibs = 16;
constexpr u32 kMaxWarnKeys = 32;                   // distinct unsupported MTL keys reported by name
constexpr f64 kMaxCoordinate = 1e9;
constexpr u32 kNone = ~0u;

// ---- small text helpers ----

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f'; }

StringView trim(StringView s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

// The next whitespace-separated token of `s` (empty at the end); `s` is advanced past it.
StringView next_token(StringView& s) {
    usize i = 0;
    while (i < s.size() && is_space(s[i])) ++i;
    const usize start = i;
    while (i < s.size() && !is_space(s[i])) ++i;
    const StringView t = s.substr(start, i - start);
    s.remove_prefix(i);
    return t;
}

// `t`, shortened and made printable, for a message.
void quote(String& out, StringView t) {
    out += '\'';
    const usize n = std::min<usize>(t.size(), 40);
    for (usize i = 0; i < n; ++i) {
        const char c = t[i];
        out += (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    if (t.size() > n) out += "...";
    out += '\'';
}

enum class Next { Line, End, TooLong, Nul };

// Logical lines of a text: LF, CRLF and CR end a line, a trailing backslash joins the next one. `line` is the 1-based
// number of the first physical line of the logical one.
class LineReader {
public:
    explicit LineReader(StringView text) : text_(text) {
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF && static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF)
            pos_ = 3;  // UTF-8 byte order mark
    }

    Next next(StringView& out, u32& line) {
        if (pos_ >= text_.size()) return Next::End;
        joined_.clear();
        bool use_joined = false;
        line = line_;
        for (;;) {
            const usize start = pos_;
            usize i = start;
            while (i < text_.size() && text_[i] != '\n' && text_[i] != '\r') {
                if (text_[i] == '\0') {
                    line = line_;
                    return Next::Nul;
                }
                ++i;
            }
            const StringView phys = text_.substr(start, i - start);
            if (i < text_.size()) i += (text_[i] == '\r' && i + 1 < text_.size() && text_[i + 1] == '\n') ? 2 : 1;
            pos_ = i;
            ++line_;
            const StringView t = trim(phys);
            const bool cont = !t.empty() && t.back() == '\\' && pos_ < text_.size();
            if (!use_joined && !cont) {
                if (phys.size() > kMaxLineBytes) return Next::TooLong;
                out = phys;
                return Next::Line;
            }
            use_joined = true;
            const StringView piece = cont ? t.substr(0, t.size() - 1) : phys;
            if (joined_.size() + piece.size() + 1 > kMaxLineBytes) return Next::TooLong;
            for (char c : piece) joined_.push_back(c);
            if (!cont) {
                out = StringView(joined_.data(), joined_.size());
                return Next::Line;
            }
            joined_.push_back(' ');
        }
    }

private:
    StringView text_;
    usize pos_ = 0;
    u32 line_ = 1;
    Array<char> joined_;
};

// ---- numbers ----

enum class Num { Ok, Bad, NotFinite, TooBig };

// Whether a number from_chars called out of range is below 1 in magnitude (an underflow) rather than a huge one.
bool underflows(StringView t) {
    usize i = 0;
    if (i < t.size() && (t[i] == '-' || t[i] == '+')) ++i;
    i64 int_digits = 0;
    bool lead = true;
    for (; i < t.size() && t[i] >= '0' && t[i] <= '9'; ++i) {
        if (lead && t[i] == '0') continue;
        lead = false;
        ++int_digits;
    }
    if (int_digits == 0) return true;  // 0.000...1 or 0e5
    while (i < t.size() && t[i] != 'e' && t[i] != 'E') ++i;
    if (i >= t.size()) return false;
    ++i;
    bool neg = false;
    if (i < t.size() && (t[i] == '-' || t[i] == '+')) neg = t[i++] == '-';
    i64 e = 0;
    for (; i < t.size() && t[i] >= '0' && t[i] <= '9'; ++i) e = std::min<i64>(e * 10 + (t[i] - '0'), 1000000);
    return neg && e > int_digits;
}

Num parse_number(StringView t, f32& out) {
    if (!t.empty() && t[0] == '+') t.remove_prefix(1);
    if (t.empty() || t[0] == '+') return Num::Bad;
    f64 v = 0.0;
    const char* const end = t.data() + t.size();
    const auto r = std::from_chars(t.data(), end, v);
    if (r.ec == std::errc::invalid_argument || r.ptr != end) return Num::Bad;
    // Underflow (1e-400) is a zero; only overflow is too big.
    if (r.ec == std::errc::result_out_of_range) {
        if (!underflows(t)) return Num::TooBig;
        out = 0.0f;
        return Num::Ok;
    }
    if (!std::isfinite(v)) return Num::NotFinite;
    if (std::fabs(v) > kMaxCoordinate) return Num::TooBig;
    out = static_cast<f32>(v);
    return Num::Ok;
}

enum class Idx { Ok, Bad, TooBig };

Idx parse_int(StringView t, i64& out) {
    if (!t.empty() && t[0] == '+') t.remove_prefix(1);
    if (t.empty() || t[0] == '+') return Idx::Bad;
    const char* const end = t.data() + t.size();
    const auto r = std::from_chars(t.data(), end, out);
    if (r.ec == std::errc::result_out_of_range) return Idx::TooBig;
    if (r.ec != std::errc() || r.ptr != end) return Idx::Bad;
    return Idx::Ok;
}

// ---- diagnostics ----

class Reporter {
public:
    Reporter(StringView file, const char* stage, DiagList& diags) : file_(file), stage_(stage), diags_(diags) {}

    bool fail(u32 line, StringView msg, Errc code = Errc::ImportFailed) {
        diags_.error(code, file_, line, 0, stage_, msg);
        return false;
    }
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    bool failf(u32 line, const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        return fail(line, buf);
    }
    void warn(u32 line, StringView msg) { diags_.warning(file_, line, 0, stage_, msg); }
    StringView file() const { return file_; }

    bool number_error(u32 line, StringView tok, Num n) {
        String m;
        switch (n) {
            case Num::NotFinite:
                quote(m, tok);
                m += " is not a finite number";
                break;
            case Num::TooBig:
                quote(m, tok);
                m += " is out of range (the importer takes |value| <= 1e9)";
                break;
            default:
                m += "invalid number ";
                quote(m, tok);
                break;
        }
        return fail(line, m.view());
    }

private:
    String file_;
    const char* stage_;
    DiagList& diags_;
};

// A counted warning: first line and number of statements.
struct Warn {
    u32 count = 0;
    u32 line = 0;
    void hit(u32 l) {
        if (count == 0) line = l;
        if (count < kNone) ++count;
    }
};

void emit(Reporter& rep, const Warn& w, const char* what) {
    if (w.count == 0) return;
    String m;
    appendf(m, "%s (first: line %u, %u statement%s)", what, w.line, w.count, w.count == 1 ? "" : "s");
    rep.warn(w.line, m.view());
}

// ---- names ----

// Distinct names in first-seen order, looked up by hash with the strings compared.
class NameTable {
public:
    i32 find(StringView s) const {
        const u32* head = head_.find(fnv1a64(s));
        if (!head) return -1;
        for (u32 i = *head; i != kNone; i = entries_[i].next)
            if (entries_[i].name.view() == s) return static_cast<i32>(i);
        return -1;
    }
    u32 add(StringView s) {
        const auto at = static_cast<u32>(entries_.size());
        Entry e;
        e.name.assign(s);
        const u64 h = fnv1a64(s);
        auto [slot, inserted] = head_.try_emplace(h, at);
        e.next = inserted ? kNone : *slot;
        if (!inserted) *slot = at;
        entries_.push_back(std::move(e));
        return at;
    }
    usize size() const { return entries_.size(); }
    StringView name(u32 i) const { return entries_[i].name.view(); }

private:
    struct Entry {
        String name;
        u32 next = kNone;
    };
    Array<Entry> entries_;
    HashMap<u64, u32> head_;
};

// Path of `name` relative to assets/, given the directory of the file that names it (`from`: "models/a.obj"). Backslashes
// are separators; absolute and escaping paths are rejected.
bool resolve_rel(StringView from, StringView name, String& out) {
    out.clear();
    if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != StringView::npos) return false;
    String joined;
    const usize slash = from.rfind('/');
    if (slash != StringView::npos) joined += from.substr(0, slash + 1);
    for (char c : name) joined += c == '\\' ? '/' : c;
    Array<StringView> parts;
    const StringView j = joined.view();
    usize start = 0;
    for (usize i = 0; i <= j.size(); ++i) {
        if (i < j.size() && j[i] != '/') continue;
        const StringView seg = j.substr(start, i - start);
        start = i + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (parts.empty()) return false;
            parts.pop_back();
            continue;
        }
        parts.push_back(seg);
    }
    for (usize i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return !out.empty();
}

// ============================ MTL ============================

struct TexReq {
    String path;
    u32 line = 0;  // 0 = absent
};

struct MtlMaterial {
    String name;
    u32 line = 0;
    asset::MaterialChunk m{};
    TexReq kd, bump, ke, pr, pm;
    bool pr_set = false, pm_set = false, ke_set = false, d_set = false;  // given explicitly in the MTL
};

struct MtlFile {
    String display;  // "assets/models/a.mtl"
    String rel;      // "models/a.mtl"
    Guid guid;
    Array<MtlMaterial> mats;
    NameTable names;
};

asset::MaterialChunk default_material() {
    asset::MaterialChunk m{};
    m.base_color[0] = m.base_color[1] = m.base_color[2] = m.base_color[3] = 1.0f;
    m.metallic = 0.0f;
    m.roughness = 1.0f;
    m.alpha_cutoff = 0.5f;
    return m;
}

struct UnsupportedKey {
    String key;
    Warn warn;
};

f32 clamp01(f32 v) { return std::min(1.0f, std::max(0.0f, v)); }

class MtlParser {
public:
    MtlParser(Reporter& rep, MtlFile& file) : rep_(rep), file_(file) {}

    bool run(StringView text) {
        if (text.size() > kMaxFileBytes) return rep_.failf(0, "the .mtl is over the importer's limit of %zu MiB", kMaxFileBytes >> 20);
        LineReader lr(text);
        for (;;) {
            StringView line;
            u32 no = 0;
            switch (lr.next(line, no)) {
                case Next::End: finish(); return true;
                case Next::TooLong: return rep_.failf(no, "line is over the importer's limit of %zu MiB", kMaxLineBytes >> 20);
                case Next::Nul: return rep_.fail(no, "binary data (a NUL byte) in a text file");
                case Next::Line:
                    if (!statement(line, no)) return false;
                    break;
            }
        }
    }

private:
    bool numbers(StringView rest, u32 line, const char* key, f32* out, u32 max, u32& n) {
        n = 0;
        for (;;) {
            const StringView t = next_token(rest);
            if (t.empty() || t[0] == '#') return true;
            if (n == max) return rep_.failf(line, "'%s' has more than %u values", key, max);
            const Num r = parse_number(t, out[n]);
            if (r != Num::Ok) return rep_.number_error(line, t, r);
            ++n;
        }
    }

    void unsupported(StringView key, u32 line) {
        for (UnsupportedKey& u : unsupported_)
            if (u.key.view() == key) {
                u.warn.hit(line);
                return;
            }
        if (unsupported_.size() >= kMaxWarnKeys) {
            other_.hit(line);
            return;
        }
        UnsupportedKey u;
        u.key.assign(key);
        u.warn.hit(line);
        unsupported_.push_back(std::move(u));
    }

    // map_xx [options] file
    bool texture(StringView rest, u32 line, TexReq& out) {
        for (;;) {
            StringView probe = rest;
            const StringView t = next_token(probe);
            if (t.size() < 2 || t[0] != '-') break;
            u32 args = 0;
            u32 optional_args = 0;
            if (t == "-blendu" || t == "-blendv" || t == "-cc" || t == "-clamp" || t == "-imfchan" || t == "-bm" || t == "-texres" || t == "-boost" ||
                t == "-type")
                args = 1;
            else if (t == "-mm")
                args = 2;
            else if (t == "-o" || t == "-s" || t == "-t") {
                args = 1;
                optional_args = 2;
            } else {
                String m("unknown texture option ");
                quote(m, t);
                return rep_.fail(line, m.view());
            }
            options_.hit(line);
            rest = probe;
            for (u32 a = 0; a < args; ++a)
                if (next_token(rest).empty()) return rep_.failf(line, "texture option '%.*s' needs a value", static_cast<int>(t.size()), t.data());
            for (u32 a = 0; a < optional_args; ++a) {
                StringView look = rest;
                const StringView v = next_token(look);
                f32 dummy = 0;
                if (v.empty() || parse_number(v, dummy) != Num::Ok) break;
                rest = look;
            }
        }
        const StringView path = trim(rest);
        if (path.empty()) return rep_.fail(line, "texture statement has no file name");
        if (path.size() > kMaxNameBytes) return rep_.failf(line, "texture path is over the importer's limit of %zu bytes", kMaxNameBytes);
        out.path.assign(path);
        out.line = line;
        if (path[0] == '/' || path[0] == '\\' || path.find(':') != StringView::npos) {
            // An absolute or drive-letter path from another machine: use the file name next to the .mtl.
            usize cut = path.size();
            while (cut > 0 && path[cut - 1] != '/' && path[cut - 1] != '\\') --cut;
            const StringView base = path.substr(cut);
            const usize colon = base.find(':');
            const StringView name = colon == StringView::npos ? base : base.substr(colon + 1);
            if (!name.empty()) {
                out.path.assign(name);
                absolute_.hit(line);
            }
        }
        return true;
    }

    bool statement(StringView text, u32 line) {
        StringView rest = text;
        const StringView key = next_token(rest);
        if (key.empty() || key[0] == '#') return true;
        if (key == "newmtl") {
            const StringView name = trim(rest);
            if (name.empty()) return rep_.fail(line, "newmtl needs a name");
            if (name.size() > kMaxNameBytes) return rep_.failf(line, "material name is over the importer's limit of %zu bytes", kMaxNameBytes);
            if (file_.mats.size() >= kMaxMaterials) return rep_.failf(line, "more than %u materials is over the importer's limit", kMaxMaterials);
            if (file_.names.find(name) >= 0) {
                String m("material ");
                quote(m, name);
                m += " is defined twice: the first definition is used";
                rep_.warn(line, m.view());
                cur_ = kNone;
                skip_ = true;
                return true;
            }
            file_.names.add(name);
            MtlMaterial mat;
            mat.name.assign(name);
            mat.line = line;
            mat.m = default_material();
            file_.mats.push_back(std::move(mat));
            cur_ = static_cast<u32>(file_.mats.size() - 1);
            skip_ = false;
            return true;
        }
        if (skip_ && cur_ == kNone) return true;  // inside a duplicate definition
        if (cur_ == kNone) {
            String m("statement ");
            quote(m, key);
            m += " before the first newmtl";
            return rep_.fail(line, m.view());
        }
        MtlMaterial& mat = file_.mats[cur_];
        f32 v[3] = {0, 0, 0};
        u32 n = 0;
        if (key == "Kd" || key == "Ke") {
            StringView probe = rest;
            const StringView first = next_token(probe);
            if (first == "spectral" || first == "xyz") {
                unsupported(key, line);
                return true;
            }
            if (!numbers(rest, line, key == "Kd" ? "Kd" : "Ke", v, 3, n)) return false;
            if (n != 1 && n != 3) return rep_.failf(line, "'%.*s' needs 1 or 3 values", static_cast<int>(key.size()), key.data());
            if (n == 1) v[1] = v[2] = v[0];
            f32* dst = key == "Kd" ? mat.m.base_color : mat.m.emissive;
            for (int c = 0; c < 3; ++c) dst[c] = key == "Kd" ? clamp01(v[c]) : std::max(0.0f, v[c]);
            if (key == "Ke") mat.ke_set = true;
        } else if (key == "d" || key == "Tr") {
            StringView r = rest;
            StringView t = next_token(r);
            if (t == "-halo") t = next_token(r);
            f32 a = 0;
            if (t.empty()) return rep_.failf(line, "'%.*s' needs a value", static_cast<int>(key.size()), key.data());
            const Num nr = parse_number(t, a);
            if (nr != Num::Ok) return rep_.number_error(line, t, nr);
            if (key == "Tr" && mat.d_set) return true;  // d wins over Tr, whichever comes first
            if (key == "d") mat.d_set = true;
            a = clamp01(a);
            const f32 alpha = key == "d" ? a : 1.0f - a;
            mat.m.base_color[3] = alpha;
            if (alpha < 1.0f) mat.m.flags |= asset::MaterialFlags::AlphaBlend;
            else mat.m.flags &= ~asset::MaterialFlags::AlphaBlend;
        } else if (key == "Ns") {
            if (!numbers(rest, line, "Ns", v, 1, n)) return false;
            if (n != 1) return rep_.fail(line, "'Ns' needs a value");
            // Shininess to the engine's perceptual roughness (the shader squares it): alpha^2 = 2 / (Ns + 2).
            if (!mat.pr_set) {
                const f32 ns = std::min(1000.0f, v[0]);
                mat.m.roughness = ns <= 0.0f ? 1.0f : clamp01(std::pow(2.0f / (ns + 2.0f), 0.25f));
            }
        } else if (key == "Pr") {
            if (!numbers(rest, line, "Pr", v, 1, n)) return false;
            if (n != 1) return rep_.fail(line, "'Pr' needs a value");
            mat.m.roughness = clamp01(v[0]);
            mat.pr_set = true;
        } else if (key == "Pm") {
            if (!numbers(rest, line, "Pm", v, 1, n)) return false;
            if (n != 1) return rep_.fail(line, "'Pm' needs a value");
            mat.m.metallic = clamp01(v[0]);
            mat.pm_set = true;
        } else if (key == "map_Kd") {
            if (!texture(rest, line, mat.kd)) return false;
        } else if (key == "map_Bump" || key == "map_bump" || key == "bump" || key == "norm") {
            if (!texture(rest, line, mat.bump)) return false;
        } else if (key == "map_Ke") {
            if (!texture(rest, line, mat.ke)) return false;
        } else if (key == "map_Pr") {
            if (!texture(rest, line, mat.pr)) return false;
        } else if (key == "map_Pm") {
            if (!texture(rest, line, mat.pm)) return false;
        } else if (key == "Ka" || key == "Ks" || key == "Ni" || key == "illum" || key == "Tf" || key == "sharpness") {
            // Carry nothing the engine's metallic-roughness material uses.
        } else {
            unsupported(key, line);
        }
        return true;
    }

    void finish() {
        for (const UnsupportedKey& u : unsupported_) {
            String m("MTL key ");
            quote(m, u.key.view());
            appendf(m, " is not imported (first: line %u, %u use%s)", u.warn.line, u.warn.count, u.warn.count == 1 ? "" : "s");
            rep_.warn(u.warn.line, m.view());
        }
        emit(rep_, other_, "more unsupported MTL keys are not imported");
        emit(rep_, options_, "texture options (-s, -o, -bm, ...) are not applied");
        emit(rep_, absolute_, "absolute texture paths are read as the file name next to the .mtl");
    }

    Reporter& rep_;
    MtlFile& file_;
    u32 cur_ = kNone;
    bool skip_ = false;
    Array<UnsupportedKey> unsupported_;
    Warn other_, options_, absolute_;
};

// Resolves one texture request to its registered texture asset; reports an error naming the .mtl line when it cannot.
bool resolve_texture(const IAssetLookup* lookup, const MtlFile& f, const TexReq& r, Guid& out, Reporter* rep) {
    String rel;
    if (!resolve_rel(f.rel.view(), r.path.view(), rel)) {
        if (rep) {
            String m("texture ");
            quote(m, r.path.view());
            m += " must be a relative path that stays inside assets/";
            rep->fail(r.line, m.view(), Errc::MissingRef);
        }
        return false;
    }
    if (!lookup || !lookup->guid_for_path(rel.view(), out)) {
        if (rep) {
            String m("texture ");
            quote(m, rel.view());
            m += " is not a registered asset: add the image next to the model and adopt it";
            rep->fail(r.line, m.view(), Errc::MissingRef);
        }
        return false;
    }
    return true;
}

void set_texture(asset::MaterialChunk& m, asset::MaterialSlot slot, Guid g) {
    m.textures[static_cast<u32>(slot)][0] = g.hi;
    m.textures[static_cast<u32>(slot)][1] = g.lo;
}

// The material's chunk with its textures resolved. False (errors reported) when one is missing.
bool build_material(const IAssetLookup* lookup, const MtlFile& f, const MtlMaterial& mat, Reporter& rep, asset::MaterialChunk& out) {
    out = mat.m;
    bool ok = true;
    auto slot = [&](const TexReq& r, asset::MaterialSlot s) {
        if (r.line == 0) return;
        Guid g;
        if (resolve_texture(lookup, f, r, g, &rep)) set_texture(out, s, g);
        else ok = false;
    };
    slot(mat.kd, asset::MaterialSlot::BaseColor);
    slot(mat.bump, asset::MaterialSlot::Normal);
    slot(mat.ke, asset::MaterialSlot::Emissive);
    if (mat.ke.line != 0 && !mat.ke_set) out.emissive[0] = out.emissive[1] = out.emissive[2] = 1.0f;
    if (mat.pr.line != 0 || mat.pm.line != 0) {
        Guid gr, gm;
        bool has_r = false, has_m = false;
        if (mat.pr.line != 0) {
            has_r = resolve_texture(lookup, f, mat.pr, gr, &rep);
            ok = ok && has_r;
        }
        if (mat.pm.line != 0) {
            has_m = resolve_texture(lookup, f, mat.pm, gm, &rep);
            ok = ok && has_m;
        }
        if (has_r && has_m && gr == gm) {
            set_texture(out, asset::MaterialSlot::MetallicRoughness, gr);
            // The shader multiplies the packed texture by the factors: default them to 1 unless the MTL gave them.
            if (!mat.pr_set) out.roughness = 1.0f;
            if (!mat.pm_set) out.metallic = 1.0f;
        } else if (ok) {
            rep.warn(mat.pr.line != 0 ? mat.pr.line : mat.pm.line,
                     "map_Pr and map_Pm are imported only when both name one packed texture (G = roughness, B = metallic): ignored");
        }
    }
    return ok;
}

// Texture dependencies of every material of `f`, without reporting.
void texture_deps(const IAssetLookup* lookup, const MtlFile& f, Array<DepRef>& out) {
    auto add = [&](const TexReq& r, const char* where) {
        if (r.line == 0) return;
        Guid g;
        if (!resolve_texture(lookup, f, r, g, nullptr)) return;
        for (const DepRef& e : out)
            if (e.guid == g) return;
        DepRef d;
        d.guid = g;
        d.expected = ChunkType::Texture;
        d.line = r.line;
        d.col = 1;
        String w(f.display.view());
        w += ": ";
        w += where;
        d.where.assign(w.view());
        out.push_back(d);
    };
    for (const MtlMaterial& m : f.mats) {
        add(m.kd, "map_Kd");
        add(m.bump, "map_Bump");
        add(m.ke, "map_Ke");
        add(m.pr, "map_Pr");
        add(m.pm, "map_Pm");
    }
}

String display_of(StringView rel) {
    String d("assets/");
    d += rel;
    return d;
}

// ============================ OBJ ============================

struct Corner {
    u32 v = kNone, vt = kNone, vn = kNone;
    friend bool operator==(const Corner& a, const Corner& b) { return a.v == b.v && a.vt == b.vt && a.vn == b.vn; }
};

struct CornerHash {
    u64 operator()(const Corner& c) const { return hash_combine(hash_combine(c.v, c.vt), c.vn); }
};

struct Face {
    u32 first;
    u32 count;
    u32 group;
    u32 line;
};

struct Group {
    u32 node;
    u32 mat;  // index into ObjData::mats, kNone = default
};

struct LibRef {
    String name;
    u32 line = 0;
};

struct ObjData {
    Array<f32> pos, uv, nrm;  // 3 / 2 / 3 per element; pos and nrm already in the engine's +Y-up space
    Array<Corner> corners;
    Array<Face> faces;
    Array<Group> groups;
    Array<asset::MeshNode> nodes;
    Array<char> strings;
    NameTable mats;
    Array<u32> mat_line;  // first usemtl line of each name
    Array<LibRef> libs;
    bool need_normals = false;
    Warn w_pl, w_curve, w_smooth, w_other, w_color;
};

class ObjParser {
public:
    ObjParser(Reporter& rep, bool up_z, ObjData& d) : rep_(rep), up_z_(up_z), d_(d) {}

    bool run(StringView text) {
        if (text.size() > kMaxFileBytes) return rep_.failf(0, "the file is over the importer's limit of %zu MiB", kMaxFileBytes >> 20);
        LineReader lr(text);
        for (;;) {
            StringView line;
            u32 no = 0;
            switch (lr.next(line, no)) {
                case Next::End: return true;
                case Next::TooLong: return rep_.failf(no, "line is over the importer's limit of %zu MiB", kMaxLineBytes >> 20);
                case Next::Nul: return rep_.fail(no, "binary data (a NUL byte) in a text file");
                case Next::Line:
                    if (!statement(line, no)) return false;
                    break;
            }
        }
    }

private:
    bool numbers(StringView rest, u32 line, const char* key, f32* out, u32 max, u32& n) {
        n = 0;
        for (;;) {
            const StringView t = next_token(rest);
            if (t.empty() || t[0] == '#') return true;
            if (n == max) return rep_.failf(line, "'%s' has more than %u values", key, max);
            const Num r = parse_number(t, out[n]);
            if (r != Num::Ok) return rep_.number_error(line, t, r);
            ++n;
        }
    }

    // One index of a face vertex at the current element count; negative ones count back from the last element.
    bool index(StringView t, u32 count, const char* what, u32 line, u32& out) {
        i64 v = 0;
        const Idx r = parse_int(t, v);
        if (r != Idx::Ok) {
            String m("invalid ");
            m += what;
            m += " index ";
            quote(m, t);
            return rep_.fail(line, m.view());
        }
        if (v == 0) return rep_.failf(line, "%s index 0 is not valid (indices start at 1)", what);
        if (v > 0) {
            if (v > static_cast<i64>(count)) return rep_.failf(line, "%s index %lld is out of range (%u defined so far)", what, static_cast<long long>(v), count);
            out = static_cast<u32>(v - 1);
            return true;
        }
        if (v < -static_cast<i64>(count))  // (not -v: that overflows for the smallest i64)
            return rep_.failf(line, "%s index %lld reaches before the first one (%u defined so far)", what, static_cast<long long>(v), count);
        out = static_cast<u32>(static_cast<i64>(count) + v);
        return true;
    }

    bool add_name(StringView rest, u32 line, const char* key, String& out) {
        const StringView name = trim(rest);
        if (name.size() > kMaxNameBytes) return rep_.failf(line, "%s name is over the importer's limit of %zu bytes", key, kMaxNameBytes);
        out.assign(name);
        return true;
    }

    bool make_node(u32 line) {
        if (d_.nodes.size() >= kMaxNodes) return rep_.failf(line, "more than %u nodes (o / g with faces) is over the importer's limit", kMaxNodes);
        asset::MeshNode mn{};
        mn.parent = -1;
        mn.scale[0] = mn.scale[1] = mn.scale[2] = 1.0f;
        mn.rotation[3] = 1.0f;
        mn.name_offset = static_cast<u32>(d_.strings.size());
        mn.name_size = static_cast<u32>(pending_name_.size());
        for (char c : pending_name_.view()) d_.strings.push_back(c);
        d_.nodes.push_back(mn);
        cur_node_ = static_cast<u32>(d_.nodes.size() - 1);
        return true;
    }

    bool current_group(u32 line, u32& out) {
        if (cur_group_ == kNone) {
            if (cur_node_ == kNone && !make_node(line)) return false;
            const u64 key = (u64(cur_node_) << 32) | u64(cur_mat_ == kNone ? 0u : cur_mat_ + 1u);
            auto [slot, inserted] = group_of_.try_emplace(key, static_cast<u32>(d_.groups.size()));
            if (inserted) {
                if (d_.groups.size() >= kMaxSubmeshes)
                    return rep_.failf(line, "more than %u (node, material) submeshes is over the importer's limit", kMaxSubmeshes);
                d_.groups.push_back({cur_node_, cur_mat_});
            }
            cur_group_ = *slot;
        }
        out = cur_group_;
        return true;
    }

    bool face(StringView rest, u32 line) {
        const u32 nv = static_cast<u32>(d_.pos.size() / 3), nt = static_cast<u32>(d_.uv.size() / 2), nn = static_cast<u32>(d_.nrm.size() / 3);
        const auto first = static_cast<u32>(d_.corners.size());
        u32 n = 0;
        bool missing_normal = false;
        for (;;) {
            const StringView t = next_token(rest);
            if (t.empty() || t[0] == '#') break;
            if (n == kMaxFaceCorners) return rep_.failf(line, "a face with more than %u vertices is over the importer's limit", kMaxFaceCorners);
            if (d_.corners.size() >= kMaxCorners) return rep_.failf(line, "more than %llu face corners is over the importer's limit", static_cast<unsigned long long>(kMaxCorners));
            StringView part[3];
            u32 np = 0;
            bool ok = true;
            usize start = 0;
            for (usize i = 0; i <= t.size() && ok; ++i) {
                if (i < t.size() && t[i] != '/') continue;
                if (np == 3) ok = false;
                else part[np++] = t.substr(start, i - start);
                start = i + 1;
            }
            if (!ok || part[0].empty() || (np == 2 && part[1].empty()) || (np == 3 && part[2].empty())) {
                String m("invalid face vertex ");
                quote(m, t);
                return rep_.fail(line, m.view());
            }
            Corner c;
            if (!index(part[0], nv, "vertex", line, c.v)) return false;
            if (np >= 2 && !part[1].empty() && !index(part[1], nt, "texture coordinate", line, c.vt)) return false;
            if (np == 3 && !index(part[2], nn, "normal", line, c.vn)) return false;
            missing_normal = missing_normal || c.vn == kNone;
            d_.corners.push_back(c);
            ++n;
        }
        if (n < 3) return rep_.failf(line, "a face needs at least 3 vertices, this one has %u", n);
        triangles_ += n - 2;
        if (triangles_ > kMaxTriangles) return rep_.failf(line, "more than %llu triangles is over the importer's limit", static_cast<unsigned long long>(kMaxTriangles));
        u32 group = 0;
        if (!current_group(line, group)) return false;
        d_.faces.push_back({first, n, group, line});
        d_.need_normals = d_.need_normals || missing_normal;
        return true;
    }

    bool statement(StringView text, u32 line) {
        StringView rest = text;
        const StringView key = next_token(rest);
        if (key.empty() || key[0] == '#') return true;
        f32 v[7] = {0, 0, 0, 0, 0, 0, 0};
        u32 n = 0;
        if (key == "v") {
            if (d_.pos.size() / 3 >= kMaxVertices) return rep_.failf(line, "more than %u vertices is over the importer's limit", kMaxVertices);
            if (!numbers(rest, line, "v", v, 6, n)) return false;
            if (n != 3 && n != 4 && n != 6) return rep_.fail(line, "'v' needs x y z (optionally w, or r g b)");
            if (n == 6) d_.w_color.hit(line);
            d_.pos.push_back(v[0]);
            d_.pos.push_back(up_z_ ? v[2] : v[1]);
            d_.pos.push_back(up_z_ ? -v[1] : v[2]);
        } else if (key == "vt") {
            if (d_.uv.size() / 2 >= kMaxVertices) return rep_.failf(line, "more than %u texture coordinates is over the importer's limit", kMaxVertices);
            if (!numbers(rest, line, "vt", v, 3, n)) return false;
            if (n < 1) return rep_.fail(line, "'vt' needs u (and v)");
            d_.uv.push_back(v[0]);
            d_.uv.push_back(n >= 2 ? v[1] : 0.0f);
        } else if (key == "vn") {
            if (d_.nrm.size() / 3 >= kMaxVertices) return rep_.failf(line, "more than %u normals is over the importer's limit", kMaxVertices);
            if (!numbers(rest, line, "vn", v, 3, n)) return false;
            if (n != 3) return rep_.fail(line, "'vn' needs x y z");
            d_.nrm.push_back(v[0]);
            d_.nrm.push_back(up_z_ ? v[2] : v[1]);
            d_.nrm.push_back(up_z_ ? -v[1] : v[2]);
        } else if (key == "f") {
            return face(rest, line);
        } else if (key == "o" || key == "g") {
            if (!add_name(rest, line, "o / g", pending_name_)) return false;
            cur_node_ = kNone;
            cur_group_ = kNone;
        } else if (key == "usemtl") {
            const StringView name = trim(rest);
            if (name.empty()) return rep_.fail(line, "usemtl needs a material name");
            if (name.size() > kMaxNameBytes) return rep_.failf(line, "material name is over the importer's limit of %zu bytes", kMaxNameBytes);
            i32 at = d_.mats.find(name);
            if (at < 0) {
                if (d_.mats.size() >= kMaxMaterials) return rep_.failf(line, "more than %u materials is over the importer's limit", kMaxMaterials);
                at = static_cast<i32>(d_.mats.add(name));
                d_.mat_line.push_back(line);
            }
            cur_mat_ = static_cast<u32>(at);
            cur_group_ = kNone;
        } else if (key == "mtllib") {
            const StringView name = trim(rest);
            if (name.empty()) return rep_.fail(line, "mtllib needs a file name");
            if (name.size() > kMaxNameBytes) return rep_.failf(line, "mtllib name is over the importer's limit of %zu bytes", kMaxNameBytes);
            if (d_.libs.size() >= kMaxMtlLibs) return rep_.failf(line, "more than %u mtllib statements is over the importer's limit", kMaxMtlLibs);
            LibRef lib;
            lib.name.assign(name);
            lib.line = line;
            d_.libs.push_back(std::move(lib));
        } else if (key == "l" || key == "p") {
            d_.w_pl.hit(line);
        } else if (key == "s") {
            const StringView arg = trim(rest);
            if (!arg.empty() && arg != "off" && arg != "0") d_.w_smooth.hit(line);
        } else if (key == "vp" || key == "cstype" || key == "deg" || key == "bmat" || key == "step" || key == "curv" || key == "curv2" || key == "surf" ||
                   key == "parm" || key == "trim" || key == "hole" || key == "scrv" || key == "sp" || key == "end" || key == "con") {
            d_.w_curve.hit(line);
        } else if (key == "mg" || key == "bevel" || key == "c_interp" || key == "d_interp" || key == "lod" || key == "usemap" || key == "maplib" ||
                   key == "shadow_obj" || key == "trace_obj" || key == "ctech" || key == "stech") {
            d_.w_other.hit(line);
        } else {
            String m("unknown statement ");
            quote(m, key);
            return rep_.fail(line, m.view());
        }
        return true;
    }

    Reporter& rep_;
    bool up_z_;
    ObjData& d_;
    String pending_name_;
    u32 cur_node_ = kNone;
    u32 cur_mat_ = kNone;
    u32 cur_group_ = kNone;
    u64 triangles_ = 0;
    HashMap<u64, u32> group_of_;
};

// Smooth normals per position, weighted by the polygon's corner angle (the file has no vn for some corner).
void smooth_normals(const ObjData& d, Array<f32>& out) {
    const usize nv = d.pos.size() / 3;
    Array<f64> acc;
    acc.resize(nv * 3, 0.0);
    for (const Face& f : d.faces) {
        const Corner* c = &d.corners[f.first];
        auto at = [&](u32 i, int k) { return static_cast<f64>(d.pos[usize(c[i % f.count].v) * 3 + usize(k)]); };
        f64 nx = 0, ny = 0, nz = 0;
        for (u32 i = 0; i < f.count; ++i) {  // Newell's method
            nx += (at(i, 1) - at(i + 1, 1)) * (at(i, 2) + at(i + 1, 2));
            ny += (at(i, 2) - at(i + 1, 2)) * (at(i, 0) + at(i + 1, 0));
            nz += (at(i, 0) - at(i + 1, 0)) * (at(i, 1) + at(i + 1, 1));
        }
        const f64 len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!(len > 0.0)) continue;
        nx /= len;
        ny /= len;
        nz /= len;
        for (u32 i = 0; i < f.count; ++i) {
            const u32 prev = i + f.count - 1, next = i + 1;
            f64 a[3], b[3];
            for (int k = 0; k < 3; ++k) {
                a[k] = at(prev, k) - at(i, k);
                b[k] = at(next, k) - at(i, k);
            }
            const f64 cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
            const f64 s = std::sqrt(cx * cx + cy * cy + cz * cz), co = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            if (s == 0.0 && co == 0.0) continue;
            const f64 angle = std::atan2(s, co);
            f64* dst = &acc[usize(c[i].v) * 3];
            dst[0] += nx * angle;
            dst[1] += ny * angle;
            dst[2] += nz * angle;
        }
    }
    out.resize(nv * 3, 0.0f);
    for (usize v = 0; v < nv; ++v) {
        const f64 len = std::sqrt(acc[v * 3] * acc[v * 3] + acc[v * 3 + 1] * acc[v * 3 + 1] + acc[v * 3 + 2] * acc[v * 3 + 2]);
        if (len > 0.0)
            for (usize k = 0; k < 3; ++k) out[v * 3 + k] = static_cast<f32>(acc[v * 3 + k] / len);
    }
}

// The materials the model names, loaded from its mtllib statements.
struct MtlSet {
    Array<MtlFile> files;
    NameTable names;            // every material name, first definition wins
    Array<u32> name_file;       // per name: index into files
    Array<u32> name_material;   // per name: index into that file's mats
    bool any_missing = false;   // an mtllib could not be loaded: unknown usemtl names are not reported again
};

// Loads the .mtl files named by `libs` through the registry. A file that is not there is a warning (when `report`); one
// that is there but unreadable is an error and false.
bool load_mtls(const ImportInput& in, const Array<LibRef>& libs, MtlSet& set, DiagList& diags, bool report) {
    Reporter obj(in.display_path, kObjStage, diags);
    auto load_one = [&](StringView name) -> int {  // 1 loaded, 0 missing, -1 error
        String rel;
        Guid g;
        Span<const byte> bytes;
        StringView path;
        if (!resolve_rel(in.source_path, name, rel) || !in.lookup || !in.lookup->guid_for_path(rel.view(), g) ||
            !in.lookup->source_bytes(g, bytes, path))
            return 0;
        for (const MtlFile& f : set.files)
            if (f.guid == g) return 1;  // the same file named twice
        MtlFile f;
        f.display = display_of(path);
        f.rel.assign(path);
        f.guid = g;
        DiagList scratch;
        DiagList& target = report ? diags : scratch;
        Reporter mrep(f.display.view(), kMtlStage, target);
        MtlParser parser(mrep, f);
        if (!parser.run(StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) return -1;
        const auto fi = static_cast<u32>(set.files.size());
        for (u32 m = 0; m < f.mats.size(); ++m) {
            if (set.names.find(f.mats[m].name.view()) >= 0) continue;
            if (set.names.size() >= kMaxMaterials) break;
            set.names.add(f.mats[m].name.view());
            set.name_file.push_back(fi);
            set.name_material.push_back(m);
        }
        set.files.push_back(std::move(f));
        return 1;
    };
    for (const LibRef& lib : libs) {
        int r = load_one(lib.name.view());
        if (r == 0 && lib.name.view().find_first_of(" \t") != StringView::npos) {
            // "mtllib a.mtl b.mtl": several files on one line (a file name with a space is tried whole first).
            StringView rest = lib.name.view();
            for (StringView t = next_token(rest); !t.empty(); t = next_token(rest)) {
                const int one = load_one(t);
                if (one < 0) return false;
                if (one == 0) {
                    set.any_missing = true;
                    if (report) {
                        String m("mtllib ");
                        quote(m, t);
                        m += " is not a registered asset (add the .mtl next to the model and adopt it): default materials are used";
                        obj.warn(lib.line, m.view());
                    }
                }
            }
            continue;
        }
        if (r < 0) return false;
        if (r == 0) {
            set.any_missing = true;
            if (report) {
                String m("mtllib ");
                quote(m, lib.name.view());
                m += " is not a registered asset (add the .mtl next to the model and adopt it): default materials are used";
                obj.warn(lib.line, m.view());
            }
        }
    }
    return true;
}

// Only the mtllib statements (scan_dependencies), without the cost of a full parse.
void scan_libs(StringView text, Array<LibRef>& libs) {
    if (text.size() > kMaxFileBytes) return;
    LineReader lr(text);
    for (;;) {
        StringView line;
        u32 no = 0;
        const Next r = lr.next(line, no);
        if (r == Next::End || r == Next::TooLong || r == Next::Nul) return;
        StringView rest = line;
        if (next_token(rest) != "mtllib") continue;
        const StringView name = trim(rest);
        if (name.empty() || name.size() > kMaxNameBytes || libs.size() >= kMaxMtlLibs) continue;
        LibRef lib;
        lib.name.assign(name);
        lib.line = no;
        libs.push_back(std::move(lib));
    }
}

void add_dep(Array<DepRef>& out, Guid g, ChunkType type, u32 line, StringView where) {
    for (const DepRef& e : out)
        if (e.guid == g) return;
    DepRef d;
    d.guid = g;
    d.expected = type;
    d.line = line;
    d.col = 1;
    d.where.assign(where);
    out.push_back(d);
}

void settings_error(DiagList& diags, StringView file, const char* stage, StringView msg) {
    diags.error(Errc::ParseError, file, 0, 0, stage, msg);
}

class ObjImporter final : public IImporter {
public:
    StringView id() const override { return "obj"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Mesh; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".obj"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList&) const override {
        Array<LibRef> libs;
        scan_libs(StringView(reinterpret_cast<const char*>(in.source.data()), in.source.size()), libs);
        if (libs.empty()) return;
        DiagList scratch;
        MtlSet set;
        if (!load_mtls(in, libs, set, scratch, false)) return;
        for (const MtlFile& f : set.files) add_dep(out, f.guid, ChunkType::Material, 1, f.display.view());
        for (const MtlFile& f : set.files) texture_deps(in.lookup, f, out);
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& writer, DiagList& diags) const override {
        const StringView file = in.display_path;
        if (!detail::check_keys(in.settings, {"scale", "max_triangles", "lod_levels", "lod_ratio", "lod_error", "up"}, file, kObjStage, diags))
            return false;
        f64 scale = 1.0, max_triangles = 0.0, lod_levels = 0.0, lod_ratio = 0.35, lod_error = 0.05;
        if (!detail::read_number(in.settings, "lod_levels", 0.0, lod_levels, file, kObjStage, diags)) return false;
        if (!detail::read_number(in.settings, "lod_ratio", 0.35, lod_ratio, file, kObjStage, diags)) return false;
        if (!detail::read_number(in.settings, "lod_error", 0.05, lod_error, file, kObjStage, diags)) return false;
        if (!detail::read_number(in.settings, "scale", 1.0, scale, file, kObjStage, diags)) return false;
        if (!detail::read_number(in.settings, "max_triangles", 0.0, max_triangles, file, kObjStage, diags)) return false;
        if (lod_levels < 0.0 || lod_levels >= asset::kMaxMeshLods || lod_levels != std::floor(lod_levels)) {
            settings_error(diags, file, kObjStage, "'lod_levels' must be a whole number 0..3");
            return false;
        }
        if (!(lod_ratio > 0.0 && lod_ratio < 1.0) || !(lod_error > 0.0 && lod_error <= 1.0)) {
            settings_error(diags, file, kObjStage, "'lod_ratio' must be in (0, 1) and 'lod_error' in (0, 1]");
            return false;
        }
        if (!(scale > 0.0) || !std::isfinite(scale)) {
            settings_error(diags, file, kObjStage, "'scale' must be positive");
            return false;
        }
        if (max_triangles < 0.0 || max_triangles != std::floor(max_triangles)) {
            settings_error(diags, file, kObjStage, "'max_triangles' must be a whole number >= 0 (0 = no budget)");
            return false;
        }
        bool up_z = false;
        if (const JsonValue up = in.settings.get("up"); up.valid()) {
            if (!up.is_string() || (up.as_string() != "y" && up.as_string() != "z")) {
                detail::error_at(diags, Errc::ParseError, file, up, kObjStage, "'up' must be \"y\" or \"z\"");
                return false;
            }
            up_z = up.as_string() == "z";
        }

        Reporter rep(file, kObjStage, diags);
        ObjData d;
        {
            ObjParser parser(rep, up_z, d);
            if (!parser.run(StringView(reinterpret_cast<const char*>(in.source.data()), in.source.size()))) return false;
        }
        emit(rep, d.w_pl, "points and lines (l, p) are not imported");
        emit(rep, d.w_curve, "free-form curves and surfaces are not imported");
        emit(rep, d.w_smooth, "smoothing groups (s) are ignored: normals come from vn, else they are smooth per position");
        emit(rep, d.w_other, "other attribute statements are ignored");
        emit(rep, d.w_color, "vertex colors after v x y z are not imported");

        // Materials.
        MtlSet mtls;
        if (!load_mtls(in, d.libs, mtls, diags, true)) return false;
        Array<asset::MaterialChunk> materials;
        materials.resize(d.mats.size(), default_material());
        bool materials_ok = true;
        for (u32 i = 0; i < d.mats.size(); ++i) {
            const i32 at = mtls.names.find(d.mats.name(i));
            if (at < 0) {
                if (!mtls.any_missing) {
                    String m("material ");
                    quote(m, d.mats.name(i));
                    m += " is not defined by any mtllib: the default material is used";
                    rep.warn(d.mat_line[i], m.view());
                }
                continue;
            }
            const MtlFile& f = mtls.files[mtls.name_file[static_cast<u32>(at)]];
            Reporter mrep(f.display.view(), kMtlStage, diags);
            materials_ok = build_material(in.lookup, f, f.mats[mtls.name_material[static_cast<u32>(at)]], mrep, materials[i]) && materials_ok;
        }
        if (!materials_ok) return false;

        // Geometry: one submesh per (node, material) group.
        Array<f32> smooth;
        if (d.need_normals) smooth_normals(d, smooth);
        Geometry geo;
        {
            Array<u32> start;
            start.resize(d.groups.size() + 1, 0u);
            for (const Face& f : d.faces) ++start[f.group + 1];
            for (usize g = 0; g < d.groups.size(); ++g) start[g + 1] += start[g];
            Array<u32> fill;
            fill.resize(d.groups.size(), 0u);
            Array<u32> order;
            order.resize(d.faces.size(), 0u);
            for (u32 f = 0; f < d.faces.size(); ++f) order[start[d.faces[f].group] + fill[d.faces[f].group]++] = f;

            for (u32 g = 0; g < d.groups.size(); ++g) {
                HashMap<Corner, u32, CornerHash> weld;
                Array<f32> p, n, uv;
                Array<u32> idx;
                auto vertex = [&](const Corner& c) {
                    auto [slot, inserted] = weld.try_emplace(c, static_cast<u32>(p.size() / 3));
                    if (inserted) {
                        for (usize k = 0; k < 3; ++k) p.push_back(d.pos[usize(c.v) * 3 + k]);
                        for (usize k = 0; k < 3; ++k) n.push_back(c.vn != kNone ? d.nrm[usize(c.vn) * 3 + k] : smooth[usize(c.v) * 3 + k]);
                        if (c.vt != kNone) {
                            uv.push_back(d.uv[usize(c.vt) * 2]);
                            uv.push_back(1.0f - d.uv[usize(c.vt) * 2 + 1]);
                        } else {
                            uv.push_back(0.0f);
                            uv.push_back(0.0f);
                        }
                    }
                    return *slot;
                };
                for (u32 k = start[g]; k < start[g + 1]; ++k) {
                    const Face& f = d.faces[order[k]];
                    const Corner* c = &d.corners[f.first];
                    for (u32 i = 1; i + 1 < f.count; ++i) {
                        const u32 a = vertex(c[0]);
                        const u32 b = vertex(c[i]);
                        const u32 e = vertex(c[i + 1]);
                        idx.push_back(a);
                        idx.push_back(b);
                        idx.push_back(e);
                    }
                }
                detail::Primitive prim{};
                prim.material_ext = asset::MaterialExt{};  // no extension lobes: OBJ has none
                prim.positions = p.span();
                prim.normals = n.span();
                prim.uvs = uv.span();
                prim.indices = idx.span();
                prim.material = d.groups[g].mat == kNone ? default_material() : materials[d.groups[g].mat];
                prim.node = d.groups[g].node;
                detail::add_submesh(prim, geo);
            }
        }

        if (scale != 1.0)
            for (asset::MeshNode& mn : d.nodes)
                for (int i = 0; i < 3; ++i) {
                    mn.translation[i] *= static_cast<f32>(scale);
                    mn.scale[i] *= static_cast<f32>(scale);
                }
        if (lod_levels > 0.0) detail::generate_lods(geo, static_cast<u32>(lod_levels), static_cast<f32>(lod_ratio), static_cast<f32>(lod_error));
        if (geo.submeshes.empty()) rep.warn(0, "the file has no faces");
        usize triangles = 0;  // the base levels: LODs are cheaper by construction
        for (const asset::MeshSubmesh& sm : geo.submeshes) triangles += sm.index_count / 3;
        if (max_triangles > 0.0 && static_cast<f64>(triangles) > max_triangles) {
            String msg;
            appendf(msg, "%zu triangles, over the mesh's budget of %.0f (sidecar 'max_triangles')", triangles, max_triangles);
            diags.error(Errc::InvalidArgument, file, 0, 0, kObjStage, msg.view());
            return false;
        }

        detail::SkinTables skins;
        skins.submesh_skins.resize(geo.submeshes.size(), -1);  // all rigid
        detail::MeshTables tables;
        tables.nodes = d.nodes.span();
        tables.strings = d.strings.span();
        detail::write_mesh_chunk(geo, skins, tables, false, in.guid, writer);
        return true;
    }
};

class MtlImporter final : public IImporter {
public:
    StringView id() const override { return "mtl"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Material; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".mtl"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList&) const override {
        DiagList scratch;
        MtlFile f;
        f.display.assign(in.display_path);
        f.rel.assign(in.source_path);
        Reporter rep(in.display_path, kMtlStage, scratch);
        MtlParser parser(rep, f);
        if (!parser.run(StringView(reinterpret_cast<const char*>(in.source.data()), in.source.size()))) return;
        texture_deps(in.lookup, f, out);
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& writer, DiagList& diags) const override {
        if (!detail::check_keys(in.settings, {}, in.display_path, kMtlStage, diags)) return false;
        MtlFile f;
        f.display.assign(in.display_path);
        f.rel.assign(in.source_path);
        f.guid = in.guid;
        Reporter rep(in.display_path, kMtlStage, diags);
        MtlParser parser(rep, f);
        if (!parser.run(StringView(reinterpret_cast<const char*>(in.source.data()), in.source.size()))) return false;
        asset::MaterialChunk first = default_material();
        bool ok = true;
        for (u32 i = 0; i < f.mats.size(); ++i) {
            asset::MaterialChunk m;
            ok = build_material(in.lookup, f, f.mats[i], rep, m) && ok;
            if (i == 0) first = m;
        }
        if (!ok) return false;
        if (f.mats.empty()) rep.warn(0, "the file defines no material (newmtl): a default material is cooked");
        else if (f.mats.size() > 1) rep.warn(f.mats[1].line, "only the first material is cooked as this asset; a model's .obj reads all of them");
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Material;
        d.guid = in.guid;
        d.version = asset::chunk_version(asset::MATERIAL_VERSION, 0);
        // user_flags is unused by materials: it fingerprints the whole file, so a change to any material (not only the first,
        // which is all the payload holds) changes this asset's output and re-cooks the models that read the file.
        const u64 h = asset::xxh64(in.source.data(), in.source.size());
        d.user_flags = static_cast<u32>(h ^ (h >> 32));
        writer.add(d, asset::as_bytes_of(first));
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_obj_importer() { return mem::make_unique<ObjImporter>(mem::heap()); }
mem::UniquePtr<IImporter> make_mtl_importer() { return mem::make_unique<MtlImporter>(mem::heap()); }

}  // namespace mce::cook
