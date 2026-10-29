# Hızlı tarifler

Hepsi MCP araç sırasıdır. Ayrıntılı üç tarif: `docs/user-guide/recipes/`.

## 1. Bot'un bitirebildiği seviye
`scene_new` → `entity_create` ile zemin, oyuncu, hedef (`Flag`), engeller → `scene_save` → `playtest {bot:{kind:"walk_to",actor:"Player",target:"Flag",plane:"x"}}` → feel raporu (`finished`, `stuck_spots`) → `render_frame` ile takıldığı yere bak → `tests/reach_flag.mctest` yaz → `test_run`.

## 2. Yeni bileşen + sistem (canlı)
`components.h`: `struct Patrol{f32 speed=2; f32 range=3; ... u32 pad_=0;}; MCE_COMPONENT(Patrol, speed, range, (time,Transient))`. `my_game.cpp`: sistemi yaz (`f.query<Patrol&,LocalTransform&>().each(...)`, tohumlu rastgele, duvar saati yok), `install()`'de `register_components<Patrol>()` + `add_system(Phase::FixedUpdate_Pre,"patrol",&patrol)`. `module_build` (hata `{file,line,col}`) → `component_add` → `scene_save` → `run_headless` + `trace` → `module_test`.

## 3. HUD (can, mermi, nakit, wanted yıldızları, minimap)
`ui_create "ui/hud.mcui" modal:false` → `ui_widget_add` ile panel/text/minimap widget'ları → bağlamaları ekle (`Wanted3D/level` gibi bileşen alanı yolları, envanter için `slot_N` adları) → sahnede `UiScreen{document:"guid:.."}` entity → `ui_preview` (960x540 ve `safe_area`) → `render_frame` oyun içinde. Metinler 16 px üstü, kontrast 4.5:1. Örnek: `samples/10_missions/assets/ui/hud.mcui`.

HUD bağları: metin `{"kind":"Text","bind":"Health3D/current","format":"HEALTH %.0f"}` (format yalnız bir `%f/e/g`); koşullu görünüm: `bind`'lı Panel/Image `range:[lo,hi,step]`'in `lo`'sunda ve üstünde görünür (başlangıçta `"visible":false`); wanted yıldızı = 5 Panel, `lo`=1..5, içinde Text `*`; minimap `{"kind":"Minimap","minimap":{"track":"Player","radius":60}}` ve varlığa `MapMarker{color,label,always_visible}`; nakit için oyuncuya `Wallet3D`; mermi `Weapon3D/ammo` ve `reserve`; `Wanted3D/level` 0-5. Ekran görüntüsü: `xvfb-run -a -s "-screen 0 1280x720x24 -noreset" mce_player -pack x.mce -scene scenes/city.mcscene -frames 120 -capture x.ppm -platform.error_box 0 -r.vsync 0`, sonda "0 GL errors" yazmalı. Durumu hızlı denemek için proje klasörünün scratch kopyasında sahne JSON'unu yamala (`"Wanted3D":{"heat":70}`) ve yeniden cook et. Ayrıntı: `docs/user-guide/12-ui-and-console.md`, `docs/architecture/ui.md`, `samples/ui_screens`, `src/engine/ui/src/ui.cpp` (sync_properties).

## 4. Ses ekleme
Ses dosyalarını `assets/audio/`'ya koy (`adopt`), döngü sesleri `pcm`, uzun yatak `vorbis`; `.mcsound` seti (çoklu klip, pitch/gain aralığı) → kök entity'ye `AudioSource`/`EngineAudio`/`WeaponAudio`/`FootstepAudio`/`SirenAudio`; `AudioListener` kamerada; `AudioSnapshot`/`AudioDuck` ile mix. Doğrulama: `mce_player -audio_record out.wav` (çevrimdışı mix; uzunluk, tepe, hash basar). Bulutta kulakla duyamazsın: raporda söyle.

## 5. Polis / wanted döngüsü
`Wanted3D` (oyuncuda) + `Police3D` ajanları (`NavMeshSurface3D` + `navmesh_bake`) + `drive_car` ile polis arabaları (`Vehicle3D` + yol takibi `RoadNetwork3D`) + `Roadblock3D` + `Respawn3D`/`RespawnPoint3D` (hastane/karakol). Sahne `gen_assets.py` ile üretilir (elle düzenleme). MCP testi örneği `tests/mcp/test_mcp_openworld.cpp` (`component_set` + `scene_save` + `run_headless`; `run_headless` kaydedilmiş sahneyi baştan oynatır, her çağrı sıfırdan başlar, `input:"20-80 Fire"`, `inspect`). Yerelde `ninja <hedef>`, `xvfb-run -a ctest -R <ad>`, hızlı kapı `local_ci.sh walls`. Test: `component_set` ile suç tetikle, `run_headless` + `inspect` ile `Wanted3D.level` ve polis sayısı, `bt_trace`/`debug_view` ile yol. Kaynak: `docs/milestones/{M-ARREST,M-OPENWORLD-POLICE}.md`, `docs/user-guide/29-roads-and-traffic.md`, `kit/{wanted3d,roadblock,effects,roads}.h`.

## 6. Trafik ve yayalar
`roads_generate {columns:5,rows:5,block:100}` → `roads_info` (`ok:true`, `no_way_on`/`cut_off` boş) → `RoadNetwork3D` + `TrafficSpawner3D` + `CrowdSpawner3D` (prefab ile) → `run_headless` 600 tik, `inspect` ile canlı araç sayısı → `render_frame` iki tikte (300, 700). Görünmüyorsa kamera + sistem sırası (pitfalls).

## 7. Gün/gece ve hava
`WorldClock` (başlangıç saati, gün uzunluğu) + `DayCycle` (güneş, ortam, sis) + `TimedLight3D` (lamba/pencere akşam yansın) + `Weather` (+`Precipitation3D`, `Lightning`, `WindDirection`). `render_sheet` ile birkaç saati yan yana gör. Düşük kalitede kapanan katmanları (`r.nightsky`, `r.water`...) kontrol et.

## 8. Karakter + animasyon
Blender'da iskeletli karakter (`Idle/Walk/Run/Jump` klipleri, kemik adları sabit) → `.glb` → `MeshRef` + `Animator{graph}` + `Locomotion3D{stride_speed}` + `CharacterController3D`. `.mcanim` blend state `speed` parametresiyle. Klipler başka dosyadan: `animations_from` / `mce retarget`. `render_sheet` ile yürüyüş döngüsünü gör.

## 9. Küçük görev
`Objective` zinciri (`ReachZone`, `next`) + `Mission` + `MissionMarker` + `MissionReward3D(cash, fail_fee)` + `MissionRetry` + `MapMarker`. Luau ile zincir: `mission.*` API. Test: `component_set` ile oyuncuyu hedefe koy, `scene_save`, `run_headless` ve `inspect Mission`.

## 10. Prefab düzeni
`prefab_create_from {entity:"Grunt",path:"prefabs/grunt.mcprefab"}`; örnek: `entity_create {name:"Grunt B",prefab:"guid:..",components:{LocalTransform:{position:[4,0,0]},Health3D:{hp:5}}}`. Çalışma zamanında `ecs::instantiate` (override: `PrefabOverride::of("Health.hp",99.0f)`).

## 11. Ekran koruması (golden)
Her ana ekran için `render_frame {save:"shots/title.png"}` → `image_diff {golden:"shots/title.png", update:true}`; sonra her değişiklikte `image_diff` `pass`. Eşik: `score≤0.01`. GPU farkı için yalnız aynı arka uçta karşılaştır.

## 12. Performans tur
`perf_run {target:"desktop", frames:300}` → `top_systems`/`gpu_stages` → tek değişiklik → `perf_run` → `perf_compare`. Mobil için `{target:"mobile"}` ve `project_check {target:"mobile"}` (doku boyutu, sprite doku sayısı).

## 13. Gönderim
`project_check` → `ship_build` (veya `mce ship MyGame --version 1.0 [--textures bc] [--max-texture-size 1024]`) → `mce content verify` (lisans kapısı) → zip. Mobil: `ship.py --target android|ios`; web: `--target web`.

## 14. Luau ile veri-yalnız oyun
`.luau` betiği (`script_check`), `ScriptSystem` + `ScriptVars/ScriptParams`; kod yazmadan Pursuit/Dungeon örnekleri gibi. Betik adım bütçesi `.mctest` `script` assert'iyle korunur.
