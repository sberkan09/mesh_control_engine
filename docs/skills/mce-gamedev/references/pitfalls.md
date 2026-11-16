# Sık hatalar ve çözümleri

Kaynaklar: user-guide 14.4 ve 17, MCP dogfood/blind test notları (`docs/research/`), ve motorla oyun geliştiren thread'lerin (açık dünya HUD/ses/polis/yaya, görsel, rutinler, M-MONETIZE) bildirdikleri.

## İçerik ve cook

| Belirti | Neden | Çözüm |
|---|---|---|
| `unknown component type 'X'` / `unknown field` | stok araç `--module`'süz ya da modül kaynaktan eski | `mce build`/`module_build`; `<game>_mcp`'yi yeniden başlat; `types.cpp` bileşen başlığını include etmeli |
| `unregistered asset (no .mceasset sidecar)` | yeni dosya sidecar'sız | `cook_run {"adopt":true}` / `mce cook --adopt` |
| Magenta dama | doku yüklenemedi / GUID bayat | log varlığı söyler; yeniden cook; `srgb`/biçim |
| Normal map/LUT/ORM yanlış, parlak | sRGB adopte edildi | `asset_set_import_settings {"settings":{"srgb":false}}` **ilk cook'tan önce** |
| Oyun eski sahneyi oynatıyor | MCP cook `cooked/` yazar, oyun `assets.mce` okur | `cook_run {pack:true}` / `mce build` |
| glTF "external buffers are not supported" | `.gltf`+`.bin` | `.glb` ya da gömülü base64 buffer; doku ayrı PNG kalır |
| Draco hatası | `KHR_draco_mesh_compression` | Blender'da Draco kapat; veya `gltfpack -c` ile meshopt |
| Model devasa/ minicik | birim/eksen | sidecar `scale`, OBJ `up`; Blender'da Apply Scale + Unit 1.0 + dışa aktarıcıda +Y Up |
| `asset_write` "format, version and guid are set by the tool" | bunları `content`'e yazdın | çıkar |
| Sahnede alan hâlâ varsayılan | alan adı eşleşmiyor / `Transient` yazıldı | `refl_describe_component`, cook uyarısına bak |
| "re-cook" / `InvalidArgument` yüklemede | bileşen yerleşimi değişti | yeniden cook (build yapar) |
| Sabit dizi hatası "expected an array of 8 elements" | `CameraRail3D.points` gibi dizi kısa | tam uzunlukta yaz |
| `.mctest` prefab sonrası entity adı reddedilir | cook prefab'ı genişletir, indeksler kayar | test edilen entity'leri prefab örneklerinden önce yaz |
| Sahne kaydı "refuses to overwrite" | başka dosyaya `force` | yeni yola kaydet (taze GUID) ya da `scene_open`+`scene_clear` |

## MCP

| Belirti | Neden | Çözüm |
|---|---|---|
| Düzenlemeler yanlış entity'lere gitti | `scene_open` sonrası id'ler değişti | ad kullan; önce `scene_list_entities`; `scene_undo` |
| `run_headless` reddediyor | kaydedilmemiş değişiklik | `scene_save` |
| Yeni bileşen `cook_run`'ı bozuyor | sunucu eski tablo | `module_build`; gömülü `<game>_mcp` ise yeniden derle+başlat |
| `console_exec time_scale` reddedildi | hile convar | `--allow-cheats` |
| Başlık menüsünde takılı kare/test | oyun menüyle açılıyor | `input:"10 Confirm"` ile geç ya da menüsüz sahne |
| Araç şemaları yok / GitHub-Linear-MCP koptu | bağlantı düşer | `ToolSearch select:<araç>` ile yeniden yükle |
| İki ajan aynı dosyayı yazıyor | son yazan kazanır (malzeme vb.) | `asset_lock`; sahnede otomatik birleştirme + `resolve` |

## Fizik / oyun

| Belirti | Çözüm |
|---|---|
| Gövde `LocalTransform` yazımını yok sayar | fizik poz sahibidir: `teleport` veya hız |
| `has_body` ilk tikte false | gövdeler ilk adımda yaratılır; ilk tik mantığını koru |
| Sensör her şeye tetikler | `mask`'ı oyuncu katmanına ayarla |
| Yaya/trafik bir türlü görünmez, yerinde kalır | sistem sırası: kalabalık `kit3d_bodies`'ten sonra çalışmalı; kamera yönü; headless entity sayısı doğan olanı saymaz |
| Kinematik karakter yer altında/titreme | kapsül ile `CharacterController3D`, yer katmanı; hareketli platformda kinematik hız kullan |
| Ölünce/tutuklanınca oyun biter | `RespawnPoint3D` + `Respawn3D` tanımla |
| Ses çıkmıyor | bileşeni üreten sistem var mı; ses **kök** entity'de mi; bus; `-audio.enabled`; macOS ses izni |
| Olay testte "geç" geliyor | olaylar test dünyasında bir tick sonra görünür: assert'i tik payıyla yaz |
| Şeffaf/karışık nesneler HLOD'dan düşüyor | HLOD yalnız opak/maskeli görünür rigid mesh'leri birleştirir |
| `UiScreen` bağı çalışmıyor / yanlış entity'yi okuyor | bağlar (`Bileşen/alan`) yalnız **sayısal** alanları okur; ekranın kendi entity'sinde bileşen yoksa sahnedeki **ilk** entity'ye bakar | bileşeni ekranın entity'sine koy; `Weapon3D` kamerada, `Health3D` oyuncuda ise iki ayrı `.mcui` + iki `UiScreen`. Metin/string alanları bağlanamaz |
| HUD'da ★ ve Türkçe/Unicode glif görünmüyor | yerleşik font yalnız ASCII (32-126) | `*` kullan; Unicode için `font` sidecar'ında `ranges` ile TTF göm (OFL, Türkçe kapsamlı temalar şablonda) |
| HUD editörde ekran dışı | editör görünümü viewport boyutundadır; oyun penceresinde bak |
| UI küçük (1080p) | `ui.reference_height 720` |
| İskinsiz Blocky karakter kayıyor | gerçek iskeletli karakter + `Locomotion3D` + `.mcanim` |

## Geliştirme süreci (repo içinde)

| Belirti | Çözüm |
|---|---|
| Yeni bileşen sonrası CI kırmızı (`mcp_transcript`, capabilities) | 3 yerde `#include` (editor core, mcp app, tools/cook `engine_components.cpp`); `docs/engine/capabilities.md` (`python3 scripts/ci/capabilities.py`, sonra `--check`); `sample_coverage.allow` satırı **veya** örnekte kullan (örneğe girince allow satırını sil); `tests/mcp/transcripts/demo_edit.golden.jsonl` içindeki `"components": N` artır (`mce_mcp` ile gerçek sayıyı bul); `docs/user-guide` ve `docs/engine/kit.md` anlatımı; `test_padding` |
| `-Werror`: `-Wshadow`, `-Wshorten-64-to-32` | tam sayı daraltmayı açık yap |
| clang-tidy (`-warnings-as-errors`) | `size()>0` yerine `empty()`; struct üzerinde `memcmp` yasak; `bugprone-integer-division`: tamsayı bölmeyi kendi `i32` değişkeninde yap. Yerelde `clang-tidy-18 -p build/<dir> <dosya.cpp>`, `clang-format-18 --dry-run` |
| ECS'te `w.get<T>` yok | `try_get` / `try_get_mut` |
| Reflection bayrakları | `ReadOnly`, `Transient` (sahneye yazılmaz), `Save` (save game'e girer) |
| `changelog.d` adı reddedildi | `<BİLET>-<n>-<slug>.md`, bilet adı harf+rakam tek parça (`MISSIONFEE-1-fee.md`), ilk satır `- ` ile başlar |
| `git fetch` yavaş | arka planda çalıştır; `master` sık ilerler: PR'dan önce `git fetch origin master` + merge. Çakışma genelde `gen_assets.py`, `city.mcscene`, docs'ta: betiği elle birleştir, sahneyi betikle yeniden üret |
| `local_ci --task` ile `--premerge` birlikte çalışıyor, sahte SegFault (test_module/test_mcp) | iki koşu aynı build ağacını ezer | aynı anda çalıştırma; tek başına geçer |
| Soğuk derleme saatler sürüyor | ccache boş | proje dosyalarındaki anlık görüntüyü aç: `tar -I zstd -xf /mnt/project-files/.cache/ccache-linux.tar.zst` (ccache dizinine); ccache'i silme |
| `git status`'ta `projects/*/.mce/` | CI artığı | commit'leme, `git clean` ile sil |
| Sahne dosyasını elle düzenledin, merge'de ezildi | `city.mcscene` gibi sahneler `gen_assets.py` ile üretilir | betiği değiştir, sahneyi yeniden üret |
| Container mesajla yeniden başladı, `--task` öldü | her uyanışta `uptime -s`; `--task --resume` |
| Golden PNG'ler | dev-only (CI bakmaz), `MCE_UPDATE_GOLDENS=1` ile yazılır; mevcut golden'lar byte-aynı kalmalı |
| PassU uniform bloğu | 256 B katı kalmalı |
| Fuzz testleri | `label fuzz`, `MCE_FUZZ_ITERS`; `tests/fuzz/mutator.h`, `json_swap.h`; bulunan hatayı düzeltince aynı PR'a regresyon testi |

## Oyun mantığı yazarken (polis/wanted/yol takibi işinden)

| Belirti / kural | Çözüm |
|---|---|
| Faz içinde `world.spawn_with` assert eder | `f.commands().spawn` kullan (yapısal değişiklik Commands ile) |
| Sorgu yürürken yapı değişimi | istekleri topla, yürüyüşten sonra uygula |
| Önbellek sonucu save/load ve replay'i ayrıştırıyor | önbellek saf olmalı: çıktısı yalnız duruma bağlı (yol takibinde `road_route`, önbellekte yalnız A*) |
| Çalışma anında doğan varlık kaydedilmiyor | `SaveKey` taşımaz; kalıcı olması gereken varlığı sahnede yarat |
| Fizik gövdeli çocuk ebeveyni izlemiyor | fizik gövdeleri `ChildOf`'u yok sayar; gövdeyi kök yap |
| Wanted seviyesi beklenenden farklı | seviye `ceil(heat/...)`: heat 25/20 seviye 2'ye denk gelir; `inspect` ile `Wanted3D` oku |
| Spawn çakışıyor / araç iç içe doğuyor | spawn öncesi boşluk kontrolü yap, başarısızsa bekle (sonraki tikte dene) |
| Araç kamerası titriyor | kamera hızını konum farkından hesapla, `target` değişince sıfırla, eşiklerde histerezis kullan |
| Transcript golden'da `components` sayısı master ile çakışıyor | master'daki değeri al ve kendi eklediğin kadar artır |
