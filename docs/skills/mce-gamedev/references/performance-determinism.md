# Performans bütçeleri ve determinism

## Hedef başına bütçe (varsayılan; `.mceproj` `budgets` ile değişir, 0 = sınırsız)

| | desktop | mobile | web |
|---|---|---|---|
| `frame_ms` | 16.7 | 33.3 | 16.7 |
| `sim_ms` | 4 | 4 | 4 |
| `render_ms` | 6 | 8 | 6 |
| `gpu_ms` | 12 | 25 | 12 |
| `draws` | 3000 | 400 | 1000 |
| `triangles` | 3 M | 500 k | 1 M |
| `sprites` | 50 k | 10 k | 20 k |
| `upload_bytes` | 32 MB | 4 MB | 8 MB |
| `sprite_textures` (sahne başı) | 64 | 16 | 32 |
| `texture_size` (en uzun kenar) | 8192 | 2048 | 4096 |

Zamanlar p95, sayaçlar en kötü kare üzerinden. Sayaçlar her makinede aynıdır, süreler değil. 3D varlık için pratik üçgen bütçeleri: ana karakter 5-15 k, NPC 2-6 k, araç 5-12 k, bina 1-5 k, props 100-1000; sidecar `max_triangles` ile zorla; uzak için `lod_levels` 2-3. Bir sahneyi tek `perf_run` ile ölç, sonra bir şey değiştir, `perf_compare`.

## Yaygın düzeltmeler

| Fazla olan | Çare |
|---|---|
| `draws`, `sprite_batches` | sprite'ları `.mcsheet` atlasında topla; aynı mesh+malzeme otomatik instanced (kopya başına tint/malzeme override'ı bunu böler); statik mesh'leri `StaticMerge3D`/HLOD ile birleştir |
| `sim_ms` | `top_systems` sistemi söyler; iç içe tüm-entity döngülerini ızgaraya böl; fizik gövdesini azalt |
| `render_ms` | çıkarılacak şeyi azalt (cull, gizle), batch sayısı |
| `gpu_ms` | `gpu_stages` geçişi söyler; `Environment3D` gölge/SSAO/SSR/bloom azalt; render ölçeği |
| `triangles` | düşük poli, LOD |
| `upload_bytes` | her karede yeniden yazılan dinamik sprite/mesh azalt |
| doku boyutu | `max_size` sidecar veya `--max-texture-size 512`; pack boyutu için `sizes` raporuna bak (1024² RGBA8+mip ≈5.3 MB, BC7 ≈1.3 MB) |
| görsel yeni katman | 1080p `perf_run` ile ölç; Low kalite katmanı yeni efekti kapatmalı |

CI sayaç kapısı: user-guide 25.5. Yeni 60 sn'yi aşan test için smoke varyantı + `mce_mark_slow`.

## Determinism kuralları

Aynı sahne + tik sayısı + girdi + aynı ikili = aynı `World::state_hash`. Headless çalışma, editör Play paneli, `run_headless`, testler ve replay buna dayanır.

- Sistem içinde duvar saati, rastgele cihaz, `std::rand`, global/statik durum **yok**. Rastgelelik: bileşende saklanan tohumlu PCG (parçacıklar, loot `seed`, `procgen_run` seed).
- Sırasız container iterasyon sırası oyunu etkilememeli (motorun `HashMap`'i sim sırasında yasak; `FlatMap` sıralı). Motor içinde `std::vector/unordered_map` duvarı vardır (`scripts/ci/check_walls.py`).
- Float matematiği aynı makinede aynıdır; makineler arası aynı hash iddia edilmez. FMA'dan kaçın (CPU iskelet skinning'i bunu yapar). FBX makineler arası birkaç kuantizasyon adımı farklı çıkabilir (glTF ve OBJ çıkmaz).
- Bileşen baytları hash'lenir: **padding yok**, açık `u8 pad_[N]` ve `static_assert(sizeof)`; yeni alan hash'i değiştirir (beklenen).
- `Transient` çalışma zamanı alanı sahneye yazılmaz; yalnız oyunun yazdığı alanları işaretle.
- UI oyun durumuna doğrudan yazmaz; olay → sistem. Render kararları (LOD, çizim) hash'i değiştirmemeli (render yalnız `-view.*` ile görünümü değiştirir).
- Akış: headless ve editör Play bölgeyi aynı tikte senkron yükler ki hash korunsun; pencereli oynatıcı arka planda yükler.
- Doğrulama: aynı komutu iki kez çalıştır, hash eşit mi; değişiklik "hiçbir şeyi değiştirmemeli" ise hash **aynı** kalmalı (kapalı özellik kareyi/ hash'i değiştirmemeli, "off-check").
- Hash değişince: `scripts/ci/local_ci.sh determinism` (tam matris entegrasyon koşusunda).
