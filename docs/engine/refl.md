# refl — L1 field metadata registry: reflected structs, enums and property paths

`mce_refl` is layer L1 (`src/engine/refl/CMakeLists.txt`) and the sole source of component field metadata
(ARCHITECTURE fix F4, ADR-0009). Its only dependency is `mce::mce_core`. It is linked publicly by `mce_ecs`
and `mce_asset`, directly by `mce_input`, `mce_audio`, `mce_physics2d`, `mce_ui`, `mce_scene` and
`mce_gameplay_kit`, and is part of the `mce_engine` umbrella. Consumers include the scene cook
(`asset/cook/src/scene_importer.cpp`, `scene_writer.cpp`), the runtime scene loader, ECS prefab overrides and
saves, `mce_ui` binding, the editor inspector and the editor MCP server. The registry knows nothing about
entities, scenes or the inspector. Headers live in `src/engine/refl/include/mce/refl/`; `mce/refl/refl.h` is
the umbrella.

## Public surface

- `mce/refl/type.h`: the records and the registry.
  - `FieldKind`: `Bool`, signed and unsigned integers, `F32`/`F64`, `String` (a `FixedString<N>`), `Enum`,
    `Vec2`/`Vec3`/`Vec4`/`Quat`/`Color`, `AssetRef`, `EntityRef`, `Struct`. `field_kind_name()` names them.
  - `FieldFlags`: `ReadOnly` (shown, never written), `Transient` (never serialized into scenes), `Save`
    (included in save games), `EditorRange` (editor min/max are set), `WorldPoint` (a Vec3, or a fixed array
    of them, holding a world-space point: a floating-origin rebase moves it, M-OPENWORLD A2).
  - `FieldRecord`: name (`StringId` and text), kind, byte offset, element size, `count` (> 1 for a fixed
    array), `type_id` of a referenced Struct/Enum/ref type, help text, flags, editor range, `StringOps` for
    String fields.
  - `TypeRecord`: id, name, size, align, `TypeFlags::Component`, the field span, `construct_default`
    (placement `T{}`), and `type_hash` (= `kTypeHash<T>`, the ECS component identity). `find_field()`.
  - `EnumRecord` / `EnumValue`, with `find()` by name or by value.
  - `find_type(StringId)`, `all()` (sorted by name), `find_enum`, `all_enums`, and `register_type` /
    `unregister_type` / `register_enum` / `unregister_enum`, which the registrar objects call and tools or
    tests may call directly.
- `mce/refl/macros.h`: the declaration macros, used at namespace scope in the type's own namespace.
  - `MCE_COMPONENT(T, fields...)`: reflected ECS component; static-asserts trivially copyable and standard
    layout.
  - `MCE_REFL_STRUCT(T, fields...)`: plain value struct used as a nested field or schema.
  - `MCE_ENUM(E, values...)`: enumerator names and values.
  - Field attributes: `Range{min, max}`, `Help{"text"}`, `ReadOnly`, `Transient`, `Save`, `WorldPoint`. A
    field is a bare member name or `(name, attributes...)`.
- `mce/refl/field_traits.h`: `FieldTraits<T>` maps a member type to its `FieldKind`. Built in: bool,
  fixed-width integers, floats, `FixedString<N>`, math vectors, `quat`, `Color`, enums, and reflected structs.
  Higher layers specialize it for their handles: `mce::Entity` in `mce/ecs/entity.h` (EntityRef) and
  `asset::AssetRef<T>` in `mce/asset/handle.h` (AssetRef). Typed helpers: `type_of<T>()`, `type_id<T>()`,
  `enum_of<E>()`, `kind_matches<V>()`, and the concepts `ReflectedStruct` / `ReflectedEnum`.
- `mce/refl/property_path.h`: the addressing scheme shared by the inspector, undo diffs, prefab overrides,
  saves and UI binding. Paths look like `hp`, `body.velocity`, `body.velocity.x` (lanes `x y z w`, or `r g b
  a` for Color) and `waypoints[2].y`.
  - `resolve_path(const TypeRecord&, StringView) -> Result<PropertyRef>`: walks the registry once. The
    returned `PropertyRef` (leaf field, kind, offset from the root, size, count, type id) can be cached and
    applied to any instance of the root type.
  - `property_ptr<V>`, `get_property<V>`, `set_property<V>`: typed access; `get_string` / `set_string` for
    String leaves through the field's `StringOps`.

A minimal declaration:

```cpp
struct Health { f32 hp = 100; f32 max = 100; };
MCE_COMPONENT(Health, (hp, Range{0, 1000}, Save), max)
```

## Rules and invariants

- One metadata source. Inspector, scene cook and load, undo, prefab overrides, saves and UI binding all read
  `TypeRecord`/`FieldRecord`; no consumer keeps its own field tables (core.md §2.8 consumer acceptance
  matrix).
- Reflected types are standard-layout plain structs with public members, named unqualified inside their own
  namespace. Components must also be trivially copyable. Fixed arrays are one-dimensional only. An unsupported
  member type is a compile error that asks for `MCE_REFL_STRUCT`, `MCE_ENUM` or a `FieldTraits`
  specialization.
- `MCE_REFL_STRUCT` for a nested type must appear before any type that nests it.
- The macros emit only compile-time tables plus one registrar object per type. Component ids are assigned by
  the ECS at registration time, never by `mce_refl`.
- The registry is fixed capacity (1024 types, 256 enums) in `constinit` tables, so registrars see a valid
  empty registry in any static-init order, and it uses no heap (`src/registry.cpp`).
- `all()` is sorted by name, so iteration order does not depend on static-init order across translation units.
- Duplicates: a second registration of an equivalent record (same name, size, align and field count) returns
  false and the first stays authoritative. This is what happens when a header-declared type is seen by more
  than one module. The same name registered with a different layout asserts, and two different names hashing
  to the same `StringId` assert.
- The registry is main-thread only after static initialization.
- Expected failures in `resolve_path` return `Errc::InvalidArgument` (syntax, index out of range, lane on a
  non-vector) or `Errc::NotFound` (unknown field or unregistered nested type), with the path in `Error::path`.
  Typed access with a C++ type that does not match the leaf's kind, type id or size is a programmer error and
  asserts.
- The macros need a conforming preprocessor (`/Zc:preprocessor` on MSVC, set by `mce_apply_compile_policy`).
- Across the engine/game-module boundary the host and module share one refl registry through the `mce_engine`
  umbrella (ADR-0021).

## How it is tested

ctest `test_refl` (`tests/refl/CMakeLists.txt`) links only `mce_refl`. Shared test types live in
`tests/refl/test_types.h`.
- `test_registry.cpp`: "MCE_COMPONENT builds the expected field table", "every supported field kind is deduced
  from the member type", "registry lookup, ordering and flags", "zero-field components are valid",
  "construct_default produces the declared default values", "enums reflect names and values", "duplicate
  registrations: equivalent records are ignored, collisions assert", "register/unregister keeps the table
  sorted", "field kind names".
- `test_second_tu.cpp`: a second translation unit that includes the shared types again. Covers "a type
  declared in a header and included by two TUs registers once" and "a type no code references still registers
  before main".
- `test_property_path.cpp`: "top-level paths resolve to the field", "nested struct, fixed array and lane
  paths", "enum, entity-ref and string leaves", "a resolved ref is reusable across instances", "malformed and
  unknown paths fail with a typed error", "nested struct whose type is not registered reports NotFound",
  "typed access with the wrong C++ type asserts".

Consumers test their own use: scene cook and load in `tests/asset` and `tests/scene`, prefab overrides and
saves in `tests/ecs` (see [ecs.md](ecs.md)).

## Design records

- [../architecture/core.md](../architecture/core.md) §2.8 (`mce_refl`, the design intent and the consumer
  acceptance matrix) and §2.9 (save/persistence through the field table).
- [../ARCHITECTURE.md](../ARCHITECTURE.md) §9, ADR-0009: reflection is a standalone L1 module and the only
  metadata source.
- [../architecture/ecs-gameplay.md](../architecture/ecs-gameplay.md) §2.1 (the component rule) and §2.13 (the
  `[Save]` marker).
- `docs/adr/0025-ecs-v05-change-ticks-hierarchy-prefabs.md`:
  `mce_ecs` links `mce_refl`; prefab overrides are a property path plus a typed value.
- `docs/adr/0019-asset-pipeline-m2.md`: the reflection-driven `.mcscene` cook.
- `docs/adr/0021-game-module-loader-m3.md`: one refl registry shared by
  host and module.
- `docs/adr/0027-editor-mcp-server.md`: MCP component reads and writes by refl
  property path.

## Known gaps

- `TypeRecord::base` is reserved and always null: only flattened plain structs are supported, no inheritance
  (`mce/refl/type.h`).
- The trait slots sketched in core.md §2.8 (`serialize_fn`, `inspect_fn`, `hash_fn`, `display_fn`) and its
  `FixedArray` kind do not exist in the code. Fixed arrays are `FieldRecord::count`, and consumers walk the
  field table directly.
- The registry is capped at 1024 types and 256 enums; a full registry asserts (`src/registry.cpp`).
