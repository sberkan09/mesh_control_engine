# Motora katkı, C++ modülü ve CI kuralları

Kaynak: repo `CLAUDE.md`, `CONTRIBUTING.md`, `docs/ROADMAP.md`. Bu dosya, oyun için gereken C++'ı ve motor reposuna PR açmayı kapsar. Oyun yazmak için çoğu zaman gerekmez.

## Oyun modülü (C++)

Yer: `projects/<Oyun>/games/<oyun>/module/`. Bileşenler `components.h` (yalnız veri, `MCE_COMPONENT`), sistemler `<oyun>.cpp`, `install()` bileşenleri ve sistemleri kaydeder, `types.cpp` bileşen başlıklarını include eder (cook/MCP bilsin). Şablon: `templates/project`. Ayrıntı: user-guide 6.

- Bileşen: düz, trivially copyable, padding'siz (`u8 pad_[N]` + `static_assert(sizeof)`), 8 baytlık/asset referansları önce, sonra 4 baytlık; `Transient`/`ReadOnly`/`Save`/`Range`/`Help` nitelikleri.
- Sistem: `Phase::FixedUpdate_Pre|FixedUpdate|FixedUpdate_Post`, `f.query<A&,B>().each(...)`, yapısal değişiklik `Commands` ile (sorgu ortasında doğrudan değiştirme), olaylar `world.events().read<T>()` (ertesi tik), `scene::request_change`, `ecs::instantiate` (prefab).
- Oyun modülleri `mce/ui/` include edemez; UI belgeleri host'tadır.
- Motor içinde `std::vector/unordered_map`, istisna, RTTI yok (kendi container'ları; duvar `scripts/ci/check_walls.py`).

## Mimari özet

C++20, CMake ≥3.28 preset, GL 4.5 (Linux/Windows), Metal (macOS), SDL GPU (telefon). Katmanlar: L0 `mce_core`; L1 platform/refl/ecs; L2 rhi/asset/physics2d/audio/input; L3 render; L4 kit/ui; L5 `mce_engine` dll + editör; L6 host'lar. Bağımlılıklar CMake FetchContent (sabit git tag'leri). Dizin: `src/engine/<modül>`, `tests/`, `samples/`, `docs/engine/<modül>.md`. Belgeler: `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`, milestone kayıtları `docs/milestones/`.

## Dal, PR ve birleştirme

- PR'lar **`master`**'a. Otomatik birleştirilen dallar silinir.
- Birleştirme yetkisi: kendi PR dalına `origin/master` birleştirip itmek ve CI yeşil + gerekli koordinatör incelemesi sonrası kendi PR'ını birleştirmek serbest; başkasının dalına force-push ve test atlamak yasak. Yalnız doküman (`*.md`) PR'ı CI'sız, hızlı okumayla birleşir.
- **Çekirdek sistem** (bellek, thread/job, render, ECS, fizik) PR'ı: PR'ı aç, koordinatör incelemesini bekle. Editör/MCP/araç/doc/test PR'ı kendi birleşimin.
- PR gövdesi atıf bloğu ve `changelog.d/<BİLET>-<n>.md` (CHANGELOG.md'ye dokunma). ROADMAP durum tablolarını thread değil koordinatör günceller.
- Linear (SBA, mesh_control_engine projesi) kartları: başlarken In Progress, birleşince Done + PR linki yorumu. Başka thread'in In Progress milestone'unu alma. İzin reddederse hangi kart/durum olduğunu söyle.

## CI

GitHub Actions `ci.yml` her PR'da `scripts/ci/local_ci.sh --task`; `integration.yml` `master`'da 6 saatte bir tam matris.

| Seviye | Ne zaman | Komut |
|---|---|---|
| PR kapısı | PR başına bir kez, kendi tabanında | `scripts/ci/local_ci.sh --task` |
| Birleştirme öncesi | `master` ilerlediyse | `scripts/ci/local_ci.sh --premerge` (walls, capabilities --check, MCP transcript golden, kendi testlerin; 1-3 dk) |
| Entegrasyon | yalnız entegrasyon iş akışı | `scripts/ci/integration_run.sh` |

- `--task` bir kez; `master` birleştirdikten sonra tekrar çalıştırma, `--premerge` çalıştır (çıktı `master`'ın PR'ın dosyalarına dokunup dokunmadığını söyler; yalnız çakışmada `--task` yeniden).
- Thread'ler `--batch` ve tam matrisi **çalıştırmaz**. Entegrasyon kırmızıysa suçlu PR'ın thread'i ileri doğru düzeltir.
- Üretilen dosyalar (`docs/engine/capabilities.md`, `scripts/ci/sample_coverage.allow`, `tests/mcp/transcripts/*.golden.jsonl`) entegrasyon koşusunun işidir; birleştirme sonrası bayatlama `--task` yenileme sebebi değil. (Yeni bileşen eklerken yine de `capabilities.py` çalıştırıp transcript'teki bileşen sayısını güncelle: bkz. pitfalls.)
- Konteyner disiplini: `scripts/ci/setup_env.sh`; `git fetch origin master` `--task`'tan önce (yoksa her aşama tam çalışır, tidy tüm ağacı denetler); `--task` **detached** çalıştır ve `=== Summary` için log'u bekle: `nohup scripts/ci/local_ci.sh --task > build/local-ci-logs/run.log 2>&1 &`. Çalışma sırasında konteyneri yeniden başlatma; thread'e mesaj gelmesi konteyneri yeniden başlatır ve detached koşuyu öldürür (`uptime -s`, `--task --resume`). ccache'i silme/küçültme.
- Hızlı yerel: `local_ci.sh walls` (~1 dk), `--quick` (walls + gcc debug + GPU testleri Xvfb).

## Değişiklik kuralları (ucuz derleme)

- **Geniş header** (çok TU include eder; walls 150 üstünde uyarır): kendi küçük "seam" PR'ı olarak önce birleşir; `.cpp` değiştirmeyi tercih et, ileri bildirim kullan, umbrella header'ı büyütme (`scripts/ci/include_fanout.py`).
- **Yavaş test** (>60 sn): `--task` için smoke varyantı + tam sürüm `mce_mark_slow(<test>)`, entegrasyonda koşar. Yeni determinism konusu = modül→konu haritasına satır. `ctest --preset ... -LE <ifade>` kullanıyorsan ifade `slow` içermeli.
- **Görev şekli**: eşzamanlı thread'ler ayrık modüllerde çalışır; üç modüle dokunan iş bölünür; PR'ları yığınla (seam → özellik → cila).
- **Dokümantasyon disiplini**: kullanıcıya görünen özellik için guide sayfası + `docs/engine/<modül>.md`, milestone dosyası (adım durumu, PR numaraları, açık işler/riskler), changelog parçası. Adım dokümanı olmadan bitmiş sayılmaz.
- Hata düzeltince aynı PR'a regresyon testi koy.

## Roadmap ve süreklilik

ROADMAP milestone'ları sırayla; bitince yeni kalem ve Linear kartı eklenir (proje kuralı: motor kesintisiz geliştirilir). Her adım bitince thread'de kısa "şu bitti, şu başlıyor" mesajı at (bildirimli). Model politikası: varsayılan Sonnet 5.5; bellek/thread/render/ECS/fizik/performans için Opus 5.5 gerekçeli; haftalık limit dolarsa Fable, sıfırlanınca eski modele dön.
