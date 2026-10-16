# 5. Assets and the cook

Everything the game loads is a **cooked** binary chunk inside a `.mce` container. You author source
files (PNG, WAV, TTF, glTF) and native text assets (scenes, prefabs, materials, particle presets,
sequences, tilemaps, shaders) under `assets/`; the cook turns them into `cooked/` during
development and into one `assets.mce` pack for shipping. The runtime never parses text or image
formats, which is what keeps the shipped game small and safe.

## 5.1 Asset types

| You author | Extension | Cooked into | Used by |
|---|---|---|---|
| Image | `.png`, `.jpg`, `.jpeg` | texture (RGBA8, optional mips, sRGB flag) | `Sprite`, materials, tilesets, particles, LUTs, normal maps |
| Sound | `.wav` | 48 kHz float clip with loop points | `AudioSource`, `PlaySfx`, sequences |
| Font | `.ttf`, `.otf` | glyph atlas plus metrics | `TextLabel` |
| Model | `.gltf`, `.glb` | mesh: quantized streams, node tree, inline materials, animation clips, optional skins | 3D rendering (chapter 8) |
| Material | `.mcmat` | metallic-roughness parameters plus texture GUIDs | 3D rendering |
| Terrain | `.mcterrain` | a heightmap: u16 samples, size, height range, chunk size | `Terrain3D` (chapter 8) |
| Scene | `.mcscene` | columnar entity rows | the game, the editor |
| Prefab | `.mcprefab` | the same, entity 0 as root | instances in scenes, runtime spawns |
| Tilemap | `.mctilemap` | one tile layer over a tileset | `TilemapLayer` |
| Sprite sheet | `.mcsheet` | texture regions and named flipbook clips with frame events | `AnimatedSprite`, `Sprite.region` |
| String table | `assets/loc/<locale>.csv` | one language's text by key (`StringTable` chunks) | `TextLabel.loc_key`, `loc::Localization` (chapter 21) |
| Animation graph | `.mcanim` | states over a mesh's clips, 1D blends, transitions, events | `Animator.graph` |
| Particle preset | `.mcpart` | emitter parameters | `ParticleEmitter` |
| Sequence | `.mcseq` | an action list | `SequencePlayer` |
| Timeline | `.mctimeline` | keyed tracks: transforms, properties, cuts, cues, subtitles | `TimelinePlayer` (chapter 13.5) |
| Input map | `.mcinput` | action maps and bindings | the player's input (chapter 11) |
| UI document | `.mcui` | a widget tree: anchors, per-state styles, layouts, the skin by reference | `ui::build_document` (chapter 12.2) |
| Shader | `.vert.glsl`, `.frag.glsl`, `.comp.glsl` | validated GLSL, variants, SPIR-V reflection | custom shaders (not yet reachable from a project) |

## 5.2 Sidecars

Every source file gets a sidecar `<file>.mceasset` next to it with the asset's GUID and the
importer's settings:

```json
{
  "guid": "201aeae6-198d-4dac-b695-c65327f35e75",
  "importer": "texture",
  "importer_version": 1,
  "settings": { "srgb": true, "mips": false }
}
```

Commit sidecars to version control: the GUID is the asset's identity, and every reference to the
asset is that GUID. Native text assets (`.mcscene`, `.mcmat`, ...) carry their GUID inline and have
no sidecar.

Settings per importer (every key is optional; the defaults are shown):

| Importer | Settings |
|---|---|
| `texture` | `"srgb": true` (colour data; set `false` for normal maps, LUTs and data textures), `"mips": true`, `"filter": "nearest"` (`"linear"` for smooth, non-pixel-art sprites; chapter 7.1) |
| `wav` | `"normalize": true` (peak to -1 dBFS), `"loop": false`, `"loop_start"`, `"loop_end"` (source-rate frames; default to the file's own loop markers), `"compress": "pcm"` (`"vorbis"` stores Ogg Vorbis, about a tenth of the size; chapter 10.6), `"quality": 0.4` (Vorbis, -0.1..1) |
| `ogg` | the same as `wav`, with `"compress": "vorbis"` by default |
| `font` | `"size": 32` (pixel height), `"ranges": [[32, 126]]` (inclusive codepoint ranges), `"padding": 1`, `"strings": true` (also every character the string tables use), `"sdf": true` (bake a distance field: text drawn well above `size` keeps sharp edges; outlines wider than the field fade), `"spread": 4` (with `sdf`: how many pixels at `size` the field reaches, default `size / 8`, at least 2) |
| `gltf` | `"scale": 1.0` (applied to every root node), `"max_triangles": 0` (0 = no budget; a budget makes an over-size mesh a cook error), `"skinning": true` (`false` imports skinned meshes as static ones; see Skinning in [chapter 8](08-3d.md#86-rendering-a-frame-yourself)), `"lod_levels": 0` (1..3 generates that many coarser levels), `"lod_ratio": 0.35`, `"lod_error": 0.05` (section 8.4), `"ragdoll": {}` (per-bone ragdoll overrides by bone name; section 9.8) |

Edit settings in the sidecar file, or through MCP's `asset_set_import_settings`. A settings change
re-cooks the asset.

## 5.3 Adding files

1. Copy the file into `assets/` (any subfolder; the layout is yours).
2. Give it a sidecar. Either run the cook with adoption, which writes a sidecar with default
   settings for every source file that lacks one:

   ```sh
   build/<preset>/projects/MyGame/my_game_cook projects/MyGame --adopt
   ```

   or ask an MCP agent for `cook_run {"adopt": true}`.
3. In the editor, "Rescan assets" (Assets panel) if it was open.

Adopted textures default to `srgb: true`; fix the sidecar for a normal map or a LUT before using it.

## 5.4 The cook

```sh
build/<preset>/projects/MyGame/my_game_cook projects/MyGame            # cook into projects/MyGame/cooked/
build/<preset>/projects/MyGame/my_game_cook projects/MyGame --pack     # also write projects/MyGame/assets.mce
build/<preset>/projects/MyGame/my_game_cook projects/MyGame --force    # ignore the cook DB, redo everything
build/<preset>/projects/MyGame/my_game_cook projects/MyGame --adopt    # write missing sidecars first
```

Exit codes: 0 clean, 1 cook errors, 2 usage. Errors are printed as `file:line:col: error: message
[stage]`, the same text the editor's Cook panel shows.

Use the **project's** cook (`my_game_cook`), or the stock `mce_cook --module
build/<preset>/projects/MyGame/libmy_game.so projects/MyGame` (`mce cook MyGame` with the SDK): both
know your components, so scenes that use them cook. The stock `mce_cook` without `--module` only
knows the engine's and fails on `Spin` with "unknown component type".

What the cook does, so its behaviour makes sense:

- **Incremental.** Each asset's re-cook key is a content hash of its source, settings, importer
  version and every dependency's output. Touching a file's timestamp changes nothing; changing one
  texture re-cooks that texture and the scenes and materials that reference it.
- **Deterministic.** Deleting `cooked/` and cooking again gives byte-identical files. The pack is
  sorted by GUID. This is a tested guarantee, which makes `cooked/` disposable.
- **Dependency checked.** A scene that references a GUID that does not exist, or a texture where a
  sound is expected, is a cook error that names the file, line and column.
- **Failure keeps the old output.** A failed asset leaves its previous cooked output in place, so
  the editor and a running game keep working while you fix it.
- **Content-addressed outputs** live under `cooked/<aa>/<guid>/<hash>.mce` with a `manifest.mce`
  mapping GUIDs to paths. You never read these by hand.

The build cooks for you: building `my_game` runs `my_game_cook` over a copy of `assets/` into
`build/<preset>/projects/MyGame/project/assets.mce`, which is the pack the host loads from the
build tree. The editor's F7 cooks into `<Project>/cooked/` for the editor's own use.

## 5.5 Development mount versus the pack

| | `cooked/` directory | `assets.mce` pack |
|---|---|---|
| Written by | the editor (F7), the cook without `--pack` | `--pack`, the build, `ship.py` |
| Loaded by | the editor; `-pack <Project>/cooked` for the player | the host and the player by default |
| Integrity | every file's CRC checked at load | checked once at mount |
| Hot reload | yes, in the editor | no |

A player or host can mount either: `-pack projects/MyGame/cooked` or `-pack path/to/assets.mce`.

## 5.6 References and GUIDs

Components refer to assets by GUID (`AssetRef<Texture>` in C++, `"guid:<uuid>"` in text). At load
the reference holds only the GUID; the engine resolves it to a loaded asset on first use and keeps
the handle. If the asset is missing or fails to load, the reference resolves to a placeholder (the
magenta checker for textures, silence for sounds) and the log says so once.

To find a GUID: the Assets panel's "Copy GUID", the sidecar file, or MCP's `asset_list`.

## 5.7 Native text formats

Besides scenes and prefabs (chapter 4), these text assets are JSON with a `format` and a `guid`:

**Material** (`.mcmat`, chapter 8):

```json
{ "format": "mce-material", "version": 1, "guid": "<uuid>",
  "base_color": [1, 1, 1, 1], "metallic": 0, "roughness": 1, "emissive": [0, 0, 0],
  "uv_scale": [1, 1],
  "alpha_mode": "opaque", "alpha_cutoff": 0.5, "double_sided": false, "unlit": false,
  "textures": { "base_color": "guid:<uuid>", "normal": "guid:<uuid>",
                "metallic_roughness": "guid:<uuid>", "emissive": "guid:<uuid>" } }
```

`uv_scale` repeats the textures across the mesh's UVs: `[8, 8]` on a plane tiles a stone texture
eight times each way (the samplers repeat), so a 30 m courtyard floor is one mesh, not many slabs.

**Terrain** (`.mcterrain`, chapter 8): `resolution` samples per side (2^n+1, 33 to 4097), `chunk` quads
per LOD node (a power of two, default 32), `size` in metres along x and z, `height_range` mapping sample
0 and 65535 to metres. `heights` is base64 of `resolution²` little-endian u16 samples, z rows of x
columns; or leave it out and give `fill` (one height in metres) for a flat start.

```json
{ "format": "mce-terrain", "version": 1, "guid": "<uuid>", "name": "island",
  "resolution": 129, "chunk": 32, "size": [128, 128], "height_range": [0, 32], "fill": 0 }
```

`layers` paints the terrain with up to 8 materials. Each layer names a `.mcmat` (its base color map,
normal map, roughness/metallic map and factors), a `tile` in metres per repeat (default 4), `triplanar`
(also project along x and z on steep ground, so cliffs are not smeared) and `height_strength` (how far
its albedo's brightness lifts it over its neighbours where layers meet). `splat` is the weight map:
base64 of `splat_resolution²` samples (default `min(resolution, 1025)`), each one u8 weight per layer.
Without `splat`, each layer's `auto` rule places it by `height` (metres) and `slope` (degrees), and
ground no rule claims goes to layer 0:

```json
"layers": [
  { "material": "guid:<grass>", "tile": 4, "auto": { "slope": [0, 25] } },
  { "material": "guid:<rock>", "tile": 6, "triplanar": true, "auto": { "slope": [30, 90] } },
  { "material": "guid:<snow>", "auto": { "height": [40, 60] } }
]
```

`holes` cuts the terrain open (cave mouths, tunnel entrances): base64 of a bit per height sample, sample
`z * resolution + x` at bit `i % 8` of byte `i / 8`, `(resolution² + 7) / 8` bytes. Every triangle with a
hole sample as a corner is left out of both the drawn surface and the `TerrainCollider3D` collision, so a
3 × 3 block of hole samples opens a 4 × 4 m gap at 1 m spacing. An all-zero mask cooks to no holes.

The editor's Terrain panel (chapter 3) sculpts and paints a terrain in place. A stroke writes the file
back with explicit `heights` (replacing `fill`), plus `splat` once a layer is painted (from then on the
`auto` rules no longer apply) and `holes` once one is cut; every other key stays as written. Heights
stay inside `height_range`: widen it first to raise higher or dig deeper.

Agents shape terrains over MCP (chapter 14) with the same brushes: `terrain_sculpt` (raise, lower, smooth,
flatten, noise) and `terrain_paint` (a layer, holes, filling them) dab at `[x, z]` points in metres from the
terrain's centre, or along them as a stroke. `terrain_import` reads a heightmap under `assets/` into the
heights: a `.png` (8- or 16-bit grey) or raw samples (`.r16`/`.raw` 16-bit little-endian, `.r8` 8-bit),
resampled to `resolution`, the image's top row along -z and its left column along -x, black and white
mapped to `range` (default `height_range`). `terrain_generate` grows a landscape: fBm value noise
(`feature` metres across the largest hills, `octaves`, `lacunarity`, `gain`, `ridged`), stretched between
`low` and `high` shares of `height_range`, sunk toward the edges by `island`, then erosion-lite: `thermal`
passes settle slopes steeper than `talus_deg` and `droplets` of rain carve channels and silt up hollows.
The same `seed` gives the same terrain. Each writes `heights` and keeps holes and painted layers; a
terrain whose layers follow `auto` rules re-applies them to the new shape.

Give every layer's maps of one kind the same size and import settings: each kind is packed into one
texture array, and a map that differs from the first layer's is left out with a warning.

**Particle preset** (`.mcpart`, chapter 7):

```json
{ "format": "mce-particles", "version": 1, "guid": "<uuid>", "name": "embers",
  "texture": "guid:<uuid>", "rate": 30, "max_particles": 128,
  "lifetime": [1.2, 2.2], "speed": [0.6, 1.6], "direction": 90, "spread": 25,
  "gravity": [0, 0.6], "drag": 0.6, "size": [0.35, 0.05], "spin": [0, 0], "area": [0.3, 0.1],
  "color_start": [1, 0.7, 0.25, 1], "color_end": [1, 0.2, 0.05, 0], "blend": "additive" }
```

**Sequence** (`.mcseq`, chapter 13), **timeline** (`.mctimeline`, chapter 13.5), **tilemap** (`.mctilemap`, chapter 7; the editor's
Tilemap panel writes it) **sprite sheet** (`.mcsheet`, chapter 7.3) and **animation graph** (`.mcanim`, chapter 8.9).

**Shaders** are validated and compiled to SPIR-V at cook, with variants declared as
`// @variant NAME` header lines, but the built-in 2D and 3D shaders are not cooked assets yet and a
project cannot replace them. Treat the shader importer as groundwork. (On macOS the Metal backend
translates the engine's GLSL to MSL with SPIRV-Cross when it creates a pipeline; nothing changes for
a project.)

## 5.8 Hot reload

In the editor, saving a source file (an image editor writing a PNG, a text editor writing a scene)
is picked up within half a second: the changed asset and its dependents re-cook and the viewport
swaps to the new version. A shipped game has no watcher; the player reloads nothing at run time.

## 5.9 Organizing content

- Any folder layout under `assets/` works; the template uses `scenes/`, `textures/`, `sfx/`,
  `fonts/`. Kindle adds `textures/ember/`, `textures/backdrop/`, `textures/switch/`.
- Keep a `CREDITS.md` in the project root for third-party content; `ship.py` copies it into the
  zip.
- Sprites show a whole texture, so one image per sprite frame. Atlas regions and sprite sheets are
  not imported yet (see 7.3 for the animation recipe).
- Large tiled areas: a `TilemapLayer` over a tileset, or one `Sprite` per tile. A single stretched
  sprite smears the texture.
