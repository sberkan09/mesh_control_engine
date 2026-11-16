# Çalıştırma ve doğrulama

Kaynak: `docs/user-guide/01, 16, 15, 17, 25`. Komutlar Linux `linux-gcc-debug` presetiyle; macOS'ta `macos-debug|release`, Windows'ta `msvc-*`. SDK varsa `mce <komut>`.

## Derleme

```sh
scripts/ci/setup_env.sh                     # Ubuntu 24.04 paketleri (bulut oturumları başlangıçta çalıştırır)
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target my_game my_game_mcp mce_cook mce_player
ctest --preset linux-gcc-debug [-R ad]       # GPU testleri için -DMCE_GPU_TESTS=ON + xvfb-run
```
Örnek tek hedefler: `--target test_kit`, `openworld_content`, `test_mcp_openworld`. Hızlı döngü: yalnız ilgili test hedefini derle ve `-tc="isim*"` ile çalıştır. Diski doldurma: `ccache`'i silme/küçültme (betik `reclaim_space` yapar). Container'a mesaj gelince detached işler ölür (`uptime -s` ile bak).

## Oynatma

```sh
mce play MyGame                                         # SDK
build/<preset>/projects/MyGame/my_game                  # checkout: pencere
mce_player -pack path/assets.mce [-module lib.so] [-scene scenes/x.mcscene]   # stok oynatıcı
my_game -headless 1 -ticks 600                          # pencere yok, sonda state hash basar
```
WASD/ok hareket, Space zıpla, Esc duraklat, `` ` `` veya F1 konsol, F7 2D post aç/kapa. Bayraklar convar'dır (`-isim değer`); `-help` hepsini listeler. Önemlileri: `-scene`, `-pack` (`.mce` veya `cooked/` klasörü), `-input dosya` (betikli girdi), `-window WxH`, `-save <slot>`, `-time_scale` (hile), `-audio.enabled 0`, `-perf_capture f.mcperf`, `-r.frame_report N`.

**Eski paket tuzağı**: `mce play`/`headless` `build/project/assets.mce`'yi çalıştırır, MCP `cook_run` ise `cooked/`'u yazar. Oyun eski içeriği gösteriyorsa `mce build` veya `cook_run {pack:true}`.

## Ekran görüntüsü ve girdi (ekransız dahil)

```sh
mce_player -pack x.mce -capture_tick 120 -capture shots/a.ppm
mce_player -pack x.mce -capture_ticks 60,300,900 -capture "shots/t_{tick}.ppm"
mce_player -pack x.mce -scene scenes/city.mcscene -input moves.txt -capture_tick 300 -capture shots/play.ppm
xvfb-run -a -s "-screen 0 1280x720x24" env LIBGL_ALWAYS_SOFTWARE=1 mce_player ...
LIBGL_ALWAYS_SOFTWARE=1 mce_player -platform.video_driver offscreen -frames 30 -capture c.ppm
```
- Ekran yoksa `-capture*` kendiliğinden SDL offscreen + Mesa yazılım GL'e düşer.
- `-capture_tick N` simülasyon tikine bağlıdır (kare hızından bağımsız, aynı girdi→aynı byte); `-frames N` yalnız "çizim var mı" duman testidir.
- PPM → PNG: `convert a.ppm a.png`. MCP'de doğrudan `render_frame` (PNG görüntü) kullan.
- `moves.txt`: satır başına `<from>[-<to>] Eylem[=değer]`, tik cinsinden (`10 Confirm`, `20-80 Move=1,0`). Başlık menüsü varsa önce menüyü geçiren satır koy.
- GPU testleri `xvfb-run -a` ister; SDL GPU testleri tek süreçte yarıda çökebilir, dosya dosya çalıştır.
- Telefon yerleşimini görmek için `-window 1080x2400`, `-safe_area l,t,r,b`.

## Platformlar

| Platform | Nasıl |
|---|---|
| **Linux** | yukarıdaki; GL 4.5 (Mesa llvmpipe yavaş ama çalışır) |
| **macOS** (Apple silicon, macOS 26+) | Metal; `cmake --preset macos-debug`, aynı komutlar, `./install.sh` SDK'yı `~/mce-sdk` kurar. Ses izni verilmezse CoreAudio ~15 sn bloklanır (`AudioQueueStart -66681`), oyun sessiz sürer: Terminal/IDE için Gizlilik > ses iznini ver. Gönderim: `python3 scripts/ship.py --project projects/X` `.app` zip'i yazar (ad hoc imza; başkasında sağ tık > Aç). Yerel CI: `scripts/ci/local_ci_macos.sh` |
| **Android** | `python3 scripts/ship.py --project projects/X --target android` APK (`--aab` Play paketi); oyun uygulamaya statik bağlanır, paket host'ta cook edilir (ASTC). Emülatör (Android 15 arm64, `-gpu host`; SwiftShader'da ilk present sonsuza takılır) ve `adb`: `adb install`, günlük `adb logcat -s MCE`, dokunmatik `adb shell input`. İcon için `ship/icon.png` 1024². Dokunma: `TouchStick`/`TouchZone` bağlamaları, `ViewInfo` safe-area. Mac gerektiren adımlar `local_ci_macos.sh android` |
| **iOS** | `--target ios` (Simülatör `.app`) veya `--preset ios-arm64` imzalı `.ipa`; Mac gerekir |
| **Web** | `ship.py --target web` (Emscripten, WebGPU, itch.io zip); `ship_build target:"web"` headless Chromium'da kontrol eder |
| **Windows** | MSVC; modül host'a bağlanır (`.so` yok); elle kontrol edilir |

Doku sıkıştırma hedefe göre: masaüstü `--textures bc` (isteğe bağlı), telefon `astc`, web `rgba8`. Piksel sanatı ve UI'da BC kullanma (`compress:"none"`).

## Doğrulama kontrol listesi

1. `run_headless`: `ok`, `log_errors 0`, beklenen `inspect` değerleri, iki koşuda aynı `state_hash`.
2. `render_frame`: karenin boş/magenta (eksik doku)/siyah olmadığını gerçekten **bak**; HUD ve UI için `ui_preview`.
3. `scene_lint`, `project_check` (kırık referans, eksik credits, 16 px altı metin).
4. `.mctest` ile bot bitiriyor mu (`finished_tick`, `deaths`, `stuck_spots`).
5. `perf_run {target}`; sayaçlar makineden bağımsızdır, süre değildir (yazılımsal rasterizer süre vermez).
6. Ekip/CI: `scripts/ci/local_ci.sh walls` (~1 dk), sonra PR'da Actions `--task`.

## Raporda dürüst ol

Bulutta doğrulanamayanlar: gerçek GPU kare hızı, fare/gamepad hissi, Mac/Android'de ses ve cihaz davranışı. "Test ettim" yerine neyi hangi komutla gördüğünü yaz (ekran görüntüsü yolu, hash).
