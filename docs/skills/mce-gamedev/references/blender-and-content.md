# Model üretimi: Blender MCP, glTF kuralları ve içerik kaynakları

## Neden Blender

Motorun kendi üreticileri (`gen_mesh`: box, plane, sphere, cylinder, cone, disc, ramp, humanoid; `gen_texture`, `gen_sprite`, `gen_sfx`, `gen_music`) blokaj ve prototip içindir. Düz kutu şehirler "oyun değil, dilim" görünür (bkz. `docs/milestones/M-OPENWORLD-GAPS.md` B bölümü). **Gerçek model gerektiğinde önce Blender ile üret** (Blender MCP veya headless `bpy`), glTF olarak dışa aktar, cook'a ver. Elle vertex JSON'u yazmaya veya her şeyi kutuyla çözmeye çalışma.

Sıra: (1) ihtiyaç zaten CC0 pakette var mı (`library_search`, `mce content list`)? (2) yoksa Blender'da modelle/kitbash et; (3) `.glb` dışa aktar; (4) `assets/models/` altına koy, `cook_run {adopt:true}`; (5) `meshes` özetini kontrol et; (6) `render_frame` ile gör.

## Blender'a nasıl erişilir

1. **Blender MCP (etkileşimli)**: kullanıcının bilgisayarında Blender + MCP eklentisi açıksa ve bu oturum o bilgisayara bağlıysa araçlar `mcp__remote-devices__Blender__*` adıyla görünür (ToolSearch ile yükle): `execute_blender_code` (Blender'ın Python'unda `bpy`, sonucu `result` değişkenine JSON olarak ata), `get_objects_summary`, `get_object_detail_summary`, `render_viewport_to_path`, `get_screenshot_of_*`, `search_api_docs`, `get_python_api_docs`, `get_blendfile_summary_*`. Tipik akış: kod çalıştır → `get_objects_summary` ile doğrula → `render_viewport_to_path` ile bak → dışa aktar.
2. **Headless `bpy`**: `blender -b --python gen_model.py -- <args>` (veya `execute_blender_code_for_cli`). Betiği repoya koy (örnek yer: `samples/<örnek>/tools/`), böylece model yeniden üretilebilir. Cloud konteynerinde Blender genelde **yoktur** (`which blender`); kurulum mümkünse `apt`/pip `bpy` dene, yoksa cihaz tarafında çalıştır. **Kullanıcının Mac'ine Remote Control ile bağlanma** (proje kuralı), Blender'ı oradan sürmek için kullanıcıdan o oturum için açık izin al.
3. Hiçbiri yoksa: `gen_mesh` + CC0 paket + `.obj` yaz (düz metin, `up`/`scale` sidecar'da) ve bunu raporda belirt.

## glTF/glb dışa aktarma kuralları (motor tarafı)

Kaynak: `docs/user-guide/08-3d.md` 8.4. Her şeyi cook özetiyle doğrula: `mesh assets/models/x.glb: N tris, N submeshes, N materials, N joints, N clips, N LODs, N warnings`.

| Konu | Kural |
|---|---|
| Biçim | **`.glb`** (tek dosya). `.gltf` + harici `.bin` cook hatası. Draco **yok** (meshopt serbest: `gltfpack -c`) |
| Eksen | Motor: sağ elli, **+Y yukarı**, **−Z ileri** (kamera/karakter). Blender Z-yukarı: dışa aktarıcıda **+Y Up** açık olsun. FBX dosyası eksen/birimi kendisi söyler (cook çevirir); OBJ'de sidecar `"up":"z"`. Karakterin yüzünü `render_frame` ile doğrula, ters bakıyorsa Blender'da 180° döndür (gen_mesh humanoid −Z'ye bakar) |
| Ölçek | 1 birim = **1 metre**. Blender'da Unit Scale 1.0, nesne ölçeğini **Apply** (Ctrl+A) et; insan ≈1.8 m, kapı ≈2.1 m, araç ≈4.5 m. Santimetre ise sidecar `"scale":0.01`. Yanlış ölçek en sık sorun: `render_frame` ile tanıdık bir nesneye (oyuncu) göre karşılaştır |
| Pivot | Yer nesneleri için taban ortası, kapılar için menteşe; origin sahnede `LocalTransform` konumu olur. Rotasyon/ölçek Apply edilmiş olsun |
| Üçgen sayısı | Yalnız üçgen listesi (dörtgenleri üçgenle; FBX/OBJ n-gon'lar dışbükey kalsın). Sidecar `"max_triangles"` ile bütçe zorla. Bütçe: bkz. performance-determinism.md |
| UV | **Tek UV seti** (`TEXCOORD_0`); `TEXCOORD_1`, vertex rengi (`COLOR_0`) yok sayılır (uyarı). Karo/atlas için `KHR_texture_transform` malzeme başına bir dönüşüm |
| Malzeme | Metal-roughness PBR. Desteklenen: base color, normal, metallic-roughness (**ORM**, R=AO), emissive, occlusion, `alphaMode`, `doubleSided`, `KHR_materials_unlit/emissive_strength`. Clearcoat/sheen/transmission yalnız faktör. Blender Principled BSDF'yi bu kanallara bağla; prosedürel node'lar dışa aktarılmaz, bake et |
| Dokular | **Ayrı PNG/JPG dosyaları**, model yanında, glTF'ye göreli yol; her biri kendi sidecar'ıyla doku varlığı. Gömülü doku kullanma (Blender'da "Keep original"/ayrı dosya, veya dışa aktarıp ayırarak). Normal/ORM dokularına `"srgb": false` ver. Boyut 2'nin kuvveti, mobil için ≤2048, `max_size` ile küçült |
| İskelet | En fazla 4 etki/vertex (fazlası en büyük 4'e indirilir, uyarı), skin başına ≤256 kemik. **Kemik adlarını stabil ve tutarlı tut**: klipler ve `animations_from`/`mce retarget` kemik adıyla bağlanır |
| Animasyon | Klip adları (`Idle`, `Walk`, `Run`, `Jump`, `Slash`...) `.mcanim` grafikte kullanılır: **tek karakter seti için aynı adları kullan**. NLA/Action'ları ayrı klip olarak dışa aktar. Morph target/shape key desteklenmez (varsayılan ağırlık pozlara pişer) |
| Işık/kamera | Mesh varlığı entity tutamaz; ışık/kamera için `mce import gltf` (veya `asset_import {tool:"gltf"}`) prefab yazar. Yönlü ışık entity olmaz (`Environment3D.sun_*` önerisi uyarıda). Işık birimleri ADR-0024 (radyans, fotometrik değil): sahne koyu/parlaksa `intensity` ölçekle |
| LOD | Düğüm adı `<ad>_LOD1`, `_LOD2` ile el yapımı seviye; yoksa sidecar `"lod_levels":1..3`. İskinli alt-mesh tek seviye |
| İsimlendirme | Düğüm/nesne adları anlamlı ve benzersiz (`Wheel_FL`, `Door_L`): `AttachToBone`, scene araması ve hata mesajları adı kullanır. Türkçe karakter ve boşluktan kaçın. Dosya adları küçük harf, `snake_case`, `hero.glb`, `traffic_car.glb` |
| Çarpışma | Görsel mesh'ten ayrı basit kutu/kapsül collider (`BoxCollider3D`) kullan; `MeshCollider3D` yalnız statik ve düşük poli |

Dışa aktarma örneği (headless `bpy`, motorun beklentilerine göre; parametre adlarını Blender sürümünde `bpy.ops.export_scene.gltf.get_rna_type()` ile doğrula):
```python
import bpy, sys
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
bpy.ops.export_scene.gltf(
    filepath=out_path, export_format='GLB', export_yup=True,
    export_apply=True,                 # modifier'ları uygula
    export_texcoords=True, export_normals=True, export_materials='EXPORT',
    export_image_format='AUTO',        # dokuyu AYRI dosya istiyorsan export_keep_originals / 'NONE' ve dosyaları kendin kopyala; çıktıyı cook özetiyle kontrol et
    export_draco_mesh_compression_enable=False,
    export_morph=False, export_lights=False, export_cameras=False,
    export_animations=True, export_skins=True, export_all_influences=False)
```

## Blender'da üretim ipuçları

- Kitbash: önce parça kitini (duvar, pencere, kapı, çatı) modül boyutuna (örn. 4 m ızgara) göre yap, sahnede `prefab` olarak örnekle; şehir için yüzlerce benzersiz mesh yerine tekrar kullanılan parça + `StaticMerge3D`/instancing (aynı mesh+malzeme otomatik instanced çizilir).
- Doku: Blender'da bake (AO, normal) → PNG; ya da `ambientcg` PBR setlerini `mce content add` ile al. Yol işareti/kaldırım gibi detay için doku + `uv_scale` ile döşeme.
- İskelet/animasyon: Rigify veya Mixamo benzeri CC0 animasyon (Quaternius Universal Animation Library `mce content add quaternius-universal-animation-library`) + `mce retarget`.
- Her model için betik + `.blend` kaynağını repoda sakla (örn. `samples/<örnek>/blender/`), yalnız `.glb` + sidecar'ı `assets/`'e koy.
- Ölçek/eksen için ilk olarak 1 m küp + insan boyu çubuğu (1.8 m) üret, motorda `render_frame` ile oyuncu yanında karşılaştır, sonra gerçek modele geç.

## CC0/CC-BY içerik kaynakları

Yalnız **CC0 ve CC-BY** (NC, SA, ND yok). Bulut konteynerinde: `kenney.nl` 403 verir (Kenney paketleri `manual`: kullanıcı tarayıcıdan indirip `--file` ile verir), **OpenGameArt çalışır**, ambientCG/Poly Haven `http` kaynağı olarak `mce content add` ile iner.

| Kaynak | Ne | Not |
|---|---|---|
| `library_search` / `library_add` (SDK içi) | Kenney city/platformer modelleri, sesler, OFL fontlar, jeneratör tarifleri | `CREDITS.md` satırını kendisi ekler |
| `mce content list/info/add/verify` | ledger: Kenney City Kits (commercial, suburban, roads), Car Kit, Blocky Characters, Impact Sounds; ambientCG asphalt/road/concrete/paving/grass PBR; Poly Haven sky HDRI; Quaternius Universal Animation Library | SHA-256 doğrular, GUID'ler deterministik, ORM paketler; `mce content verify` gönderim öncesi kapı |
| OpenGameArt (CC0 filtreli) | ses efekti, döngü, müzik | lisansı sayfada oku, kaynak sayfasını `SOURCES.md`/`CREDITS.md`'ye yaz; `tools/fetch_audio.py` örneği SHA pin'ler |
| ambientCG, Poly Haven, Quaternius, Kenney, Freesound (yalnız CC0 filtresi), Sonniss GDC paketleri (lisansı oku) | doku, HDRI, model, ses | yeni paket = `content/ledger.json` girdisi + `mce content check` + PR |

Ses: döngüler PCM `wav`, uzun müzik/ambiyans `vorbis`; kaynak 48 kHz stereo olursa yeniden örnekleme yok. `stb_vorbis` bozuk yorum sayısında çöker: `VorbisReader::open` korur, yine de tuhaf `.ogg`'leri önce `ffmpeg` ile yeniden kodla.
