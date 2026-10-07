#include "mce/refl/property_path.h"

namespace mce::refl {
namespace {

constexpr const char* kStage = "refl.path";

bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Lane index for vector-like kinds, or -1 when `name` is not a lane of `kind`.
i32 lane_index(FieldKind kind, StringView name) {
    if (name.size() != 1) return -1;
    const char c = name[0];
    u32 lanes = 0;
    const char* names = "xyzw";
    switch (kind) {
        case FieldKind::Vec2: lanes = 2; break;
        case FieldKind::Vec3: lanes = 3; break;
        case FieldKind::Vec4:
        case FieldKind::Quat: lanes = 4; break;
        case FieldKind::Color:
            lanes = 4;
            names = "rgba";
            break;
        default: return -1;
    }
    for (u32 i = 0; i < lanes; ++i) {
        if (names[i] == c) return static_cast<i32>(i);
    }
    return -1;
}

Error path_error(Errc code, StringView path, usize at) { return Error(code, kStage, path, static_cast<u32>(at)); }

}  // namespace

Result<PropertyRef> resolve_path(const TypeRecord& root, StringView path) {
    PropertyRef ref;
    const TypeRecord* type = &root;  // non-null while the current position is a struct
    usize pos = 0;
    bool first = true;

    while (true) {
        // ---- segment name
        if (!first) {
            if (pos >= path.size() || path[pos] != '.') return path_error(Errc::InvalidArgument, path, pos);
            ++pos;
        }
        const usize start = pos;
        while (pos < path.size() && is_ident_char(path[pos])) ++pos;
        if (pos == start) return path_error(Errc::InvalidArgument, path, pos);
        const StringView name = path.substr(start, pos - start);

        if (type != nullptr) {
            const FieldRecord* field = type->find_field(StringId(name));
            if (!field) return path_error(Errc::NotFound, path, start);
            ref.field = field;
            ref.kind = field->kind;
            ref.offset += field->offset;
            ref.size = field->size;
            ref.count = field->count;
            ref.type_id = field->type_id;
        } else {
            // A lane of the current vector-like leaf.
            const i32 lane = ref.count == 1 ? lane_index(ref.kind, name) : -1;
            if (lane < 0) return path_error(Errc::InvalidArgument, path, start);
            ref.kind = FieldKind::F32;
            ref.offset += static_cast<u32>(lane) * static_cast<u32>(sizeof(f32));
            ref.size = sizeof(f32);
            ref.type_id = StringId();
        }
        first = false;

        // ---- optional [index] on an array field
        if (pos < path.size() && path[pos] == '[') {
            if (ref.count <= 1) return path_error(Errc::InvalidArgument, path, pos);
            ++pos;
            u64 index = 0;
            const usize digits = pos;
            while (pos < path.size() && path[pos] >= '0' && path[pos] <= '9') {
                index = index * 10 + static_cast<u64>(path[pos] - '0');
                if (index >= ref.count) return path_error(Errc::InvalidArgument, path, digits);
                ++pos;
            }
            if (pos == digits || pos >= path.size() || path[pos] != ']') {
                return path_error(Errc::InvalidArgument, path, pos);
            }
            ++pos;
            ref.offset += static_cast<u32>(index) * ref.size;
            ref.count = 1;
        }

        if (pos == path.size()) return ref;

        // ---- descend
        type = nullptr;
        if (ref.kind == FieldKind::Struct) {
            if (ref.count != 1) return path_error(Errc::InvalidArgument, path, pos);  // needs [index]
            type = find_type(ref.type_id);
            if (!type) return path_error(Errc::NotFound, path, pos);
        }
    }
}

namespace {
bool is_string_leaf(const PropertyRef& ref) {
    return ref.valid() && ref.kind == FieldKind::String && ref.count == 1 && ref.field->string_ops != nullptr;
}
}  // namespace

StringView get_string(const void* object, const PropertyRef& ref) {
    const bool ok = is_string_leaf(ref);
    MCE_ASSERT_MSG(ok, "property is not a string");
    if (!ok) return {};
    return ref.field->string_ops->get(static_cast<const byte*>(object) + ref.offset);
}

void set_string(void* object, const PropertyRef& ref, StringView value) {
    const bool ok = is_string_leaf(ref);
    MCE_ASSERT_MSG(ok, "property is not a string");
    if (!ok) return;
    ref.field->string_ops->set(static_cast<byte*>(object) + ref.offset, value);
}

}  // namespace mce::refl
