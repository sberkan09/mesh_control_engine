#pragma once
// FieldTraits<T>: maps a C++ member type to its FieldKind (core.md §2.8).
//
// Built in: bool, fixed-width integers, f32/f64, FixedString<N>, math vec2/vec3/vec4/quat/Color,
// enums, and structs reflected with MCE_REFL_STRUCT / MCE_COMPONENT. Fixed arrays `T x[N]`
// are described by the element's traits with FieldRecord::count = N.
//
// Higher layers declare their handle types by specializing FieldTraits in namespace mce::refl,
// e.g. mce_ecs:
//
//   template <> struct mce::refl::FieldTraits<mce::Entity> {
//       static constexpr FieldKind kind = FieldKind::EntityRef;
//       static constexpr const char* type_name = "Entity";
//   };

#include <concepts>
#include <type_traits>

#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/type.h"

namespace mce::refl {

// Primary template: not reflectable. make_field static_asserts on `supported`.
template <class T, class = void>
struct FieldTraits {
    static constexpr bool supported = false;
};

namespace detail {
template <FieldKind K>
struct KindTraits {
    static constexpr bool supported = true;
    static constexpr FieldKind kind = K;
    static constexpr const char* type_name = nullptr;
};
}  // namespace detail

// ADL hooks emitted by the macros (found in the reflected type's own namespace):
//   constexpr const char* mce_refl_type_name(T*);          MCE_REFL_STRUCT / MCE_COMPONENT
//   const TypeRecord&     mce_refl_type_record(T*);
//   constexpr const char* mce_refl_enum_name(E*);          MCE_ENUM
//   const EnumRecord&     mce_refl_enum_record(E*);
template <class T>
concept ReflectedStruct = requires {
    { mce_refl_type_name(static_cast<T*>(nullptr)) } -> std::convertible_to<const char*>;
};

template <class E>
concept ReflectedEnum = std::is_enum_v<E> && requires {
    { mce_refl_enum_name(static_cast<E*>(nullptr)) } -> std::convertible_to<const char*>;
};

// clang-format off
template <> struct FieldTraits<bool> : detail::KindTraits<FieldKind::Bool> {};
template <> struct FieldTraits<i8>   : detail::KindTraits<FieldKind::I8> {};
template <> struct FieldTraits<i16>  : detail::KindTraits<FieldKind::I16> {};
template <> struct FieldTraits<i32>  : detail::KindTraits<FieldKind::I32> {};
template <> struct FieldTraits<i64>  : detail::KindTraits<FieldKind::I64> {};
template <> struct FieldTraits<u8>   : detail::KindTraits<FieldKind::U8> {};
template <> struct FieldTraits<u16>  : detail::KindTraits<FieldKind::U16> {};
template <> struct FieldTraits<u32>  : detail::KindTraits<FieldKind::U32> {};
template <> struct FieldTraits<u64>  : detail::KindTraits<FieldKind::U64> {};
template <> struct FieldTraits<f32>  : detail::KindTraits<FieldKind::F32> {};
template <> struct FieldTraits<f64>  : detail::KindTraits<FieldKind::F64> {};
template <> struct FieldTraits<math::vec2>  : detail::KindTraits<FieldKind::Vec2> {};
template <> struct FieldTraits<math::vec3>  : detail::KindTraits<FieldKind::Vec3> {};
template <> struct FieldTraits<math::vec4>  : detail::KindTraits<FieldKind::Vec4> {};
template <> struct FieldTraits<math::quat>  : detail::KindTraits<FieldKind::Quat> {};
template <> struct FieldTraits<math::Color> : detail::KindTraits<FieldKind::Color> {};
// clang-format on

template <usize N>
struct FieldTraits<FixedString<N>> {
    static constexpr bool supported = true;
    static constexpr FieldKind kind = FieldKind::String;
    static constexpr const char* type_name = nullptr;
    static constexpr StringOps string_ops{
        [](const void* f) { return static_cast<const FixedString<N>*>(f)->view(); },
        [](void* f, StringView v) { static_cast<FixedString<N>*>(f)->assign(v); },
        static_cast<u32>(FixedString<N>::capacity()),
    };
};

template <class E>
struct FieldTraits<E, std::enable_if_t<std::is_enum_v<E>>> {
    static constexpr bool supported = true;
    static constexpr FieldKind kind = FieldKind::Enum;
    static constexpr const char* type_name = [] {
        if constexpr (ReflectedEnum<E>) return mce_refl_enum_name(static_cast<E*>(nullptr));
        else return static_cast<const char*>(nullptr);  // un-reflected enum: shown as its integer
    }();
};

template <class T>
struct FieldTraits<T, std::enable_if_t<ReflectedStruct<T>>> {
    static constexpr bool supported = true;
    static constexpr FieldKind kind = FieldKind::Struct;
    static constexpr const char* type_name = mce_refl_type_name(static_cast<T*>(nullptr));
};

// ---------------------------------------------------------------- typed access helpers

template <class T>
const TypeRecord& type_of() {
    static_assert(ReflectedStruct<T>, "type_of<T>: T is not reflected (MCE_COMPONENT / MCE_REFL_STRUCT)");
    return mce_refl_type_record(static_cast<T*>(nullptr));
}

template <class T>
constexpr StringId type_id() {
    static_assert(ReflectedStruct<T>, "type_id<T>: T is not reflected (MCE_COMPONENT / MCE_REFL_STRUCT)");
    return StringId(mce_refl_type_name(static_cast<T*>(nullptr)));
}

template <class E>
const EnumRecord& enum_of() {
    static_assert(ReflectedEnum<E>, "enum_of<E>: E is not reflected (MCE_ENUM)");
    return mce_refl_enum_record(static_cast<E*>(nullptr));
}

// True when values of C++ type V may be read/written through a field of this kind/type/size.
template <class V>
constexpr bool kind_matches(FieldKind kind, StringId type_id, u32 size) {
    using Traits = FieldTraits<std::remove_cv_t<V>>;
    if constexpr (!Traits::supported) {
        return false;
    } else {
        if (Traits::kind != kind || sizeof(V) != size) return false;
        if constexpr (Traits::type_name != nullptr) return StringId(Traits::type_name) == type_id;
        else return !type_id.valid();
    }
}

}  // namespace mce::refl
