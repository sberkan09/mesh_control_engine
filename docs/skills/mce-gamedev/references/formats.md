# Dosya biçimleri

Kaynak: `docs/user-guide/02, 04, 05, 08, 11, 12, 13`. Bu dosya özet; alan listesi için `asset_schema {type}` ve `refl_describe_component {name}` kullan. Her biçim JSON'dur ve `format`, `version`, `guid` içerir; `asset_create` taze GUID'i kendisi koyar, `asset_write` `format`/`version`/`guid` yazmayı **reddeder**.

## Proje klasörü

```
MyGame/
  MyGame.mceproj          # {format:"mce-project", name, title, engine_version, games:["my_game"], start_scene:"<guid>", budgets?}
  assets/                 # TEK düzenlenen ağaç (yollar hep assets/ göreli)
    scenes/main.mcscene
    prefabs/*.mcprefab
    textures|models|audio|fonts|ui|...  (düzen sana ait)
    foo.png + foo.png.mceasset   # sidecar: GUID + importer ayarları
  games/my_game/module/   # components.h (MCE_COMPONENT), my_game.cpp (install, sistemler), types.cpp
  tests/*.mctest          # senaryolar
  cooked/  assets.mce     # ÇIKTI, silinebilir, byte-aynı yeniden üretilir
  CREDITS.md  .mcp.json  friction.jsonl  tasks.mctasks
```
`start_scene` değişince yeniden configure et. `scene_new` yeni GUID verir: başlangıç sahnesini yeniden yazarken `scene_open` + `scene_clear` kullan, yoksa `.mceproj` boşa işaret eder.

## Değer yazımı (.mcscene / MCP)

| Tür | Yazım |
|---|---|
| bool, sayı | `true`, `3.5` (tam sayı alanlarına tam sayı) |
| string | `"text"` (FixedString<N> kapasitesini aşma) |
| enum | adı: `"Dynamic"` |
| vec2/3/4, quat, renk | `[x,y]`, `[x,y,z]`, `[x,y,z,w]` (quat `[x,y,z,w]`), `[r,g,b,a]` doğrusal 0..1 |
| asset | `"guid:<uuid>"` veya `null` |
| entity ref | dosyada `"$<index>"`; MCP'de id veya benzersiz ad |
| iç içe struct | nesne: `{"type":"Circle","radius":0.3}` |
| sabit dizi | **tam `count` eleman** (ör. `CameraRail3D.points` 8, `Blackboard.entities` 8) |

Atlanan alan C++ varsayılanını korur (`"RigidBody2D": {}` geçerli statik gövde). `Transient` alanları yazma. Bileşen adları niteliksiz (`Sprite`, `mce::kit::Sprite` değil).

## Sahne ve prefab

```json
{ "format":"mce-scene","version":1,"guid":"...","name":"main",
  "entities":[
    {"name":"Camera","components":{"Camera3DComponent":{"primary":true},"LocalTransform":{"position":[0,2,8]}}},
    {"name":"Turret A","prefab":"guid:a1b2...","components":{"LocalTransform":{"position":[5,0,0]},"Health3D":{"hp":99}}},
    {"name":"Barrel","components":{"ChildOf":{"parent":"$0"}}} ]}
```
- Prefab = aynı biçim, `"format":"mce-prefab"`, 0. entity kök. Örnekte `components` **kök alanlarını override** eder; alt-entity override'ı yok. Cook örnekleri düz satırlara açar (iç içe dahil).
- Hiyerarşi: `ChildOf{parent}`; çocuğun `LocalTransform`'u ebeveyne göredir, `WorldTransform` motor yazar. Fizik gövdeli (2D) çocuk fizik tarafından yerleştirilir: gövdeleri kök yap.
- Sahne değiştirme: oyun kodunda `scene::request_change(world,"scenes/level2.mcscene")`; kaynak/skor kaybolmasın diye **resource**'a koy, entity'ye değil.
- Her oynanabilir sahnede tam bir `primary` kamera (2D `Camera2DComponent`, 3D `Camera3DComponent`).
- Büyük dünya: `SceneChunk` + `StreamingFocus` (+ `FloatingOrigin3D`, `ChunkLod3D` ile HLOD; MCP `hlod_bake`). Bölge sahneleri dünya koordinatında yazılır.
- Lint: `scene_lint`, `scene_lint_fix`, `scene_lint_all`; diff: `scene_diff`.

## Sidecar (.mceasset)

```json
{"guid":"...","importer":"texture","importer_version":1,"settings":{"srgb":true,"mips":true}}
```
Commit et (GUID kimliktir). Ayarlar: `texture` (`srgb`, `mips`, `filter` nearest|linear, `max_size`, `compress`), `wav/ogg` (`normalize`, `loop`, `loop_start/end`, `compress` pcm|vorbis, `quality`), `font` (`size`, `ranges`, `sdf`), `gltf` (`scale`, `max_triangles`, `skinning`, `lod_levels`, `lod_ratio`, `lod_error`, `ragdoll`, `animations_from`), `obj` (`up`:"y"|"z", `scale`...), `fbx` (`scale`, `sample_rate`...). Ayar değişimi yeniden cook eder. Normal/roughness/AO/LUT/yükseklik dokuları `"srgb": false`.

## Malzeme .mcmat

```json
{"format":"mce-material","version":1,"guid":"...","base_color":[1,1,1,1],"metallic":0,"roughness":1,"emissive":[0,0,0],
 "uv_scale":[1,1],"alpha_mode":"opaque|mask|blend","alpha_cutoff":0.5,"double_sided":false,"unlit":false,
 "textures":{"base_color":"guid:..","normal":"guid:..","metallic_roughness":"guid:..","emissive":"guid:..","height":"guid:.."},
 "height_scale":0.05}
```
`metallic_roughness` dokusu: R=occlusion, G=roughness, B=metallic. glTF malzemeleri mesh içinde gelir; `.mcmat` el yapımı malzeme içindir ve `MeshRef.material` olarak tüm alt-mesh'leri değiştirir. Eklentiler: `clearcoat`, `sheen_*`, `anisotropy`, `transmission`, `ior`.

## UI belgesi .mcui

`ui_create/ui_widget_add/ui_widget_set/ui_widget_remove/ui_theme_set/ui_preview` ile düzenle (kanonik biçimde yazar, hatalıysa hiçbir şey yazmaz). Sahneye `UiScreen` bileşeniyle bağlanır.
```json
{"format":"mce-ui","version":1,"guid":"...","font":"guid:<Font>","sprites":"guid:<SpriteSheet>",
 "widgets":[
  {"name":"bg","fill":true,"style":{"tint":[0,0,0,0.6]}},
  {"name":"menu","kind":"Stack","at":[0.5,0.45,320,0],"stack":{"spacing":12},"children":[
    {"name":"play","kind":"Button","text":"PLAY","default_focus":true,"nav":{"down":"menu/volume"}},
    {"name":"volume","kind":"Slider","range":[-60,0,3],"convar":"audio.music_db"}]}]}
```
- Widget türleri: Panel, Image, Text, Button, Toggle, Slider, Stack, Grid, Tabs, Minimap (`MapMarker`lardan harita), Console. Yol adresi `menu/play`. Kardeş adları benzersiz.
- `at:[ax,ay,w,h]` demirli konum, `anchors`, `stack`, `grid`, `style` (tüm durumlar) / `styles` (tek durum: Hover, Pressed, Focused, Disabled), `own_style` (tema üzerine kendi varsayılanını koru), `text_align`, `bind`/`convar`.
- **HUD veri bağlama**: `bind_property(widget, world, entity, "Health/hp")` C++ tarafında; belge tarafında envanter için widget **adları** tanınır: `slot_1..24`, `equip_*`, `inventory_weight`, `status_1..8`.
- Düşük çözünürlükte küçük görünme: `ui.reference_height 720` convar'ı UI'yı pencere kısa kenarıyla ölçekler.
- Temalar `.mcuitheme`: şablonda `pixel`, `clean`, `story`. `project_check` 16 px altı / 4.5:1 altı kontrastı uyarır.
- UI kuralı: UI oyun durumuna doğrudan yazmaz; menü olay üretir, host/sistem durumu değiştirir. Oyun modülleri `mce/ui/` include edemez (duvar).
- Basit HUD: `TextLabel` + `Parallax{factor:[0,0]}` kameraya yapışır; yüksek `layer`.

## Animasyon grafiği .mcanim (3D, `Animator.graph`)

```json
{"format":"mce-animgraph","version":1,"guid":"...","name":"hero","params":["speed","attack"],"start":"locomotion",
 "states":[{"name":"locomotion","blend":{"param":"speed","points":[{"clip":"Idle","at":0},{"clip":"Walk","at":2},{"clip":"Run","at":6}]},
            "events":[{"time":0.25,"name":"footstep"}]},
           {"name":"slash","clip":"Slash","loop":false,"speed":1.2}],
 "transitions":[{"from":"*","to":"slash","when":[{"param":"attack","op":">","value":0.5}],"fade":0.1},
                {"from":"slash","to":"locomotion","on_finish":true,"fade":0.2}]}
```
- En fazla 4 parametre. Klip adları glTF animasyon adlarıdır: aynı klip adlı her mesh aynı grafiği kullanır.
- Hareketten otomatik: `Locomotion3D` hareketi (`speed`, `move_x`, `move_y`, `vertical_speed`, `grounded`, `airborne`, `crouch`) grafik parametrelerine **ada göre** yazar; `stride_speed` oynatma hızını ayarlar. Grafikte bu adlarda parametre tanımla.
- Sürüm 2: 2D blend space, katmanlar (kemik maskesi, additive), kök hareketi, IK (iki-kemik, look-at, ayak). Olaylar `AnimEvent3D`.
- Başka karakterden/kütüphaneden klip: sidecar `animations_from` veya `mce retarget <kaynak.glb> --onto <karakter.glb>` (kemik adıyla bağlanır; kemik adları tutmalı). Tek parça düğüm animasyonlu (iskinsiz) karakterler (Kenney Blocky) yalnızca `--map-only` ile retarget olur.
- 2D iskelet: `.mcskel2d` (`skel2d_*` araçları), Spine/DragonBones içe aktarma `mce import spine|dragonbones`.

## Parçacık .mcpart

```json
{"format":"mce-particles","version":1,"guid":"..","name":"embers","texture":"guid:..","rate":30,"max_particles":128,
 "lifetime":[1.2,2.2],"speed":[0.6,1.6],"direction":90,"spread":25,"gravity":[0,0.6],"drag":0.6,"size":[0.35,0.05],
 "color_start":[1,0.7,0.25,1],"color_end":[1,0.2,0.05,0],"blend":"additive"}
```
Her yayıcı tohumlu PCG kullanır (`seed`), 3D için `ParticleEmitter3D`.

## Diziler, zaman çizelgeleri, girdi, eşya

- **`.mcseq`**: `{"steps":[{"wait":0.5},{"move_to":[4,2],"speed":3},{"anim":"attack"},{"sound":"guid:..","gain_db":-3},{"wait_event":"x"},{"emit":"y"},{"call":"name"},{"spawn":"guid:..","at":[0,1]},{"destroy":true}]}`; `SequencePlayer{sequence}`. Bekleyen adım kendi tickini yer; beklemesiz döngü cook hatasıdır.
- **`.mctimeline`** (`timeline_create/add_track/add_key`): transform/özellik/kesme/ipucu/altyazı izleri, `TimelinePlayer` + `Cutscene`.
- **`.mcbt`** davranış ağacı (`bt_create/bt_add_node/bt_set/bt_trace`), `AiBrain`, algı `Perceiver`/`Stimulus`.
- **`.mcinput`**: `maps[].actions[]` (`name`, `type` button|axis1d|axis2d, `bindings`: `key`, `mouse_button`, `mouse_delta`, `gamepad_button`, `gamepad_axis`, `scale`, `deadzone`). Eylemler `InputState`'ten okunur; çok oyunculu için `"player": n`.
- **`.mcitems`**: eşya katalogu + loot tabloları + tarifler (`Inventory`, `Equipment`, `LootDrop`, `Container`, `CraftingStation`).
- **`.mcroads`**: yol ağı (`roads_generate` ızgara üretir, `roads_info` kontrol eder); `TrafficSpawner3D`, `TrafficDriver3D`, `RoadNetwork3D`, `CrowdSpawner3D`.
- **`.mcterrain`**: yükseklik haritası + katmanlar (`terrain_generate/sculpt/paint/import`).
- **`.mcsheet`** (sprite sayfası + flipbook klipleri), **`.mctilemap`** (`tilemap_paint`), Aseprite/Tiled/LDtk için `asset_import`.
- **`.luau`** betik (cook varlığı, `ScriptSystem`, `mce.*` API; `script_check`).
- **`.mcstore/.mcservices/.mcconfig/.mcdefaults`**: satın alma, servisler, uzaktan yapılandırma (bkz. user-guide 22, 23).
