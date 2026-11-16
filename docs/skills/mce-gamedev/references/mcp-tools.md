# MCP ile motoru sürmek

Tam tablo: `docs/user-guide/14-mcp-ai-workflow.md` (14.2). Sunucu içinden de: `docs_search`, `engine_capabilities`, kaynak `mce://docs/index`. Promptlar: `start_game {brief}`, `add_level`, `polish_pass`.

## Sunucuyu başlatma

```sh
mce mcp MyGame [--scene scenes/main.mcscene] [--allow-cheats] [--agent <ad>]   # SDK
build/<preset>/projects/MyGame/my_game_mcp --project projects/MyGame            # checkout, oyun içine gömülü
build/<preset>/src/editor/mcp/mce_mcp --module build/<preset>/projects/MyGame/libmy_game.so --project projects/MyGame
mce mcp-config MyGame [--desktop]     # .mcp.json (Claude Code) ve Claude Desktop kaydı
```
stdio JSON-RPC, tek satır mesaj. Betik istemcisi örneği: `projects/Kindle/tools/author_level_mcp.py`. Container'da MCP bağlantısı kopabilir; araç şemalarını `ToolSearch select:` ile yeniden yükle.

## Araç grupları (kısa)

| Grup | Araçlar |
|---|---|
| Keşif | `refl_list_components`, `refl_describe_component`, `engine_capabilities`, `docs_search` |
| Okuma | `scene_info`, `scene_list_entities`, `scene_get_entity`, `scene_find`, `scene_stats`, `asset_list`, `asset_read`, `asset_used_by`, `asset_thumbnail` |
| Sahne düzenleme | `entity_create` (bileşenlerle, `prefab` ile örnek), `entity_delete/duplicate`, `component_add/remove/set` (`path` ile özellik yolu: `position.x`, `items[2]`), `scene_clear`, `scene_undo/redo`, `scene_new/open/save/switch/documents`, `scene_diff`, `scene_lint(_fix/_all)` |
| Prefab | `prefab_create_from`, `prefab_apply`, `prefab_revert` |
| Varlık | `asset_schema`, `asset_create`, `asset_write`, `asset_set_import_settings`, `asset_import` (aseprite/tiled/ldtk/gltf/fbx), `asset_delete`, `cook_run`, `cook_errors`, `tilemap_paint` |
| Dünya | `terrain_*`, `roads_generate/roads_info`, `navmesh_bake`, `hlod_bake`, `procgen_run` |
| UI/Zaman/AI/2D iskelet | `ui_*`, `timeline_*`, `bt_*`, `perception_debug_lines`, `skel2d_*` |
| Doğrulama | `run_headless` (ticks, input, inspect, trace, convars, scene), `test_run`, `playtest` (bot), `debug_lines`, `debug_view` |
| Görme | `render_frame`, `render_sheet`, `timeline_preview`, `ui_preview`, `image_diff`, `view_*`, `player_*` |
| Ölçüm | `perf_run`, `perf_compare` |
| Kod | `module_build` (yeniden yükler), `module_test` |
| İçerik üretimi | `gen_palette/sprite/pixels/mesh/tileset/texture/sfx/music`, `library_search`, `library_add` |
| Gönderim | `project_check`, `ship_build`, `project_get/set`, `scene_settings_get/set` |
| Takım | `project_watch`, `asset_lock/unlock/locks`, `task_*`, `report_friction`, `console_list/exec` |

## Tipik kalıplar

- **Hızlı sahne yazma**: bir bileşen nesnesiyle `entity_create {"name":"Crate","components":{"LocalTransform":{"position":[0,1,0]},"MeshRef":{...}}}`. Tekrarlayanlar için `entity_duplicate` + `component_set LocalTransform position`.
- **Waypoint zinciri son→ilk** yarat (her `next` var olan entity'yi göstersin), döngüyü `component_set` ile kapat.
- **Girdi betiği**: `input: "10-70 Move=1,0 Jump\n100 Fire"` (veya segment dizisi). Çok oyunculu `P2/Move`. Oyun menüyle açılıyorsa önce `10 Confirm` ile menüyü geç.
- **Sayısal kanıt**: `run_headless {"ticks":600,"inspect":{"entities":["Player"],"show":["Health3D","Vehicle3D"]}}`; zaman serisi için `trace:{every:10,fields:[{entity:"Player",field:"LocalTransform.position"}]}`. Motorun yazdığı alanlar da gelir (`NavAgent3D.state`, `Vehicle3D.speed/gear`, `Ragdoll3D.settled`).
- **Test** (`tests/*.mctest`): senaryo `scene`, `ticks`, `bot`/`input`, `until`, `death`, `assert[]` (alan, `alive`, `event`, `count`, `script`, `log_contains`, `log_errors`, `feel`). Botlar: `walk_to`, `explore`, `masher`; `hold`, `hold_when`, `stop_when`. Düzlem `xy` | `x` | `xz` (3D ileri −Z). `mce test <proje>` terminalden çalıştırır. Prefab örneklerinden SONRA gelen entity adı testte reddedilir (cook prefab'ı genişletir): test edilen entity'leri prefab örneklerinden önce yaz.
- **Eski GUID/değer**: `asset_list`, `asset_used_by` (silmeden önce).
- **Çok ajan**: aynı proje klasöründe ayrı sunucular; `asset_lock` ile kilitle, `project_watch` ile izle, `task_claim` ile iş al. İki ajan aynı sahneyi kaydederse entity/bileşen bazında birleştirir, çakışmada `resolve: ours|theirs`.

## Cook hatası okuma

`cook_run` → `ok:false` + `file:line:col`. İlk hatayı oku: sidecar yok → `adopt`; bilinmeyen bileşen/alan → `module_build`; kırık referans → GUID düzelt. `run_headless/render_frame/ui_preview/timeline_preview` bütün projeyi cook eder: tek hatalı dosya hepsini durdurur. Çalıştıramadığını kenara al: `cook_run {"paths":[...]}` veya `{"skip_errors":true}` (pack yazmaz).

## Sınırlar

Çalışan editöre bağlanılamaz; uzun araç çağrısı iptal edilemez; tilemap/terrain boyama MCP'de var ama shader/özel render geçişi yok (yerleşik shader'lar projeden değiştirilemez).
