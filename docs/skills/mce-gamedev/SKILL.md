---
name: mce-gamedev
description: Mesh Control Engine (mesh_control_engine, MCE) ile oyun geliştirme rehberi. Proje kurma, sahne/prefab/UI/animasyon formatları, kit bileşenleri, MCP araçlarıyla motoru sürme, cook, mce_player ile çalıştırma ve doğrulama, Blender MCP ile model üretimi, bütçeler, determinism ve CI kuralları. Bu motorla oyun/örnek/sahne yazarken veya motorun kendisine katkı verirken kullan.
---

# mce-gamedev: Mesh Control Engine ile oyun geliştirme

Motor: C++20, sıfırdan yazılmış 2D/3D motor + editör + MCP sunucusu. Repo `github.com/sberkan09/mesh_control_engine` (PR'lar `master`'a). Dünya sağ elli, **Y yukarı, metre, saniye, derece**; 2D sahneler XY düzleminde (+Y yukarı). Sabit 60 Hz simülasyon, deterministik (`state_hash`).

Kullanıcı Türkçe yazar: Türkçe yanıtla, gerekmedikçe soru sorma, karar ver ve ilerle.

## Önce bunu oku: karar ağacı

1. **Oyun/sahne/içerik yapıyorsun** (en yaygın): aşağıdaki "Çalışma döngüsü"nü izle. Kod yazmadan çoğu şey veridir (`.mcscene`, `.mcprefab`, `.mcui`, `.mcanim`, `.mcmat`...).
2. **Yeni bir bileşen/sistem gerekiyor** (veride karşılığı yok): `docs/skills/mce-gamedev/references/engine-contribution.md` (C++ modülü ve motor CI kuralları).
3. **3D model/doku lazım**: `docs/skills/mce-gamedev/references/blender-and-content.md`. Önce Blender MCP / headless `bpy` ile üret, glTF (`.glb`) olarak dışa aktar, cook'a ver. Kenney/CC0 kaynaklar ve `gen_*` araçları orada.
4. **Bir şey bozuldu**: `docs/skills/mce-gamedev/references/pitfalls.md`.

Referans dosyaları repoda `docs/skills/mce-gamedev/references/` altındadır (gerektikçe oku, hepsini baştan okuma; checkout yoksa `github.com/sberkan09/mesh_control_engine` `master`'dan GitHub MCP `get_file_contents` ile çek). Aşağıdaki yollar bu klasöre görelidir:

| Dosya | İçerik |
|---|---|
| `docs/skills/mce-gamedev/references/formats.md` | `.mceproj`, `.mcscene`, `.mcprefab`, `.mcui`, `.mcanim`, `.mcmat`, `.mcpart`, `.mcseq`, `.mcinput`, `.mcitems`, sidecar `.mceasset`, değer yazımı |
| `docs/skills/mce-gamedev/references/mcp-tools.md` | MCP araçları grup grup, tipik akışlar, `.mctest` ve botlar |
| `docs/skills/mce-gamedev/references/kit-components.md` | Veri olarak kullanılan kit bileşenleri (2D, 3D, araç, polis, ses, UI, hava...) ve GTA tarzı örnekten çıkan kurallar |
| `docs/skills/mce-gamedev/references/blender-and-content.md` | Blender MCP/bpy, glTF kuralları (ölçek, eksen, isim), CC0 kaynaklar, `mce content`, `gen_*` |
| `docs/skills/mce-gamedev/references/run-and-verify.md` | `mce_player`, headless, ekran görüntüsü, Linux/macOS/Android, perf |
| `docs/skills/mce-gamedev/references/performance-determinism.md` | Bütçeler, determinism kuralları |
| `docs/skills/mce-gamedev/references/recipes.md` | Hızlı tarifler |
| `docs/skills/mce-gamedev/references/pitfalls.md` | Sık hatalar ve çözümleri (tek tablo) |
| `docs/skills/mce-gamedev/references/engine-contribution.md` | Motor/CI/PR kuralları (CLAUDE.md özeti) |

## Çalışma döngüsü (MCP ile)

```
kur → keşfet → içerik → yaz (sahne) → kaydet → cook → doğrula (headless) → GÖR (render_frame) → test (.mctest) → ship
```

1. **Kur**: SDK ile `mce new MyGame` (proje klasörü + `.mcp.json`), checkout'ta `python3 scripts/new_project.py MyGame` sonra `cmake --preset linux-gcc-debug` ve `--target my_game my_game_mcp`. Önce `mce build MyGame`; MCP sunucusu modülün bileşenlerini ister.
2. **Sunucu**: `mce mcp MyGame` (stdio JSON-RPC; stdout'a başka bir şey yazılmaz). Stok `mce_mcp`'yi `--module` olmadan çalıştırma: oyunun bileşenlerini tanımaz. Yeni bileşen eklediysen `module_build` (yeniden başlatmadan yükler).
3. **Keşfet**: `engine_capabilities`, `docs_search "<konu>"`, `refl_list_components`, `refl_describe_component <Ad>`. Alan adlarını tahmin etme, refl'den oku.
4. **İçerik**: dosyayı `assets/` altına koy, `cook_run {"adopt": true}` (sidecar yazar), `asset_list` ile GUID al. Normal map/LUT/maske için `asset_set_import_settings {"settings":{"srgb":false}}`. JSON kaynakları (`.mcpart`, `.mcui`, `.mcanim`, `.mcmat`...) elle yazma, `asset_schema` + `asset_create`/`asset_write` kullan (cook'ta doğrulanır).
5. **Sahne**: `scene_open` → `entity_create` (bileşen nesnesiyle) → `component_set` → **isimlendir** → `scene_save`. `run_headless` yalnızca **kaydedilmiş** sahneyi oynatır.
6. **Cook ve doğrula**: `cook_run`, sonra `run_headless {"ticks":600,"inspect":{...}}`. `state_hash`'i karşılaştırarak değişikliğin bir şey değiştirdiğini kanıtla.
7. **Gör**: her görsel değişiklikten sonra `render_frame` (tick, input, camera ile belirli an). Bir kare yetmezse `render_sheet`. Düzgün görünen ekran için `image_diff {update:true}` ile golden al.
8. **Test**: `.mctest` (bot + assert) yaz, `test_run`; terminalden `mce test <proje>`.
9. **Ölç ve gönder**: `perf_run {"target":"desktop"}`, `project_check`, `ship_build`.

Sürekli geri bildirim: motor bir şeyi zorlaştırıyorsa (eksik araç, yanıltıcı hata, yanlış doküman) çözümden önce `report_friction`.

## Altın kurallar

- **Veri önce.** Önce bileşen+sahne+Luau betik (`.luau`) ile çöz; C++ yalnızca sahnede ifade edilemeyen mantık için.
- **İsimlendir.** Entity adları benzersiz ve anlamlı olsun; MCP her yerde id yerine ad kabul eder. `scene_open`'dan sonra id'ler değişebilir, toplu işten önce `scene_list_entities`.
- **Kaydet → cook → oyna.** MCP `cook_run` `cooked/` yazar; oyun yürütülebilirinin okuduğu `assets.mce` paketi için `cook_run {"pack":true}` veya `mce build`.
- **Her şeyi doğrula, tahmin etme.** Headless `inspect` ile sayısal kanıt, `render_frame` ile görsel kanıt. Ekran görüntüsü olmadan "görsel olarak düzeldi" deme. Yazılımsal rasterizer (bulut) süre ölçmez; sayaçlara (draws, triangles) bak.
- **Determinism'i bozma**: sim'de duvar saati, `std::rand`, sırasız container yok. Rastgelelik tohumlu (bileşende `seed`). Ayrıntı: `docs/skills/mce-gamedev/references/performance-determinism.md`.
- **Bileşen düzeni**: `Transient` alanlar sahneye yazılmaz; yeni bileşenlerde padding yok (`u8 pad_[N]` + `static_assert(sizeof)`), çünkü bileşen baytları hash'lenir.
- **Lisans**: yalnızca CC0 ve CC-BY içerik (NC/SA/ND yok). Kaynağı `CREDITS.md`'ye yaz (`gen_*`, `library_add`, `mce content add` otomatik yazar).
- **Mac'i Remote Control ile kullanma**, kullanıcı o oturum için açıkça izin vermedikçe. Cloud'da (Linux, Mesa yazılım GL) çoğu şey doğrulanır; gerçek GPU kare hızı, ses ve fare hissi doğrulanamaz: bunu raporda açıkça söyle.
- **Dokümantasyon ve changelog**: kullanıcıya görünen her özellik için ilgili guide sayfası, milestone dosyası ve `changelog.d/<BİLET>-<n>.md` (CHANGELOG.md'ye dokunma). Ayrıntı `docs/skills/mce-gamedev/references/engine-contribution.md`.

## Hızlı başlangıç: sıfırdan küçük bir 3D sahne

```
mce new Arena && mce build Arena && mce mcp Arena          # veya .mcp.json ile Claude Code'a kaydettir
gen_mesh {shape:"humanoid", save:"models/hero.glb"}        # model yoksa blokaj için
library_add {id:"<kenney-city-id>"}                         # CC0 hazır modeller (library_search ile bul)
scene_new → entity_create (Camera3DComponent, Environment3D, MeshRef, BoxCollider3D ...) → scene_save
render_frame {}                                            # gör
run_headless {ticks:300, inspect:{...}}                    # kanıtla
```

Örnek kaynak: `samples/` (özellik başına bir örnek), `projects/` (tam oyunlar), `docs/user-guide/18-samples.md`. En büyük 3D örnek: `samples/09_openworld` (şehir, trafik, yaya, polis, wanted, görev, HUD).

## En sık 12 tuzak (ayrıntı: `docs/skills/mce-gamedev/references/pitfalls.md`)

1. Stok `mce_mcp`/`mce_cook` `--module`'süz oyunun bileşenlerini tanımaz: `mce build`, `module_build`, gerekirse sunucuyu yeniden başlat.
2. Yeni dosya sidecar'sız cook'ta düşer: `cook_run {"adopt":true}`; normal/ORM/LUT dokularına ilk cook'tan önce `"srgb":false`.
3. MCP `cook_run` `cooked/` yazar, oyun `assets.mce` okur: `cook_run {"pack":true}` veya `mce build`.
4. `run_headless`/`render_frame` yalnız **kaydedilmiş** sahneyi oynatır; `scene_open` sonrası id'ler değişir, ad kullan.
5. glTF yalnız `.glb` (tek dosya), Draco yok, doku ayrı PNG; birim metre, +Y yukarı, −Z ileri, scale Apply.
6. Bileşenlerde padding yok (`pad_` + `static_assert(sizeof)`), sim'de duvar saati/`rand` yok; aksi halde `state_hash` oynar.
7. `UiScreen` bağları yalnız sayısal alanları okur ve ekranın entity'sinde bileşen yoksa sahnedeki ilk entity'ye bakar; font yalnız ASCII (★ yerine `*`).
8. Ses bileşenini üreten sistem şart; ses kök entity'de olmalı (`audio_bridge` kendi `LocalTransform`'unu okur).
9. Yaya/trafik görünmüyorsa sistem sırası (`kit3d_bodies`'ten sonra) ve kamera yönü; headless entity sayısı doğanları saymaz.
10. Üretilen sahneyi (`gen_assets.py` çıktısı) elle düzenleme; betiği değiştir, yeniden üret.
11. Yeni bileşen = 3 yerde `#include`, `capabilities.py`, `sample_coverage.allow`, transcript golden'daki `components` sayısı, `test_padding`, `changelog.d/<BİLET>-<n>-<slug>.md`.
12. CI: `--task` bir kez ve detached, `--premerge` ile aynı anda değil; mesaj gelince konteyner yeniden başlar (`uptime -s`, `--resume`); ccache'i silme.
