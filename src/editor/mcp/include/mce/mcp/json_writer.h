#pragma once
// JsonWriter — streaming JSON output for the MCP server (editor-mcp.md §3). Reading uses the cook's
// hand-written codec (mce/cook/json.h, ADR-0027); this is its small structured counterpart: commas
// and nesting are tracked here so tool code only says what to write. Editor-side only: no runtime
// target links mce_mcp_core.

#include "mce/cook/json.h"

namespace mce::mcp {

class JsonWriter {
public:
    static constexpr u32 kMaxDepth = 64;

    explicit JsonWriter(cook::String& out) : out_(out) {}

    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    // Object member name; the next call writes its value.
    JsonWriter& key(StringView k);

    JsonWriter& str(StringView s);
    JsonWriter& boolean(bool b);
    JsonWriter& num_i(i64 v);
    JsonWriter& num_u(u64 v);
    JsonWriter& num_f(f64 v);  // NaN/Inf write 0
    JsonWriter& null();
    // A value that is already JSON text (trusted: produced by the cook's writers).
    JsonWriter& raw(StringView json);
    // A parsed value, re-written compactly (canonical form).
    JsonWriter& copy(cook::JsonValue v);

    u32 depth() const { return depth_; }

private:
    void before_value();

    cook::String& out_;
    bool first_[kMaxDepth + 1] = {};
    u32 depth_ = 0;
    bool after_key_ = false;
};

}  // namespace mce::mcp
