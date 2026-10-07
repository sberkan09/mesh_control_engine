#include "mce/cook/json.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace mce::cook {

// ------------------------------------------------------------------ JsonValue


JsonType JsonValue::type() const { return doc_->nodes_[index_].type; }

bool JsonValue::as_bool() const {
    MCE_ASSERT_MSG(is_bool(), "JsonValue::as_bool on non-bool");
    return doc_->nodes_[index_].b;
}

f64 JsonValue::as_number() const {
    MCE_ASSERT_MSG(is_number(), "JsonValue::as_number on non-number");
    return doc_->nodes_[index_].num;
}

bool JsonValue::is_integer() const {
    if (!is_number()) return false;
    const f64 v = doc_->nodes_[index_].num;
    return std::floor(v) == v && std::fabs(v) <= 9007199254740992.0;
}

StringView JsonValue::as_string() const {
    MCE_ASSERT_MSG(is_string(), "JsonValue::as_string on non-string");
    const auto& n = doc_->nodes_[index_];
    return StringView(doc_->strings_.data() + n.str_off, n.str_len);
}

u32 JsonValue::size() const {
    const auto& n = doc_->nodes_[index_];
    return (n.type == JsonType::Array || n.type == JsonType::Object) ? n.count : 0;
}

JsonValue JsonValue::at(u32 i) const {
    const auto& n = doc_->nodes_[index_];
    MCE_ASSERT_MSG(i < size(), "JsonValue::at out of range");
    if (n.type == JsonType::Array) return JsonValue(doc_, doc_->kids_[n.first + i]);
    return JsonValue(doc_, doc_->kids_[n.first + 2 * i + 1]);
}

StringView JsonValue::key_at(u32 i) const {
    const auto& n = doc_->nodes_[index_];
    MCE_ASSERT_MSG(n.type == JsonType::Object && i < n.count, "JsonValue::key_at on non-object");
    return JsonValue(doc_, doc_->kids_[n.first + 2 * i]).as_string();
}

JsonValue JsonValue::get(StringView key) const {
    if (!is_object()) return {};
    for (u32 i = 0; i < size(); ++i)
        if (key_at(i) == key) return at(i);
    return {};
}

u32 JsonValue::line() const { return doc_->nodes_[index_].line; }
u32 JsonValue::col() const { return doc_->nodes_[index_].col; }

// ------------------------------------------------------------------ parser

class JsonParser {
public:
    JsonParser(JsonDoc& doc, StringView text, JsonError& err) : doc_(doc), s_(text), err_(err) {}

    bool run() {
        doc_.nodes_.clear();
        doc_.kids_.clear();
        doc_.strings_.clear();
        doc_.nodes_.push_back({});  // root slot: index 0
        skip_ws();
        if (failed_) return false;
        if (!value(0, 0)) return false;
        skip_ws();
        if (failed_) return false;
        if (pos_ != s_.size()) return fail("trailing characters after JSON value");
        return true;
    }

private:
    static constexpr u32 kMaxDepth = 128;

    bool fail(const char* msg) {
        if (!failed_) {
            failed_ = true;
            err_.line = line_;
            err_.col = col_;
            err_.message.assign(msg);
        }
        return false;
    }

    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    void advance() {
        if (s_[pos_] == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        ++pos_;
    }

    void skip_ws() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                advance();
            } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
                while (pos_ < s_.size() && s_[pos_] != '\n') advance();
            } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '*') {
                advance();
                advance();
                while (pos_ + 1 < s_.size() && !(s_[pos_] == '*' && s_[pos_ + 1] == '/')) advance();
                if (pos_ + 1 >= s_.size()) {
                    fail("unterminated comment");
                    return;
                }
                advance();
                advance();
            } else {
                break;
            }
        }
    }

    bool literal(StringView word) {
        if (s_.substr(pos_, word.size()) != word) return fail("invalid literal");
        for (usize i = 0; i < word.size(); ++i) advance();
        return true;
    }

    // Parses a value into node `slot`.
    bool value(u32 slot, u32 depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        JsonDoc::Node n;
        n.line = line_;
        n.col = col_;
        const char c = peek();
        if (c == '{') {
            n.type = JsonType::Object;
            doc_.nodes_[slot] = n;
            return object(slot, depth);
        }
        if (c == '[') {
            n.type = JsonType::Array;
            doc_.nodes_[slot] = n;
            return array(slot, depth);
        }
        if (c == '"') {
            n.type = JsonType::String;
            if (!string(n.str_off, n.str_len)) return false;
        } else if (c == 't') {
            n.type = JsonType::Bool;
            n.b = true;
            if (!literal("true")) return false;
        } else if (c == 'f') {
            n.type = JsonType::Bool;
            if (!literal("false")) return false;
        } else if (c == 'n') {
            if (!literal("null")) return false;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            n.type = JsonType::Number;
            if (!number(n.num)) return false;
        } else {
            return fail(c == '\0' ? "unexpected end of input" : "unexpected character");
        }
        doc_.nodes_[slot] = n;
        return true;
    }

    bool number(f64& out) {
        const usize start = pos_;
        if (peek() == '-') advance();
        if (peek() == '0') {
            advance();
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') advance();
        } else {
            return fail("invalid number");
        }
        if (peek() == '.') {
            advance();
            if (!(peek() >= '0' && peek() <= '9')) return fail("invalid number");
            while (peek() >= '0' && peek() <= '9') advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            advance();
            if (peek() == '+' || peek() == '-') advance();
            if (!(peek() >= '0' && peek() <= '9')) return fail("invalid number");
            while (peek() >= '0' && peek() <= '9') advance();
        }
        const char* b = s_.data() + start;
        const char* e = s_.data() + pos_;
        auto r = std::from_chars(b, e, out);
        if (r.ec != std::errc() || r.ptr != e) return fail("number out of range");
        return true;
    }

    static void put_utf8(Array<char>& s, u32 cp) {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(u32& out) {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = peek();
            u32 v;
            if (c >= '0' && c <= '9') v = static_cast<u32>(c - '0');
            else if (c >= 'a' && c <= 'f') v = static_cast<u32>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = static_cast<u32>(c - 'A' + 10);
            else return fail("invalid \\u escape");
            out = (out << 4) | v;
            advance();
        }
        return true;
    }

    bool string(u32& off, u32& len) {
        advance();  // opening quote
        Array<char>& s = doc_.strings_;
        off = static_cast<u32>(s.size());
        for (;;) {
            if (pos_ >= s_.size()) return fail("unterminated string");
            const char c = s_[pos_];
            if (c == '"') {
                advance();
                break;
            }
            if (static_cast<u8>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                s.push_back(c);
                advance();
                continue;
            }
            advance();
            const char e = peek();
            if (e == '\0') return fail("unterminated string");
            advance();
            switch (e) {
                case '"': s.push_back('"'); break;
                case '\\': s.push_back('\\'); break;
                case '/': s.push_back('/'); break;
                case 'b': s.push_back('\b'); break;
                case 'f': s.push_back('\f'); break;
                case 'n': s.push_back('\n'); break;
                case 'r': s.push_back('\r'); break;
                case 't': s.push_back('\t'); break;
                case 'u': {
                    u32 cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (peek() != '\\') return fail("unpaired surrogate");
                        advance();
                        if (peek() != 'u') return fail("unpaired surrogate");
                        advance();
                        u32 lo;
                        if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("unpaired surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired surrogate");
                    }
                    put_utf8(s, cp);
                    break;
                }
                default: return fail("invalid escape");
            }
        }
        len = static_cast<u32>(s.size()) - off;
        return true;
    }

    u32 new_node() {
        doc_.nodes_.push_back({});
        return static_cast<u32>(doc_.nodes_.size() - 1);
    }

    bool array(u32 slot, u32 depth) {
        advance();  // [
        SmallVector<u32, 16> items;
        skip_ws();
        if (failed_) return false;
        if (peek() == ']') {
            advance();
        } else {
            for (;;) {
                const u32 child = new_node();
                if (!value(child, depth + 1)) return false;
                items.push_back(child);
                skip_ws();
                if (failed_) return false;
                if (peek() == ',') {
                    advance();
                    skip_ws();
                    if (failed_) return false;
                    continue;
                }
                if (peek() == ']') {
                    advance();
                    break;
                }
                return fail("expected ',' or ']'");
            }
        }
        JsonDoc::Node& n = doc_.nodes_[slot];
        n.first = static_cast<u32>(doc_.kids_.size());
        n.count = static_cast<u32>(items.size());
        for (u32 k : items) doc_.kids_.push_back(k);
        return true;
    }

    bool object(u32 slot, u32 depth) {
        advance();  // {
        SmallVector<u32, 32> items;  // key node, value node pairs
        skip_ws();
        if (failed_) return false;
        if (peek() == '}') {
            advance();
        } else {
            for (;;) {
                if (peek() != '"') return fail("expected object key string");
                const u32 key = new_node();
                JsonDoc::Node kn;
                kn.type = JsonType::String;
                kn.line = line_;
                kn.col = col_;
                if (!string(kn.str_off, kn.str_len)) return false;
                doc_.nodes_[key] = kn;
                const StringView kv(doc_.strings_.data() + kn.str_off, kn.str_len);
                for (usize i = 0; i < items.size(); i += 2) {
                    const JsonDoc::Node& other = doc_.nodes_[items[i]];
                    if (StringView(doc_.strings_.data() + other.str_off, other.str_len) == kv) {
                        line_ = kn.line;
                        col_ = kn.col;
                        return fail("duplicate object key");
                    }
                }
                skip_ws();
                if (failed_) return false;
                if (peek() != ':') return fail("expected ':'");
                advance();
                skip_ws();
                if (failed_) return false;
                const u32 val = new_node();
                if (!value(val, depth + 1)) return false;
                items.push_back(key);
                items.push_back(val);
                skip_ws();
                if (failed_) return false;
                if (peek() == ',') {
                    advance();
                    skip_ws();
                    if (failed_) return false;
                    continue;
                }
                if (peek() == '}') {
                    advance();
                    break;
                }
                return fail("expected ',' or '}'");
            }
        }
        JsonDoc::Node& n = doc_.nodes_[slot];
        n.first = static_cast<u32>(doc_.kids_.size());
        n.count = static_cast<u32>(items.size() / 2);
        for (u32 k : items) doc_.kids_.push_back(k);
        return true;
    }

    JsonDoc& doc_;
    StringView s_;
    JsonError& err_;
    usize pos_ = 0;
    u32 line_ = 1;
    u32 col_ = 1;
    bool failed_ = false;
};

bool JsonDoc::parse(StringView text, JsonError& err) {
    JsonParser p(*this, text, err);
    if (!p.run()) {
        nodes_.clear();
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ writer

void append_json_string(String& out, StringView s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<u8>(c) < 0x20) appendf(out, "\\u%04x", static_cast<unsigned>(static_cast<u8>(c)));
                else out += c;
        }
    }
    out += '"';
}

void append_json_number(String& out, f64 v) {
    if (v == 0.0) v = 0.0;  // fold -0 so canonical output is stable
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);  // shortest round-trip
    out.append(StringView(buf, static_cast<usize>(r.ptr - buf)));
}

namespace {
void indent(String& out, u32 depth) {
    out += '\n';
    for (u32 i = 0; i < depth; ++i) out += "  ";
}

void write_value(JsonValue v, String& out, bool canonical, u32 depth) {
    switch (v.type()) {
        case JsonType::Null: out += "null"; break;
        case JsonType::Bool: out += v.as_bool() ? "true" : "false"; break;
        case JsonType::Number: append_json_number(out, v.as_number()); break;
        case JsonType::String: append_json_string(out, v.as_string()); break;
        case JsonType::Array: {
            out += '[';
            for (u32 i = 0; i < v.size(); ++i) {
                if (i) out += canonical ? "," : ", ";
                write_value(v.at(i), out, canonical, depth + 1);
            }
            out += ']';
            break;
        }
        case JsonType::Object: {
            SmallVector<u32, 32> order;
            for (u32 i = 0; i < v.size(); ++i) order.push_back(i);
            if (canonical) std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return v.key_at(a) < v.key_at(b); });
            out += '{';
            for (u32 k = 0; k < order.size(); ++k) {
                if (k) out += ',';
                if (!canonical) indent(out, depth + 1);
                append_json_string(out, v.key_at(order[k]));
                out += canonical ? ":" : ": ";
                write_value(v.at(order[k]), out, canonical, depth + 1);
            }
            if (!canonical && v.size()) indent(out, depth);
            out += '}';
            break;
        }
    }
}
}  // namespace

void write_json(JsonValue v, String& out, bool canonical) {
    if (!v.valid()) return;
    write_value(v, out, canonical, 0);
}

}  // namespace mce::cook
