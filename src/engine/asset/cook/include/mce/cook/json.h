#pragma once
// JSON for MCE-native text assets and sidecars (cook/editor side only — the runtime never parses
// text, FR-AST-010).
//
// Hand-written rather than nlohmann/json (the assets.md §9.2 suggestion): every node carries its
// source line/column, which is what lets the cook report "file:line:col" for a missing reference
// or a bad field (FR-AST-040) — nlohmann only knows positions for syntax errors. It also avoids
// exceptions and STL strings in an engine target. `//` and `/* */` comments are accepted (the
// sidecar examples in assets.md are jsonc); duplicate keys are a parse error.

#include "mce/cook/string.h"
#include "mce/result.h"

namespace mce::cook {

enum class JsonType : u8 { Null, Bool, Number, String, Array, Object };

class JsonDoc;

class JsonValue {
public:
    JsonValue() = default;
    JsonValue(const JsonDoc* doc, u32 index) : doc_(doc), index_(index) {}

    bool valid() const { return doc_ != nullptr; }
    u32 index() const { return index_; }
    JsonType type() const;
    bool is_null() const { return valid() && type() == JsonType::Null; }
    bool is_bool() const { return valid() && type() == JsonType::Bool; }
    bool is_number() const { return valid() && type() == JsonType::Number; }
    bool is_string() const { return valid() && type() == JsonType::String; }
    bool is_array() const { return valid() && type() == JsonType::Array; }
    bool is_object() const { return valid() && type() == JsonType::Object; }

    bool as_bool() const;
    f64 as_number() const;
    // True when the number is integral and exactly representable (|v| <= 2^53).
    bool is_integer() const;
    StringView as_string() const;

    // Arrays and objects.
    u32 size() const;
    JsonValue at(u32 i) const;
    StringView key_at(u32 i) const;  // objects only
    // Object member, or an invalid value when absent.
    JsonValue get(StringView key) const;

    u32 line() const;
    u32 col() const;

private:
    const JsonDoc* doc_ = nullptr;
    u32 index_ = 0;
};

struct JsonError {
    u32 line = 0;
    u32 col = 0;
    FixedString<128> message;
};

class JsonDoc {
public:
    // Parses `text`; on failure returns false and fills `err`.
    bool parse(StringView text, JsonError& err);
    JsonValue root() const { return nodes_.empty() ? JsonValue() : JsonValue(this, 0); }

private:
    friend class JsonValue;
    friend class JsonParser;
    struct Node {
        JsonType type = JsonType::Null;
        bool b = false;
        u32 line = 0;
        u32 col = 0;
        f64 num = 0.0;
        u32 str_off = 0;  // String: value; members of objects: key via kids
        u32 str_len = 0;
        u32 first = 0;  // into kids_ (arrays: value indices; objects: pairs of key-string-node, value)
        u32 count = 0;
    };
    Array<Node> nodes_;
    Array<u32> kids_;
    Array<char> strings_;
};

// Writer. `canonical` = no whitespace, object keys sorted bytewise, numbers in shortest
// round-trip form: the canonical serialization the scene hash is taken over (assets.md §6).
// Otherwise two-space indented, keys in source order (sidecars, written by --adopt).
void write_json(JsonValue v, String& out, bool canonical);
void append_json_string(String& out, StringView s);
void append_json_number(String& out, f64 v);

}  // namespace mce::cook
