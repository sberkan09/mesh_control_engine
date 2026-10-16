#pragma once
// Registration macros (core.md §2.8, ecs-gameplay.md §2.1, FR-DX-020).
//
// Declare a plain struct, then reflect it at namespace scope in the struct's own namespace:
//
//   struct Health { f32 hp = 100; f32 max = 100; u8 flags; };
//   MCE_COMPONENT(Health,
//       (hp, Range{0, 1000}, Save, Help{"current hit points"}),
//       max,
//       (flags, ReadOnly))
//
// The macros are complete declarations (no trailing semicolon needed).
// Each field is either a bare member name or `(name, attributes...)`. Attributes:
//   Range{min, max}   editor drag limits (sets FieldFlags::EditorRange)
//   Help{"text"}      tooltip / doc string
//   ReadOnly, Transient, Save, Replicated   FieldFlags bits
//
// MCE_COMPONENT marks the type as an ECS component (TypeFlags::Component) and enforces the
// component rule: trivially copyable, standard layout. MCE_REFL_STRUCT reflects a plain value
// struct used as a nested field (kind Struct); it must appear before any type that nests it.
// MCE_ENUM(E, A, B, C) reflects enumerator names for dropdowns and text serialization.
//
// The macros emit only compile-time tables plus one registrar object per type; component ids
// are assigned by the ECS at register_components time, never here (ecs-gameplay R6).
// Types must be named unqualified (the macro is used inside the type's namespace) and have
// public members only.

#include <cstddef>
#include <new>
#include <type_traits>

#include "mce/refl/field_traits.h"
#include "mce/type_name.h"

namespace mce::refl {

// Field attributes, usable unqualified inside the macro field lists.
namespace attr {
struct Range {
    f32 min;
    f32 max;
};
struct Help {
    const char* text;
};
struct Flag {
    u32 bits;
};
inline constexpr Flag ReadOnly{FieldFlags::ReadOnly};
inline constexpr Flag Transient{FieldFlags::Transient};
inline constexpr Flag Save{FieldFlags::Save};
inline constexpr Flag Replicated{FieldFlags::Replicated};
}  // namespace attr

namespace detail {

constexpr void apply_attr(FieldRecord& f, attr::Range r) {
    f.editor_min = r.min;
    f.editor_max = r.max;
    f.flags |= FieldFlags::EditorRange;
}
constexpr void apply_attr(FieldRecord& f, attr::Help h) { f.help = h.text; }
constexpr void apply_attr(FieldRecord& f, attr::Flag fl) { f.flags |= fl.bits; }

template <class Member, class... Attrs>
constexpr FieldRecord make_field(const char* name, usize offset, Attrs... attrs) {
    static_assert(std::rank_v<Member> <= 1, "reflected fields support one-dimensional fixed arrays only");
    using Elem = std::remove_cv_t<std::remove_extent_t<Member>>;
    using Traits = FieldTraits<Elem>;
    static_assert(Traits::supported,
                  "field type is not reflectable: add MCE_REFL_STRUCT/MCE_ENUM for it, or specialize "
                  "mce::refl::FieldTraits");
    FieldRecord f;
    f.name = StringId(name);
    f.name_str = name;
    f.kind = Traits::kind;
    f.offset = static_cast<u32>(offset);
    f.size = static_cast<u32>(sizeof(Elem));
    f.count = std::is_array_v<Member> ? static_cast<u32>(std::extent_v<Member>) : 1u;
    if constexpr (Traits::type_name != nullptr) f.type_id = StringId(Traits::type_name);
    if constexpr (Traits::kind == FieldKind::String) f.string_ops = &Traits::string_ops;
    (apply_attr(f, attrs), ...);
    return f;
}

template <usize N>
struct FieldTable {
    FieldRecord data[N == 0 ? 1 : N];
    static constexpr usize size = N;
};

template <class T>
void construct_default(void* dst) {
    ::new (dst) T{};
}

class TypeRegistrar {
public:
    explicit TypeRegistrar(const TypeRecord* type) : type_(type), linked_(register_type(type)) {}
    ~TypeRegistrar() {
        if (linked_) unregister_type(type_);
    }
    TypeRegistrar(const TypeRegistrar&) = delete;
    TypeRegistrar& operator=(const TypeRegistrar&) = delete;

private:
    const TypeRecord* type_;
    bool linked_;
};

class EnumRegistrar {
public:
    explicit EnumRegistrar(const EnumRecord* e) : enum_(e), linked_(register_enum(e)) {}
    ~EnumRegistrar() {
        if (linked_) unregister_enum(enum_);
    }
    EnumRegistrar(const EnumRegistrar&) = delete;
    EnumRegistrar& operator=(const EnumRegistrar&) = delete;

private:
    const EnumRecord* enum_;
    bool linked_;
};

}  // namespace detail
}  // namespace mce::refl

// ---------------------------------------------------------------- preprocessor helpers
// Requires a conforming preprocessor (/Zc:preprocessor on MSVC, set by mce_apply_compile_policy).

#define MCE_PP_EXPAND(...) MCE_PP_EXPAND4(MCE_PP_EXPAND4(MCE_PP_EXPAND4(MCE_PP_EXPAND4(__VA_ARGS__))))
#define MCE_PP_EXPAND4(...) MCE_PP_EXPAND3(MCE_PP_EXPAND3(MCE_PP_EXPAND3(MCE_PP_EXPAND3(__VA_ARGS__))))
#define MCE_PP_EXPAND3(...) MCE_PP_EXPAND2(MCE_PP_EXPAND2(MCE_PP_EXPAND2(MCE_PP_EXPAND2(__VA_ARGS__))))
#define MCE_PP_EXPAND2(...) MCE_PP_EXPAND1(MCE_PP_EXPAND1(MCE_PP_EXPAND1(MCE_PP_EXPAND1(__VA_ARGS__))))
#define MCE_PP_EXPAND1(...) __VA_ARGS__
#define MCE_PP_PARENS ()

// MCE_PP_FOR_EACH(m, ctx, a, b, c) -> m(ctx, a) m(ctx, b) m(ctx, c)   (up to 256 items)
#define MCE_PP_FOR_EACH(m, ctx, ...) __VA_OPT__(MCE_PP_EXPAND(MCE_PP_FOR_EACH_STEP(m, ctx, __VA_ARGS__)))
#define MCE_PP_FOR_EACH_STEP(m, ctx, a, ...) m(ctx, a) __VA_OPT__(MCE_PP_FOR_EACH_AGAIN MCE_PP_PARENS(m, ctx, __VA_ARGS__))
#define MCE_PP_FOR_EACH_AGAIN() MCE_PP_FOR_EACH_STEP

#define MCE_PP_IS_PAREN(x) MCE_PP_IS_PAREN_CHECK(MCE_PP_IS_PAREN_PROBE x)
#define MCE_PP_IS_PAREN_PROBE(...) ~, 1
#define MCE_PP_IS_PAREN_CHECK(...) MCE_PP_IS_PAREN_SECOND(__VA_ARGS__, 0, ~)
#define MCE_PP_IS_PAREN_SECOND(a, b, ...) b

#define MCE_PP_IIF(c) MCE_CONCAT(MCE_PP_IIF_, c)
#define MCE_PP_IIF_0(t, f) f
#define MCE_PP_IIF_1(t, f) t

#define MCE_PP_UNPAREN(...) __VA_ARGS__

// ---------------------------------------------------------------- field expansion

#define MCE_REFL_COUNT_(T, f) +1
#define MCE_REFL_FIELD_(T, f) MCE_PP_IIF(MCE_PP_IS_PAREN(f))(MCE_REFL_FIELD_PAREN_, MCE_REFL_FIELD_ATTR_)(T, f)
#define MCE_REFL_FIELD_PAREN_(T, f) MCE_REFL_FIELD_ATTR_(T, MCE_PP_UNPAREN f)
#define MCE_REFL_FIELD_ATTR_(T, ...) MCE_REFL_FIELD_ATTR_I_(T, __VA_ARGS__)
#define MCE_REFL_FIELD_ATTR_I_(T, name, ...)                                                              \
    mce_refl_table_.data[mce_refl_i_++] =                                                                 \
        ::mce::refl::detail::make_field<decltype(T::name)>(#name, offsetof(T, name) __VA_OPT__(, ) __VA_ARGS__);

#define MCE_REFL_TYPE_(T, type_flags, ...)                                                                    \
    static_assert(std::is_standard_layout_v<T>, "reflected types must be standard-layout plain structs");    \
    [[maybe_unused]] constexpr const char* mce_refl_type_name(T*) { return #T; }                             \
    inline constexpr auto MCE_CONCAT(mce_refl_fields_, T) = [] {                                              \
        using namespace ::mce::refl::attr;                                                                    \
        ::mce::refl::detail::FieldTable<0 MCE_PP_FOR_EACH(MCE_REFL_COUNT_, T, __VA_ARGS__)> mce_refl_table_{}; \
        [[maybe_unused]] ::mce::usize mce_refl_i_ = 0;                                                        \
        MCE_PP_FOR_EACH(MCE_REFL_FIELD_, T, __VA_ARGS__)                                                      \
        return mce_refl_table_;                                                                               \
    }();                                                                                                      \
    inline constexpr ::mce::refl::TypeRecord MCE_CONCAT(mce_refl_record_, T){                                 \
        ::mce::StringId(#T),                                                                                  \
        #T,                                                                                                   \
        static_cast<::mce::u32>(sizeof(T)),                                                                   \
        static_cast<::mce::u32>(alignof(T)),                                                                  \
        (type_flags),                                                                                         \
        ::mce::Span<const ::mce::refl::FieldRecord>(MCE_CONCAT(mce_refl_fields_, T).data,                     \
                                                    MCE_CONCAT(mce_refl_fields_, T).size),                    \
        nullptr,                                                                                              \
        &::mce::refl::detail::construct_default<T>,                                                           \
        ::mce::kTypeHash<T>,                                                                                  \
    };                                                                                                        \
    [[maybe_unused]] inline const ::mce::refl::TypeRecord& mce_refl_type_record(T*) {                         \
        return MCE_CONCAT(mce_refl_record_, T);                                                               \
    }                                                                                                         \
    inline const ::mce::refl::detail::TypeRegistrar MCE_CONCAT(mce_refl_registrar_, T){                       \
        &MCE_CONCAT(mce_refl_record_, T)};

// Reflected ECS component: plain, trivially copyable struct (ecs-gameplay §2.1 rule).
#define MCE_COMPONENT(T, ...)                                                                          \
    static_assert(std::is_trivially_copyable_v<T>, "components must be trivially copyable (no heap, " \
                                                   "no destructors)");                                \
    MCE_REFL_TYPE_(T, ::mce::refl::TypeFlags::Component, __VA_ARGS__)

// Reflected plain value struct (nested field type, settings schema, save header...).
#define MCE_REFL_STRUCT(T, ...) MCE_REFL_TYPE_(T, ::mce::refl::TypeFlags::None, __VA_ARGS__)

#define MCE_REFL_ENUM_VALUE_(E, v) ::mce::refl::EnumValue{::mce::StringId(#v), #v, static_cast<::mce::i64>(E::v)},

#define MCE_ENUM(E, ...)                                                                                  \
    static_assert(std::is_enum_v<E>, "MCE_ENUM expects an enum type");                                    \
    [[maybe_unused]] constexpr const char* mce_refl_enum_name(E*) { return #E; }                          \
    inline constexpr ::mce::refl::EnumValue MCE_CONCAT(mce_refl_enum_values_, E)[] = {                    \
        MCE_PP_FOR_EACH(MCE_REFL_ENUM_VALUE_, E, __VA_ARGS__)};                                           \
    inline constexpr ::mce::refl::EnumRecord MCE_CONCAT(mce_refl_enum_record_, E){                        \
        ::mce::StringId(#E),                                                                              \
        #E,                                                                                               \
        static_cast<::mce::u32>(sizeof(E)),                                                               \
        std::is_signed_v<std::underlying_type_t<E>>,                                                      \
        ::mce::Span<const ::mce::refl::EnumValue>(MCE_CONCAT(mce_refl_enum_values_, E)),                  \
    };                                                                                                    \
    [[maybe_unused]] inline const ::mce::refl::EnumRecord& mce_refl_enum_record(E*) {                     \
        return MCE_CONCAT(mce_refl_enum_record_, E);                                                      \
    }                                                                                                     \
    inline const ::mce::refl::detail::EnumRegistrar MCE_CONCAT(mce_refl_enum_registrar_, E){             \
        &MCE_CONCAT(mce_refl_enum_record_, E)};
