#include "mce/mcp/json_writer.h"

#include <cmath>

#include "mce/assert.h"

namespace mce::mcp {

void JsonWriter::before_value() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (depth_ > 0) {
        if (!first_[depth_]) out_ += ',';
        first_[depth_] = false;
    }
}

JsonWriter& JsonWriter::begin_object() {
    before_value();
    MCE_ASSERT_MSG(depth_ < kMaxDepth, "JsonWriter: nesting too deep");
    out_ += '{';
    first_[++depth_] = true;
    return *this;
}

JsonWriter& JsonWriter::end_object() {
    MCE_ASSERT(depth_ > 0 && !after_key_);
    --depth_;
    out_ += '}';
    return *this;
}

JsonWriter& JsonWriter::begin_array() {
    before_value();
    MCE_ASSERT_MSG(depth_ < kMaxDepth, "JsonWriter: nesting too deep");
    out_ += '[';
    first_[++depth_] = true;
    return *this;
}

JsonWriter& JsonWriter::end_array() {
    MCE_ASSERT(depth_ > 0 && !after_key_);
    --depth_;
    out_ += ']';
    return *this;
}

JsonWriter& JsonWriter::key(StringView k) {
    before_value();
    cook::append_json_string(out_, k);
    out_ += ':';
    after_key_ = true;
    return *this;
}

JsonWriter& JsonWriter::str(StringView s) {
    before_value();
    cook::append_json_string(out_, s);
    return *this;
}

JsonWriter& JsonWriter::boolean(bool b) {
    before_value();
    out_ += b ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::num_i(i64 v) {
    before_value();
    cook::appendf(out_, "%lld", static_cast<long long>(v));
    return *this;
}

JsonWriter& JsonWriter::num_u(u64 v) {
    before_value();
    cook::appendf(out_, "%llu", static_cast<unsigned long long>(v));
    return *this;
}

JsonWriter& JsonWriter::num_f(f64 v) {
    before_value();
    cook::append_json_number(out_, std::isfinite(v) ? v : 0.0);
    return *this;
}

JsonWriter& JsonWriter::null() {
    before_value();
    out_ += "null";
    return *this;
}

JsonWriter& JsonWriter::raw(StringView json) {
    before_value();
    out_ += json;
    return *this;
}

JsonWriter& JsonWriter::copy(cook::JsonValue v) {
    before_value();
    if (v.valid()) cook::write_json(v, out_, true);
    else out_ += "null";
    return *this;
}

}  // namespace mce::mcp
