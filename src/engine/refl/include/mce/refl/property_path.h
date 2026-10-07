#pragma once
// Property paths (core.md §2.8): the one addressing scheme shared by the inspector, undo diffs,
// prefab overrides, saves and mce::ui binding.
//
//   "hp"                     field of the root type
//   "body.velocity"          nested reflected struct
//   "body.velocity.x"        vector / quat / color lane (x y z w, or r g b a for Color)
//   "waypoints[2].y"         fixed-array element, then a lane
//
// A path resolves against a TypeRecord to a PropertyRef: a byte offset from the start of the
// root object plus the leaf's kind/size. Resolution walks the registry once; the ref can be
// cached and applied to any instance of the root type.

#include "mce/refl/field_traits.h"
#include "mce/result.h"

namespace mce::refl {

struct PropertyRef {
    const FieldRecord* field = nullptr;  // innermost declared field (flags, help, editor range)
    FieldKind kind = FieldKind::Bool;    // leaf kind (F32 for a vector lane)
    u32 offset = 0;                      // from the start of the root object
    u32 size = 0;                        // byte size of one leaf element
    u32 count = 1;                       // > 1 when the path names a whole fixed array
    StringId type_id;                    // leaf Struct/Enum/ref type, if any

    bool valid() const { return field != nullptr; }
};

// Errors: Errc::InvalidArgument (syntax, index out of range, lane on a non-vector),
// Errc::NotFound (unknown field or unregistered nested type). Error::path carries the path.
Result<PropertyRef> resolve_path(const TypeRecord& root, StringView path);

// Typed access. Asserts (programmer error) when V does not match the leaf's kind/type/size.
template <class V>
V* property_ptr(void* object, const PropertyRef& ref) {
    MCE_ASSERT_MSG(ref.valid() && ref.count == 1 && kind_matches<V>(ref.kind, ref.type_id, ref.size),
                   "property type mismatch");
    return reinterpret_cast<V*>(static_cast<byte*>(object) + ref.offset);
}

template <class V>
const V* property_ptr(const void* object, const PropertyRef& ref) {
    return property_ptr<V>(const_cast<void*>(object), ref);
}

template <class V>
V get_property(const void* object, const PropertyRef& ref) {
    return *property_ptr<V>(object, ref);
}

template <class V>
void set_property(void* object, const PropertyRef& ref, const V& value) {
    *property_ptr<V>(object, ref) = value;
}

// String leaves go through the field type's StringOps.
StringView get_string(const void* object, const PropertyRef& ref);
void set_string(void* object, const PropertyRef& ref, StringView value);

}  // namespace mce::refl
