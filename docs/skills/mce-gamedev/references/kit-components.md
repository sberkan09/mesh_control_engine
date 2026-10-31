# Kit bileşenleri (veri olarak kullanılan oyun parçaları)

Tam liste ve hangi örnekte gösterildiği: `docs/engine/capabilities.md` (CI üretir, elle düzenleme) ve `engine_capabilities` MCP aracı. Alanlar için her zaman `refl_describe_component`. Ayrıntı: `docs/engine/kit.md`, `docs/user-guide/08-3d.md`, `09-physics.md` (bölüm numaraları aşağıda).

## 2D

`Sprite` (texture, size, `layer`, `region`, `tint`; `normal_map` ile ışıklı), `AnimatedSprite` (+`.mcsheet` klibi), `TextLabel` (HUD için `Parallax{factor:[0,0]}`), `Camera2DComponent` (`pixels_per_meter` 48 → 1280x720'de ≈26.7x15 m), `Post2DComponent`, `TilemapLayer`, `ParticleEmitter`, `Light2D`, `Parallax`, `Skeleton2D`+`Skeleton2DAnimator`+`Bone2DFollow`+`IKTarget2D`, `PathAgent` (NavGrid üzerinde A*), `RigidBody2D`/`Collider2D`/`CharacterController2D`/`Joint2D`, `CameraFollow2D`, `CameraShake2D`. Kitte oyuncu denetleyici, toplanabilir, tehlike, hedef yok: kendi `MCE_COMPONENT`'lerin (örnek `projects/Kindle/games/kindle/module/components.h`). Sprite tek dokuyu `size`'a gerer: uzun platformu tek sprite ile çizme, döşeme başına sprite veya `TilemapLayer`.

## 3D çekirdek (docs 08, 09)

- Çizim: `Camera3DComponent`, `MeshRef` (mesh, `material` override, tint), `Environment3D` (güneş, gökyüzü, sis, pozlama, gölge/SSAO/SSR/bloom), `PointLight3D`/`SpotLight3D`, `ReflectionProbe3D`, `GiVolume3D`, `Decal3D`, `Foliage3D`/`TerrainFoliage3D`, `Terrain3D`, `StaticMerge3D`, `Water`, `ParticleEmitter3D`, `LightningBolt`.
- Fizik (Jolt): `RigidBody3D`, `BoxCollider3D`/`MeshCollider3D`/`TerrainCollider3D`, `CharacterController3D` (9.3), `Trigger3D`, `Joint3D`, `Ragdoll3D` (9.8), `Raycast3D`/`Overlap3D`, `PhysicsEvents`/`ImpactDamage3D`.
- Gezinme (Recast/Detour): `NavMeshSurface3D` (`navmesh_bake`), `NavArea3D`, `NavLink3D`, `NavObstacle3D`, `NavAgent3D` (9.7).
- Animasyon: `Animator` (+`.mcanim`), `AttachToBone`, `Locomotion3D`, `IKTarget3D`. gen_mesh `humanoid`: 13 kemik, `Idle`/`Walk` klipleri, −Z'ye bakar.
- Kamera: `CameraFollow3D` (yay kolu), `CameraRail3D`, `CameraLookAt3D`, `CameraShake3D`, `CameraBlend3D`, `Cutscene`, `TimelinePlayer`; çoklu görünüm `CameraView`, `SplitScreen`, `RenderTexture`+`ShowRenderTexture` (`view_add`), `PlayerCamera`.
- Dünya akışı: `SceneChunk`, `StreamingFocus`, `FloatingOrigin3D`, `ChunkLod3D`.

## Açık dünya / aksiyon (docs 09 §9.9-9.19, 29)

| Alan | Bileşenler | Not |
|---|---|---|
| Araç | `Vehicle3D`, `VehicleSeat3D`, `Rider3D`, `VehicleDriver3D`, `VehicleRam3D`, `CarOwner3D`, `VehicleHorn3D`, `VehicleLights3D`, `DriveReadout3D` (HUD hız/vites) | binme/inme 9.9; hız, rpm, vites motor alanlarıdır, `inspect` ile okunur |
| Silah/can | `Health3D`, `Weapon3D`, `Projectile3D` | 9.12; olaylarda veri var (ör. `WeaponFired3D`); atış efekti (muzzle flash, iz) kendin bağla |
| Polis | `Wanted3D`, `Police3D` (yaya veya `drive_car`), `Roadblock3D`/`RoadblockPiece3D`, `Respawn3D`/`RespawnPoint3D`, `Confiscate3D`, `HitMarker3D` (vuruş işareti), `ImpactFx3D` (vuruş noktasında kıvılcım), `FaceVelocity3D` (model yürüme yönüne döner) | 9.13; tutuklama, takviye, barikat; ölüm/tutuklanma sonrası respawn için `RespawnPoint3D` ver, yoksa oyun biter |
| Ekonomi | `Wallet3D`, `CashDrop3D`, `CashPickup3D`, `MissionReward3D`, `AmmoShop3D`, `HealShop3D`, `ChopShop3D`, `PaySpray3D`, `RepairShop3D` | dükkân = yassı kutu entity + bileşen |
| Görev | `Mission`, `Objective` (zincir `next`), `MissionMarker`, `MissionRetry`, `MissionWantedFail`, `MapMarker` | 9.14-9.15; minimap widget'ı `MapMarker`lardan çizilir |
| Trafik/kalabalık | `RoadNetwork3D` (`.mcroads`), `TrafficSpawner3D`, `TrafficDriver3D`, `Pedestrian3D`, `CrowdSpawner3D` | docs 29; yayalar oyuncu/araç/ateşe tepki verir (Flee, ölü yaya, çarpma) |
| Yapay zeka | `AiBrain` (.mcbt), `Blackboard`, `Perceiver`, `Stimulus`, `AiTokenPool` | 13.8 |
| Envanter | `Inventory`, `Equipment`, `ItemPickup3D`, `LootDrop`, `Container`, `CraftingStation`, `StatusEffects` | `.mcitems` katalogu |
| Zaman/hava | `WorldClock`, `DayCycle`, `TimedLight3D`, `Weather`, `Precipitation3D`, `Lightning`, `WindDirection`, `WindGust`, `WindSway`, `WetSurfaces`, `RainSplash`, `RainRipples`, `SnowCover`, `GroundMist`, `CloudLayer`, `Moon`, `Aurora` | 9.16-9.19; "Low" kalite katmanı bunları kapatır (`r.nightsky`, `r.aurora`, `r.water`...) |
| Ses | `AudioSource`, `AudioSpatial`, `AudioListener`, `AudioOcclusion`, `AudioDoppler`, `AudioReverb`, `AudioDuck`, `AudioSnapshot`, `AudioBusEffects`, `AudioLimiter`, `EngineAudio`, `WeaponAudio`, `ImpactAudio`, `FootstepAudio`, `SirenAudio`, `WeatherAudio`, `SfxTrigger`, `SoundCaption` | docs 10 §10.9 |
| UI | `UiScreen`, `UiFlow`, `UiFlowRules`, `UiScreenView`, `.mcui` | 12; `UiFlowRules` `SaveSlot`/`LoadSlot` kaydeder/yükler (menü), `field` yuva adıdır |
| Diğer | `ScriptSystem`/`ScriptVars`/`ScriptParams` (Luau), `Dialogue`/`DialogueInput`/`StoryState` (roman), `SaveKey`, `ProcLevel*`, `CoopGame`/`PlayerSpawn`/`LocalPlayer`, `VideoPlayer`, `NetReplicated`/`NetOwner` | |

## GTA tarzı örnekten (samples/09_openworld) çıkan kurallar

- **Önce bağla, sonra yaz**: motorda çoğu parça hazır; örnek sahne HUD, ses, respawn, polis arabası, gün/gece, hava, görev, dükkân bileşenlerini bağlamıyordu. Eksik görünen şey için önce `engine_capabilities`/`capabilities.md` ve ilgili `10_missions`, `11_daynight`, `12_weather`, `12_loot` örneğine bak.
- **Örnek veriyi betikten üret**: `samples/09_openworld/gen_assets.py` sahneyi yeniden yazar; sahneyi elle düzenleme, betiği düzenle ve çalıştır (master ile birleştirirken çakışma bu betikte ve `city.mcscene`'de olur: betiği elle birleştir, sahneyi yeniden üret).
- **HUD**: `.mcui` belgesi + `UiScreen`; can/mermi/nakit/wanted yıldızı widget'ları `bind_property` benzeri veri bağlama ile (`Wanted3D/level` gibi yollar), minimap `Minimap` widget + `MapMarker`. Görev metni Luau ile.
- **Ses**: ses bileşenlerini üreten sistem gerekir (örnekte `kit/world_audio.h`); `audio_bridge` konumu **yalnız entity'nin kendi `LocalTransform`'undan** okur, bu yüzden sesi kök entity'ye koy (çocuğa değil). Dünya sesleri SFX bus'ında; **48 gerçek ses** var, trafik motoru düşük öncelik. `AudioDoppler`'ı `EngineAudio` perdesiyle karıştırma. Döngü sesleri PCM, uzun yataklar `vorbis`. Olaylar test dünyasında bir tick geç görünür.
- **Yaya/araç görünmüyorsa**: kamera yönünü ve sistem sırasını kontrol et. Yaya kinematik kapsülü `kit3d_bodies` tarafından geri yazılabilir (yaya yerinde kalır): kalabalık sistemi `kit3d_bodies`'ten **sonra** çalışmalı ve gövdeyi hızla sürmeli (SBA-213, PR #859). Headless "N entities" satırı sahnenin kendi entity'lerini sayar, doğan trafiği saymaz.
- **Karakter animasyonu**: iskinsiz düğüm animasyonlu modeller (Kenney Blocky) `Animator` ile kayar; gerçek iskeletli karakter + `Locomotion3D` + `.mcanim` kullan, klipleri `mce retarget` ile ekle. `use_input` karakterinde bakış yönü bileşeni yok: hareket yönüne döndürmeyi kendin ekle.
- **Görsel kalite**: tüm şehir tek `box.glb` kutuları olmasın; yol çizgisi/kaldırım/bina için gerçek mesh veya dokulu `.mcmat` (ambientCG PBR), gökyüzü HDRI (`Environment3D.sky_texture`), solgun renk için `Environment3D.exposure` ve güneş şiddetini ölçerek ayarla.
- **Küçük görev recipe**: `Objective` zinciri (`ReachZone`, `next`) + `Mission` + `MissionMarker` pad + `MissionReward3D(cash, fail_fee)` + `MissionRetry`; testi MCP ile yaz: `component_set` ile oyuncuyu yerleştir, `scene_save`, `run_headless` (ticks, inspect). Yeni dükkân/pad: yassı kutu + bileşen, minimap için `MapMarker`.
