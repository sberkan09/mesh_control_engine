#pragma once
// Forward3D — the 3D frame (rhi-renderer.md §3.5 "[v0.5] Ashen Court frame", §3.6; ADR-0024):
//
//   0. SSAO          (AmbientOcclusion::enabled, M-VISUAL 3) half-resolution normal + depth prepass of
//                    the opaques, obscurance, depth-aware blur; read by the main pass's ambient terms
//   0b. SSR          (ScreenSpaceReflections::enabled, M-VISUAL2 1) a ray march through the same
//                    prepass's depth reading last frame's HDR; blended over the main pass's probes
//   0c. contact      (ContactShadows::enabled, M-VISUAL2 3) a short march toward the sun through the
//                    prepass's depth; darkens the sun's light where the cascades are too coarse
//   1. cascades      up to 4 sun cascades into one D32 2D-array (shadow_size² per layer), depth only,
//                    then the spot / point light atlas tiles (M-VISUAL 4)
//   1b. GI           (GiVolume::enabled, M-VISUAL 6) the irradiance volume's rolling update: a sun
//                    shadow fitted to the volume, cube captures of a few probes, their projection
//   2. main pass     Forward+ PBR (Cook-Torrance GGX, ORM + normal + emissive maps) lit by the sun
//                    (cascaded PCF 3×3 shadows, cross-faded) and the clustered point lights, then
//                    the sky dome behind everything, then blended draws back-to-front; R11G11B10F
//                    HDR target with MSAA
//   3. resolve       MSAA -> single-sample HDR
//   4. bloom         soft bright-pass, 13-tap downsample chain, tent upsample (additive)
//   5. tonemap       exposure + bloom composite, ACES fit, sRGB encode (+ luma for FXAA)
//   6. FXAA          Lottes-style edge search into the final target
//   7. upscale       (PostFx::upscaler / sharpen, M-VISUAL2 2) an edge-adaptive upsample of a frame drawn
//                    below the output size, then a contrast-adaptive sharpen; FXAA and the tonemap
//                    then write an intermediate at the frame's own size
//
// Owned by the Renderer (Renderer::init_3d); the 2D path never pays for it. Targets follow the
// scene's viewport size and are recreated on resize. Each stage boundary writes a GPU timestamp
// (rhi::CommandBuffer::write_timestamp) for the NFR-F-01 per-stage budget table.

#include "mce/draw_cause.h"
#include "mce/render/scene3d.h"
#include "mce/render/texture_store.h"
#include "mce/rhi/command_buffer.h"
#include "mce/rhi/render_device.h"

namespace mce::render {

struct Forward3DPassU;  // forward3d.cpp: the lit passes' PassU block

struct Forward3DDesc {
    u32 msaa = 4;            // 1 = no multisampling (the HDR target is rendered directly)
    u32 shadow_size = 2048;  // per cascade; keep ShadowSettings::resolution equal (texel snapping)
    f32 render_scale = 1.0f; // 3D targets at viewport * scale (0.5..1); tonemap/FXAA upsample (quality.h)
    bool encode_srgb_output = true;  // the backbuffer/editor targets are plain RGBA8 (see RendererDesc)
    bool timestamps = true;          // write GPU timestamps [timestamp_base, timestamp_base + kStageCount]
    u32 timestamp_base = 0;
    // Forward+ light assignment in a compute pass when the device has compute (M-VISUAL 1); the
    // CPU ClusterLights::assign() otherwise or when false. Both give the same frame.
    bool gpu_light_clusters = true;
    // Image-based lighting resources (M-VISUAL 2); a scene turns it on with Sky::ibl. False (or
    // -r.ibl 0) keeps every scene on the hemisphere ambient.
    bool ibl = true;
    // Screen-space ambient occlusion resources (M-VISUAL 3); a scene turns it on with
    // AmbientOcclusion::enabled. False (or -r.ssao 0) draws every scene without it.
    bool ssao = true;
    // Screen-space reflection pipelines (M-VISUAL2 1); a scene turns them on with
    // ScreenSpaceReflections::enabled. False (or -r.ssr 0) keeps every scene on the probes and the sky.
    bool ssr = true;
    // ContactShadows::enabled. False (or -r.contact_shadows 0) leaves the sun to its cascades alone.
    bool contact_shadows = true;
    // Local-light shadow atlas layers after the cascades (M-VISUAL 4); keep
    // LocalShadowSettings::layers equal. 0 (or -r.local_shadows 0) draws every light unshadowed.
    u32 local_shadow_layers = 1;
    // Temporal anti-aliasing resources (M-VISUAL 5); a scene turns it on with PostFx::taa. False
    // (or -r.taa 0) keeps FXAA as the only anti-aliasing.
    bool taa = true;
    // Irradiance-volume resources (M-VISUAL 6); a scene turns it on with GiVolume::enabled. False (or
    // -r.gi 0) draws every scene with the sky's ambient.
    bool gi = true;
    // Volumetric fog pipelines (M-VISUAL 7); a scene turns it on with Volumetrics::enabled. False (or
    // -r.volumetrics 0) draws every scene with the exp² fog.
    bool volumetrics = true;
    // Post-effect pipelines of M-VISUAL 8 (auto exposure, depth of field, motion blur; the color
    // grade needs none); a scene turns each on in PostFx. False (or -r.post_fx 0) draws every scene
    // without them. Auto exposure also needs compute.
    bool post_fx = true;
    // Automatic instancing (M-VISUAL3 5a): the instanced twins of the mesh, SSAO prepass, shadow and velocity
    // pipelines that draw Scene3D's runs. False (or -r.instancing 0) draws every run as single draws.
    bool instancing = true;
    // Bytes of per-copy instancing data a frame may upload over all its views (128 per main / prepass copy, 64
    // per shadow or velocity copy, through set_storage_data); runs past it draw singly
    // (Forward3DStats::instancing_overflow). The frame's budget is the smaller of this and a quarter of
    // geometry_ring_bytes: the storage data shares that ring with the frame's transient vertices, indices and
    // light lists, and an upload that does not fit is dropped by the device, so the budget must stay under it.
    // Each view reserves the main pass's copies first, then the SSAO prepass's, then the velocity prepass's;
    // the shadow passes take what is left.
    u32 instancing_budget = 512 * 1024;
    // The device's per-frame-slot geometry ring (rhi::DeviceDesc::geometry_ring_bytes): keep them equal.
    usize geometry_ring_bytes = rhi::DeviceDesc{}.geometry_ring_bytes;
};

struct Forward3DStats {
    u32 shadow_draws = 0;    // over all cascades and local shadow tiles
    u32 cascades = 0;
    u32 local_shadow_tiles = 0;  // atlas tiles rendered for spot and point lights (M-VISUAL 4)
    u32 main_draws = 0;
    u32 pipeline_binds = 0;
    u32 texture_binds = 0;
    u32 point_lights = 0;
    u32 light_indices = 0;   // CPU assignment only (the GPU count stays on the GPU)
    bool gpu_clusters = false;  // this frame's lights were assigned by the compute pass
    bool ibl = false;           // this frame's ambient came from image-based lighting
    bool ibl_baked = false;     // the sky changed and its IBL textures were re-baked this frame
    u32 probes = 0;             // reflection probes the main pass sampled
    u32 probes_captured = 0;    // probes (re)captured this frame (placed, moved or version bumped)
    u32 probes_loaded = 0;      // baked probes decoded into their slots this frame
    u32 probe_draws = 0;        // draws of those captures, over all six faces
    bool ssao = false;          // this frame's ambient was darkened by screen-space ambient occlusion
    u32 ssao_draws = 0;         // the half-size prepass's draws (SSAO, SSR and contact shadows read it)
    bool ssr = false;           // this frame traced screen-space reflections (M-VISUAL2 1)
    bool ssr_history = false;   // ... over a valid previous frame (false: nothing reflected yet)
    bool contact_shadows = false;  // this frame marched contact shadows toward the sun (M-VISUAL2 3)
    bool taa = false;           // this frame went through the TAA resolve
    bool taa_history = false;   // ... blending a valid history (false on the first frame or after a reset)
    u32 velocity_draws = 0;     // the velocity prepass's draws
    bool gi = false;            // this frame's diffuse ambient came from the irradiance volume
    bool gi_filled = false;     // ... which was filled from scratch this frame (new volume, version or bounds)
    bool gi_loaded = false;     // ... which was loaded from its bake this frame
    u32 gi_probes = 0;          // probes captured this frame (the fill's or the rolling update's)
    u32 gi_draws = 0;           // draws of those captures, over all faces
    u32 gi_lights = 0;          // the most point / spot lights one probe's captures saw
    bool atmosphere = false;         // the sky came from the atmosphere (M-VISUAL 7)
    bool atmosphere_baked = false;   // ... re-baked this frame (the sun or the air changed)
    bool volumetrics = false;        // volumetric fog lit this frame (M-VISUAL 7)
    u32 froxels = 0;                 // its grid: columns × rows × slices
    bool auto_exposure = false;      // the histogram set this frame's exposure (M-VISUAL 8)
    bool color_grade = false;        // the LUT graded this frame
    bool color_vision = false;       // the color-vision correction ran this frame (M-ACCESS)
    bool dof = false;                // depth of field blurred this frame
    bool motion_blur = false;        // motion blur smeared this frame
    u32 batches = 0;                 // instance batches drawn (M-VISUAL 9)
    bool batches_gpu = false;        // ... culled by the compute pass (false: on the CPU)
    bool occlusion = false;          // ... and tested against the depth pyramid
    u32 occluder_draws = 0;          // draws of the occluder depth pass
    u32 batch_views = 0;             // views culled: the main view, then each cascade with casters
    u32 batch_visible = 0;           // CPU path only: instances drawn in the main view (the GPU's count stays on the GPU)
    u32 batch_draws = 0;             // instanced draws over all passes
    u32 decals = 0;                  // decals the main pass applied
    u32 particle_emitters = 0;       // emitters simulated and drawn
    u32 particles_spawned = 0;       // particles spawned this frame
    u32 joints = 0;          // joint matrices uploaded for skinned meshes
    u32 bloom_levels = 0;
    bool fxaa = false;
    bool upscaled = false;           // the spatial upsample drew the output (M-VISUAL2 2)
    bool sharpened = false;          // the contrast-adaptive sharpen drew the output
    u32 transmissive = 0;            // draws that refracted the opaque frame (M-VISUAL2 4)
    // Automatic instancing (M-VISUAL3 5a). main_draws counts an instanced run once.
    u32 runs_instanced = 0;          // instanced draws of the main pass
    u32 draws_instanced = 0;         // ... the scene draws they replaced (main_draws fell by this - runs_instanced)
    u32 shadow_runs_instanced = 0;   // instanced draws over all cascades and local shadow tiles
    u32 shadow_draws_instanced = 0;  // ... the caster draws they replaced
    u32 instancing_overflow = 0;     // runs of any pass drawn singly because the frame's instancing budget ran out
    u32 instancing_bytes = 0;        // per-copy data uploaded this record()
    // M-CONTENT 2: the scene draws of the main pass that did not take part in an instanced run, by DrawCause
    // (core/draw_cause.h); with draws_instanced they add up to scene.draws().size(). Deterministic.
    u32 draw_causes[kDrawCauseCount] = {};
};

// Milliseconds per stage of one completed frame; -1 = not measured.
struct Forward3DGpuTimes {
    u64 frame_index = ~u64(0);
    f64 clusters = -1;  // the GPU light assignment (0 when it ran on the CPU)
    f64 ssao = -1;      // prepass + occlusion + blur (0 when the scene has no SSAO)
    f64 ssr = -1;       // the reflection trace, and the prepass when SSAO is off (0 when the scene has no SSR)
    f64 contact = -1;   // the contact-shadow march, and the prepass when SSAO and SSR are off (0 when off)
    f64 shadow = -1, main = -1, resolve = -1;
    f64 taa = -1;       // velocity prepass + TAA resolve (0 when the scene has no TAA)
    f64 gi = -1;        // irradiance-volume update (0 when the scene has no volume)
    f64 volumetrics = -1;  // froxel injection + integration (0 when the scene has no volumetric fog)
    f64 post_fx = -1;      // exposure histogram + depth of field + motion blur (0 when the scene has none)
    f64 scale = -1;        // occluder depth + pyramid + batch culling + particle simulation (M-VISUAL 9)
    f64 bloom = -1, tonemap = -1, fxaa = -1, total = -1;
    f64 post() const { return bloom < 0 ? -1 : bloom + tonemap + (fxaa < 0 ? 0 : fxaa); }
};

class Forward3D {
public:
    static constexpr u32 kStageCount = 15;    // timestamps written: kStageCount + 1
    static constexpr u32 kMaxBloomLevels = 6;

    Forward3D() = default;
    Forward3D(const Forward3D&) = delete;
    Forward3D& operator=(const Forward3D&) = delete;
    ~Forward3D();

    ResultV init(rhi::RenderDevice& device, const TextureStore& textures, const Forward3DDesc& desc);
    void shutdown();
    bool initialized() const { return device_ != nullptr; }

    // M-VIEWS: several views a frame. Each view slot keeps its own size-dependent targets and its
    // temporal state (TAA and SSR history, the velocity prepass's previous view, auto exposure,
    // the occlusion pyramid); pipelines, meshes, IBL, probes, the GI volume, the shadow atlas and
    // the particle rings are shared. Slot 0 is the main view (the single-camera path).
    static constexpr u32 kMaxViews = 8;
    struct ViewOutput {
        u32 slot = 0;        // < kMaxViews
        // Pixels of the target the finished frame goes to (top-left origin); zero size = the whole
        // target. A sub-rect loads what the target holds (LoadOp::Load) instead of discarding it,
        // so the caller clears the target once before its first view.
        rhi::Rect2D rect{};
        // The frame's first record(): it runs the once-a-frame work (the particle simulation, the
        // GI volume's rolling update). Later views of the same frame only draw.
        bool first = true;
        // The target holds linear color (a float render texture that materials and sprites sample):
        // the final pass skips the sRGB encode the window and the editor targets need.
        bool linear_output = false;
    };

    // Records every pass for a built scene. Outside any pass; `target` null = backbuffer.
    // Leaves the target holding the final frame (2D/HUD passes may follow with LoadOp::Load).
    // The scene's viewport is the output size (out.rect's size when it has one).
    void record(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target = {}) { record(cmd, scene, target, ViewOutput{}); }
    void record(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target, const ViewOutput& out);
    // Destroys a view slot's targets and history (the next record() into it starts afresh). The
    // RHI retires the textures past the frames in flight.
    void release_view(u32 slot);
    bool view_alive(u32 slot) const { return slot == 0 || (slot < kMaxViews && views_[slot - 1].alive); }

    // Per-stage GPU times from the device's newest completed timestamps.
    Forward3DGpuTimes gpu_times(const rhi::GpuTimestamps& ts) const;

    const Forward3DStats& last_stats() const { return stats_; }
    const Forward3DDesc& desc() const { return desc_; }
    // D32 2D array: kMaxCascades cascade layers, then the local-light atlas layers.
    rhi::TextureHandle shadow_map() const { return shadow_map_; }
    rhi::TextureHandle hdr_target() const { return hdr_; }
    // True when record() assigns Forward+ lights on the GPU (desc + device compute).
    bool gpu_light_clusters() const { return !cluster_build_.is_null(); }
    // Image-based lighting textures (M-VISUAL 2); null until the first frame with Sky::ibl.
    static constexpr u32 kIblLayers = 5;      // pre-filtered roughness 0.2, 0.4, ... 1.0 (after a sharp layer)
    static constexpr u32 kIblEnvWidth = 128;  // equirect, 2:1
    static constexpr u32 kIblLutSize = 64;
    static constexpr u32 kProbeFaceSize = 128;
    // Layers per environment slot in ibl_env(): the sharp source, then the pre-filtered ones.
    static constexpr u32 kProbeLayers = kIblLayers + 1;
    static constexpr u32 probe_base_layer(u32 probe) { return (probe + 1) * kProbeLayers; }
    // RGBA16F 2D array: the sky's slot (kProbeLayers layers), then one per probe once a scene has
    // reflection probes.
    rhi::TextureHandle ibl_env() const { return ibl_env_; }
    // Baking (M-VISUAL 2): record_probe_export() draws probe slot `probe` as one RGBM RGBA8 strip
    // of probe_export_size() (layer l at rows [l, l + 1) * kIblEnvWidth/2 from v 0) into
    // probe_export_target(); read it back after submit. False when the slot holds nothing yet.
    static constexpr f32 kProbeRgbmRange = 16.0f;
    static constexpr rhi::Extent2D probe_export_size() { return {kIblEnvWidth, kIblEnvWidth / 2 * kProbeLayers}; }
    bool record_probe_export(rhi::CommandBuffer& cmd, u32 probe);
    // Forgets every probe slot, so the next record() captures (or loads) each probe again: a bake
    // must not reuse a capture of another draw set.
    void invalidate_probes() {
        for (u64& k : probe_keys_) k = 0;
    }
    rhi::TextureHandle probe_export_target() const { return probe_export_; }
    // The sharp equirect of the last probe captured (RGBA16F, kIblEnvWidth × kIblEnvWidth/2).
    rhi::TextureHandle probe_capture() const { return probe_capture_; }
    rhi::TextureHandle ibl_brdf_lut() const { return ibl_lut_; } // RGBA16F, rg = F0 scale, bias
    rhi::TextureHandle ibl_sh() const { return ibl_sh_; }        // RGBA16F 9×1, irradiance / pi
    // SSAO (M-VISUAL 3): the blurred ambient visibility (r) at half the 3D target's size; null until
    // the first frame with AmbientOcclusion::enabled.
    rhi::TextureHandle ssao() const { return ao_a_; }
    // SSR (M-VISUAL2 1): RGBA16F at half the 3D target's size, rgb = last frame's light at the hit,
    // a = its confidence (0 = a miss); null until the first frame with ScreenSpaceReflections::enabled.
    rhi::TextureHandle ssr() const { return ssr_; }
    // RGBA8 half size: r = the SSAO visibility (1 without SSAO), g = the sun's contact visibility.
    // Valid after a frame with contact shadows (Stats::contact_shadows).
    rhi::TextureHandle contact_shadows() const { return ao_b_; }
    // TAA (M-VISUAL 5): the velocity target (RGBA16F at the 3D size: rg = uv motion, b = covered, a =
    // view depth) and the newest history (RGBA16F at the output size); null until the first frame
    // with PostFx::taa (or motion blur / depth of field for the velocity).
    rhi::TextureHandle velocity() const { return velocity_; }
    rhi::TextureHandle taa_history() const { return history_[history_index_]; }
    // Drops the history: call on a camera cut so the next frame does not blend the old view.
    void reset_history() { history_valid_ = false; }
    // The irradiance volume (M-VISUAL 6): RGBA16F gi_volume_size() (4 texels per probe, x-major,
    // row y + counts[1] * z), null until the first frame with GiVolume::enabled. Captures are 6
    // kGiFaceSize² faces per probe, at most kGiMaxBatchProbes probes per frame. A new volume fills
    // over as many frames as that takes and lights the scene from the frame it is full.
    static constexpr u32 kGiFaceSize = 16;
    static constexpr u32 kGiMaxBatchProbes = 32;
    static constexpr u32 kGiMaxCaptureLights = 32;  // point / spot lights a probe's captures see (the strongest for it)
    // A frame's captures also stop at the probe that spends this many uniform uploads (a draw is one, a
    // face three; the first probe always goes): the per-frame uniform ring (1 MiB by default) also
    // carries the frame's own passes. A face draws at most kGiMaxFaceDraws meshes, the widest it sees.
    static constexpr u32 kGiCaptureBudget = 1024;
    static constexpr u32 kGiMaxFaceDraws = 96;
    static constexpr f32 kGiRgbmRange = 16.0f;
    rhi::TextureHandle gi_volume() const { return gi_volume_; }
    // Baking: record_gi_export() draws the volume as RGBM RGBA8 of gi_volume_size() (row 0 = v 0)
    // into gi_export_target(); read it back after submit. False when the volume holds nothing yet.
    bool record_gi_export(rhi::CommandBuffer& cmd);
    rhi::TextureHandle gi_export_target() const { return gi_export_; }
    // Forgets the volume, so the next record() fills (or loads) it again.
    void invalidate_gi() { gi_key_ = 0; }
    // Auto exposure (M-VISUAL 8): the storage buffer of kExposureBins u32 histogram bins (zeroed
    // after each frame's reduction), then f32 exposure (the auto factor applied), f32 average
    // luminance and u32 frames adapted. Null when Forward3DDesc::post_fx is off or there is no compute.
    static constexpr u32 kExposureBins = 64;
    static constexpr f32 kExposureMinLog2 = -12.0f, kExposureMaxLog2 = 12.0f;  // the bins' log2 luminance range
    rhi::BufferHandle exposure_buffer() const { return exposure_buf_; }
    // Forgets the adapted exposure: the next auto-exposure frame snaps to its target (a camera cut).
    void reset_exposure() { exposure_reset_ = true; }
    // Scale (M-VISUAL 9). The batches' indirect commands of the last frame (DrawIndexedIndirectCommand
    // records, written on the GPU) and where a (view, batch, level, submesh) lies in them: view 0 is
    // the main view, view 1 + c cascade c. ~0u when that view or batch was not culled on the GPU.
    rhi::BufferHandle batch_commands() const { return inst_args_; }
    u32 batch_command(u32 view, u32 batch, u32 level, u32 submesh) const;
    // The depth pyramid's levels (level 0: the occluder depth, RGBA32F view depth in r; each next
    // level kHizRatio times smaller, the farthest depth under each texel); null until the first frame
    // with occlusion culling.
    static constexpr u32 kHizLevels = 5;
    static constexpr u32 kHizRatio = 4;
    rhi::TextureHandle hiz_level(u32 level) const { return level == 0 ? occ_color_ : level < kHizLevels ? hiz_[level - 1] : rhi::TextureHandle{}; }
    // A particle emitter's ring (two vec4 per particle: position + age, velocity + lifetime), or null.
    rhi::BufferHandle particle_buffer(u32 emitter_id) const;

private:
    struct ViewState;
    void record_view(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target, const ViewOutput& out);
    void swap_view(ViewState& v);
    void destroy_view_resources();
    ResultV create_pipelines();
    void ensure_targets(rhi::Extent2D size);
    void destroy_targets();
    void timestamp(rhi::CommandBuffer& cmd, u32 stage);
    void record_clusters(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_ibl(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void update_atmosphere(const Scene3D& scene);
    void record_volumetrics(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void vol_params(const Scene3D& scene, Forward3DPassU& pu) const;
    void bind_main_state(rhi::CommandBuffer& cmd, const Scene3D& scene, bool apply_fog);
    rhi::TextureHandle sky_texture_override() const { return atmo_on_ ? atmo_tex_ : rhi::TextureHandle{}; }
    void record_probes(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_prepass(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_ssao(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_ssr(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_contact(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_shadows(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_local_shadows(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void shadow_draw(rhi::CommandBuffer& cmd, const math::mat4& view_proj, const Scene3D::Draw& d, rhi::PipelineHandle& bound_pipe,
                     const GpuMesh*& bound);
    void record_main(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_velocity(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_taa(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_post_fx(rhi::CommandBuffer& cmd, const Scene3D& scene);
    rhi::TextureHandle post_fx_target(rhi::Extent2D size);
    void record_gi(rhi::CommandBuffer& cmd, const Scene3D& scene);
    u32 gi_capture(rhi::CommandBuffer& cmd, const Scene3D& scene, u32 first, u32 count, f32 weight);
    // The sky-and-IBL members of a lit pass's PassU and their textures (stand-ins when IBL is off).
    rhi::TextureHandle bind_ambient(rhi::CommandBuffer& cmd, const Scene3D& scene, bool probes, bool gi, Forward3DPassU& pu);
    // The local-light shadow records of this frame (bank kLocalShadows) and PassU::local.x.
    void bind_local_shadows(rhi::CommandBuffer& cmd, const Scene3D& scene, Forward3DPassU& pu);
    void record_post(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target);
    // What the current pass has bound, so consecutive draws skip redundant binds.
    struct DrawCache {
        rhi::PipelineHandle pipe;
        const GpuMesh* mesh = nullptr;
        u32 tex[4] = {~0u - 1, ~0u - 1, ~0u - 1, ~0u - 1};
        bool frame_at_ao = false;  // bank kAo holds the opaque frame's copy (a transmissive draw's)
    };
    // draws[begin, end) of the bound frame/pass state; indices >= opaque_end take the blend
    // pipelines. `main` counts them in the stats; `prepass` draws them all with the SSAO prepass;
    // `frame`: trans_src_ holds the opaque frame, so transmissive draws refract it (bank kAo).
    // `instanced`: draws[begin, end) are scene.draws() and the runs upload_runs() placed draw instanced.
    // Returns the draws issued.
    u32 draw_meshes(rhi::CommandBuffer& cmd, const Scene3D& scene, Span<const Scene3D::Draw> draws, u32 begin, u32 end,
                    u32 opaque_end, DrawCache& cache, bool main, bool prepass = false, bool frame = false,
                    bool instanced = false);
    // ---- automatic instancing (M-VISUAL3 5a)
    struct InstSlot {  // per draw: an instanced run starting there (count >= 2) and its first slot
        u32 count = 0;
        u32 slot = 0;
    };
    // Uploads the per-copy data of the scene's opaque runs for one pass (`velocity`: the model only; else
    // model and normal) within the budget and fills inst_slots_; false when nothing is drawn instanced.
    enum InstPass : u32 { kInstMain, kInstPrepass, kInstVelocity, kInstShadow, kInstPassCount };
    bool upload_runs(rhi::CommandBuffer& cmd, const Scene3D& scene, InstPass pass);
    void count_draw_causes(const Scene3D& scene);  // Forward3DStats::draw_causes, before the passes draw
    // Splits the frame's remaining instancing budget into this view's per-pass reservations (main first).
    void reserve_instancing(const Scene3D& scene);
    // The caster list `list` (indices into shadow_draws()) of one depth pass, in caster runs.
    void draw_casters(rhi::CommandBuffer& cmd, const Scene3D& scene, const math::mat4& view_proj, Span<const u32> list);
    // ---- scale (M-VISUAL 9, forward3d_scale.cpp)
    ResultV create_scale_pipelines();
    void destroy_scale();
    // Uploads changed batches, culls them for the view and the cascades (GPU or CPU), simulates the
    // particles. Before the passes that draw batches.
    void record_scale(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_occluders(rhi::CommandBuffer& cmd, const Scene3D& scene);
    void record_particles_sim(rhi::CommandBuffer& cmd, const Scene3D& scene);
    // The batches of `view` in a shadow pass (cascade view_proj) or the velocity prepass.
    void draw_batches_depth(rhi::CommandBuffer& cmd, const Scene3D& scene, u32 view, const math::mat4& view_proj, bool velocity);
    // The batches of the main view in the main pass or the SSAO prepass (ObjectU, forward3d.cpp).
    void draw_batches(rhi::CommandBuffer& cmd, const Scene3D& scene, DrawCache& cache, bool prepass);
    void draw_particles(rhi::CommandBuffer& cmd, const Scene3D& scene);
    // The decal records and textures of a lit pass (`on` false: stand-ins); returns the record count
    // for PassU::decals.x.
    u32 bind_decals(rhi::CommandBuffer& cmd, const Scene3D& scene, bool on);
    // Binds a batch's lists and issues the draw of (view, batch, level, submesh): indirect on the GPU
    // path, an instanced draw of the CPU list otherwise. False when there is nothing to draw.
    bool batch_draw(rhi::CommandBuffer& cmd, const GpuMesh& mesh, u32 view, u32 batch, u32 level, u32 submesh);
    // The mesh's vertex/index buffers and the batch's matrices and visible list.
    void bind_batch(rhi::CommandBuffer& cmd, const GpuMesh& mesh, u32 batch);

    rhi::RenderDevice* device_ = nullptr;
    const TextureStore* textures_ = nullptr;
    Forward3DDesc desc_;
    rhi::PipelineHandle shadow_pipe_;
    rhi::PipelineHandle mesh_cull_;      // back-face culled
    rhi::PipelineHandle mesh_two_sided_;
    rhi::PipelineHandle mesh_blend_;
    rhi::PipelineHandle terrain_pipe_;  // splat-layered terrain nodes (M-TERRAIN 2), back-face culled
    rhi::PipelineHandle skinned_shadow_pipe_;  // FR-R3D-060: SkinnedMeshVertex twins of the above
    rhi::PipelineHandle skinned_cull_;
    rhi::PipelineHandle skinned_two_sided_;
    rhi::PipelineHandle skinned_blend_;
    rhi::PipelineHandle sky_;
    rhi::PipelineHandle bloom_down_;
    rhi::PipelineHandle bloom_up_;       // additive
    rhi::PipelineHandle tonemap_;
    rhi::PipelineHandle fxaa_;
    rhi::PipelineHandle cluster_build_;  // compute; null = CPU assignment
    rhi::PipelineHandle ao_prepass_;     // null when Forward3DDesc::ssao is off
    rhi::PipelineHandle ao_prepass_skinned_;
    rhi::PipelineHandle ao_pipe_;
    rhi::PipelineHandle ao_blur_;
    rhi::TextureHandle ao_gbuf_;         // RGBA16F half size: view normal, view depth
    rhi::TextureHandle ao_depth_;        // D32 half size (prepass visibility only)
    rhi::TextureHandle ao_a_;            // RGBA8 half size: occlusion, then the blurred result
    rhi::TextureHandle ao_b_;            // RGBA8 half size: the horizontal blur, then the contact shadows
    rhi::TextureHandle ao_off_;          // 1×1 stand-in bound at bank::kAo while SSAO is off
    bool prepass_done_ = false;          // this frame's half-size prepass ran (SSAO, SSR or contact shadows)
    rhi::PipelineHandle ssr_pipe_;       // null when Forward3DDesc::ssr is off
    rhi::TextureHandle ssr_;             // RGBA16F half size: reflected light, confidence
    math::mat4 ssr_prev_vp_ = math::mat4::identity();  // last frame's jittered view-proj
    bool ssr_prev_valid_ = false;        // hdr_ holds a frame drawn with ssr_prev_vp_
    rhi::PipelineHandle contact_pipe_;   // null when Forward3DDesc::contact_shadows is off; writes ao_b_
    rhi::PipelineHandle frame_copy_;     // the opaque frame into trans_src_ when the target is not multisampled
    rhi::TextureHandle trans_src_;       // the opaque frame transmissive draws refract (M-VISUAL2 4)
    rhi::TextureHandle ao_bound_;        // what the main pass bound at bank::kAo (restored after a transmissive draw)
    rhi::PipelineHandle ibl_prefilter_;  // null when Forward3DDesc::ibl is off
    rhi::PipelineHandle ibl_sh_pipe_;
    rhi::PipelineHandle ibl_brdf_;
    rhi::TextureHandle ibl_env_;
    rhi::TextureHandle ibl_lut_;
    rhi::TextureHandle ibl_sh_;
    rhi::TextureHandle ibl_off_env_;     // 1×1 stand-ins bound while IBL is off (Metal validation):
    rhi::TextureHandle ibl_off_2d_;      // the env array, and the LUT and SH slots
    rhi::SamplerHandle env_sampler_;     // linear, wraps around the equirect seam
    u64 ibl_key_ = 0;                    // the sky the IBL textures were baked from; 0 = none
    rhi::TextureHandle atmo_tex_;        // the atmosphere's sky, equirect RGBA16F with mips (a new one per bake)
    u64 atmo_key_ = 0;                   // what it was baked from
    bool atmo_on_ = false;               // this frame draws it
    static constexpr u32 kVolAtlasCols = 8;  // froxel slices per atlas row
    rhi::PipelineHandle vol_inject_, vol_integrate_;
    rhi::TextureHandle vol_media_;       // per froxel: rgb scattered light per meter, a extinction (RGBA16F atlas)
    rhi::TextureHandle vol_light_;       // integrated: rgb light toward the camera, a transmittance
    rhi::Extent2D vol_grid_;             // froxel columns × rows this frame
    u32 vol_slices_ = 0;
    bool local_shadows_ = false;         // the atlas layers exist and r.local_shadows is on (this frame)
    bool local_mismatch_logged_ = false;
    bool ibl_own_source_ = false;        // baked from Sky::ibl_texture rather than the visible sky
    rhi::PipelineHandle probe_reproject_;  // six faces -> equirect
    rhi::PipelineHandle probe_prefilter_;  // equirect -> GGX layers
    rhi::PipelineHandle probe_encode_;     // slot -> RGBM strip (bake export)
    rhi::PipelineHandle probe_decode_;     // baked RGBM layer -> slot layer
    rhi::TextureHandle probe_export_;    // RGBA8 probe_export_size()
    rhi::TextureHandle probe_faces_;     // RGBA16F 2D array, 6 layers of kProbeFaceSize²
    rhi::TextureHandle probe_depth_;     // D32 2D array, 6 layers
    rhi::TextureHandle probe_capture_;   // RGBA16F equirect: the sharp capture the layers filter
    u32 ibl_env_layers_ = 0;
    u64 probe_keys_[kMaxReflectionProbes] = {};  // what each slot was captured from; 0 = none
    rhi::BufferHandle cluster_ranges_;   // GPU assignment outputs (banks 4 and 6)
    rhi::BufferHandle cluster_indices_;
    u32 cluster_ranges_cap_ = 0;         // clusters
    u32 cluster_indices_cap_ = 0;        // indices
    rhi::SamplerHandle material_sampler_;
    rhi::SamplerHandle shadow_sampler_;
    rhi::SamplerHandle point_sampler_;
    rhi::SamplerHandle linear_clamp_;
    rhi::TextureHandle shadow_map_;
    rhi::TextureHandle hdr_msaa_;
    rhi::TextureHandle depth_;
    rhi::TextureHandle hdr_;
    rhi::TextureHandle ldr_;
    // M-VISUAL2 2: the display-space frame before the spatial upsample / sharpen (at the post size),
    // and the upsampled frame before the sharpen (at the output size); created on first use.
    rhi::TextureHandle fin_src_;
    rhi::TextureHandle fin_up_;
    rhi::Extent2D fin_src_size_{};
    rhi::Extent2D fin_up_size_{};
    rhi::PipelineHandle upscale_pipe_;
    rhi::PipelineHandle sharpen_pipe_;
    rhi::TextureHandle bloom_[kMaxBloomLevels];
    rhi::Extent2D bloom_size_[kMaxBloomLevels];
    u32 bloom_levels_ = 0;
    rhi::Extent2D size_;
    // TAA (M-VISUAL 5)
    rhi::PipelineHandle velocity_pipe_;  // null when Forward3DDesc::taa is off
    rhi::PipelineHandle velocity_skinned_;
    // Automatic instancing (M-VISUAL3 5a): null when Forward3DDesc::instancing is off (or a twin's base is null).
    rhi::PipelineHandle mesh_cull_inst_;
    rhi::PipelineHandle mesh_two_sided_inst_;
    rhi::PipelineHandle ao_prepass_inst_;
    rhi::PipelineHandle shadow_inst_;
    rhi::PipelineHandle velocity_inst_;
    u32 inst_frame_left_ = 0;               // bytes of the frame's instancing budget no view has reserved
    u32 inst_budget_[kInstPassCount] = {};  // this view's reservation per pass, bytes left
    Array<InstSlot> inst_slots_;   // per opaque draw of the pass being drawn
    Array<math::mat4> inst_data_;  // the pass's per-copy block
    Array<u32> caster_sorted_;
    Array<DrawRun> caster_runs_;
    rhi::PipelineHandle taa_pipe_;
    rhi::TextureHandle velocity_;        // RGBA16F, 3D size
    rhi::TextureHandle velocity_depth_;  // D32, 3D size
    rhi::TextureHandle history_[2];      // RGBA16F, output size
    rhi::Extent2D history_size_;
    u32 history_index_ = 0;              // the newest history
    bool history_valid_ = false;
    bool taa_on_ = false;                // this frame: jittered and resolved
    u32 taa_frame_ = 0;                  // Halton(2, 3) index
    math::mat4 jitter_ = math::mat4::identity();  // clip-space shift of this frame's projection
    math::vec2 jitter_uv_{0.0f, 0.0f};
    math::mat4 prev_view_proj_ = math::mat4::identity();  // last velocity frame's, unjittered
    rhi::TextureHandle post_source_;     // what bloom and tonemap read: hdr_, or the TAA output
    rhi::Extent2D post_size_;
    bool velocity_on_ = false;           // this frame ran the velocity prepass
    bool velocity_prev_valid_ = false;   // prev_view_proj_ is last frame's (a velocity frame came before)
    math::mat4 velocity_prev_vp_used_ = math::mat4::identity();  // the previous view-proj this frame's prepass used
    // Post effects (M-VISUAL 8)
    rhi::PipelineHandle exposure_hist_;  // compute; null when Forward3DDesc::post_fx is off or no compute
    rhi::PipelineHandle exposure_reduce_;
    rhi::PipelineHandle dof_pipe_;
    rhi::PipelineHandle motion_blur_pipe_;
    rhi::BufferHandle exposure_buf_;     // histogram + adapted exposure (see exposure_buffer())
    bool exposure_reset_ = true;
    rhi::TextureHandle fx_[2];           // RGBA16F ping-pong at fx_size_ (depth of field, motion blur)
    rhi::Extent2D fx_size_;
    u32 fx_next_ = 0;
    // Scale (M-VISUAL 9)
    rhi::PipelineHandle inst_cull_;      // instanced kMeshFS, back-face culled
    rhi::PipelineHandle inst_two_sided_;
    rhi::PipelineHandle inst_prepass_;   // SSAO prepass; null when SSAO is off
    rhi::PipelineHandle inst_shadow_;
    rhi::PipelineHandle inst_velocity_;  // null when the velocity prepass is off
    rhi::PipelineHandle occluder_pipe_;  // the GPU path's (null without compute)
    rhi::PipelineHandle hiz_reduce_;
    rhi::PipelineHandle args_init_;
    rhi::PipelineHandle cull_cs_;
    rhi::PipelineHandle particle_sim_;   // null without compute
    rhi::PipelineHandle particle_add_;
    rhi::PipelineHandle particle_alpha_;
    struct BatchGpu {                    // a batch's matrices on the GPU, kept under its id
        u32 id = 0;
        u32 version = 0;
        const GpuMesh* mesh = nullptr;
        u32 mesh_generation = 0;
        u32 count = 0;
        u32 capacity = 0;
        rhi::BufferHandle transforms;
        u32 frame = 0;                   // batch_frame_ of the last record() that drew it
        bool seen = false;
    };
    Array<BatchGpu> batch_gpu_;
    u32 batch_frame_ = 0;                // frames recorded (first views)
    struct BatchView {                   // a (view, batch) of this frame
        u32 first_command = ~0u;         // GPU path: the commands of level 0, submesh 0
        u32 slot[asset::kMaxMeshLods] = {};   // first visible slot of each level
        u32 count[asset::kMaxMeshLods] = {};  // CPU path: instances in each level's list
        u32 submeshes = 0;               // commands per level
        bool culled = false;             // this view culled this batch
    };
    Array<BatchView> batch_views_;       // view * batches + batch
    Array<u32> batch_gpu_index_;         // per scene batch: its BatchGpu
    u32 batch_view_count_ = 0;
    u32 batch_count_ = 0;
    bool batches_gpu_ = false;
    rhi::BufferHandle inst_args_;        // indirect commands (GPU path)
    rhi::BufferHandle inst_visible_;     // visible lists (GPU path)
    u32 inst_args_cap_ = 0;              // commands
    u32 inst_visible_cap_ = 0;           // slots
    rhi::TextureHandle occ_color_;       // RGBA32F view depth of the occluders (pyramid level 0)
    rhi::TextureHandle occ_depth_;       // D32
    rhi::TextureHandle hiz_[kHizLevels - 1];
    rhi::Extent2D hiz_size_[kHizLevels];
    struct EmitterGpu {
        u32 id = 0;
        u32 capacity = 0;
        u32 seed = 0;
        rhi::BufferHandle particles;
        u32 cursor = 0;                  // the next ring slot to spawn into
        u64 spawned = 0;                 // particles spawned so far (the random stream)
        f32 carry = 0.0f;                // fractional spawns carried to the next frame
        bool seen = false;
    };
    Array<EmitterGpu> emitters_gpu_;
    // Irradiance volume (M-VISUAL 6)
    rhi::PipelineHandle gi_project_;     // null when Forward3DDesc::gi is off
    rhi::PipelineHandle gi_encode_;
    rhi::PipelineHandle gi_decode_;
    rhi::TextureHandle gi_volume_;       // RGBA16F gi_volume_size()
    rhi::TextureHandle gi_faces_;        // RGBA16F 2D array, 6 layers per probe of a batch
    rhi::TextureHandle gi_faces_depth_;  // D32, the same layers
    rhi::TextureHandle gi_export_;       // RGBA8 gi_volume_size()
    rhi::Extent2D gi_size_;
    u32 gi_face_layers_ = 0;
    u32 gi_shadow_layer_ = 0;            // shadow_map_ layer of the volume's sun fit; 0 = none yet
    u32 gi_cursor_ = 0;                  // the next probe the rolling update captures
    u32 gi_filled_ = 0;                  // probes the current fill has captured
    u64 gi_key_ = 0;                     // what the volume was filled from; 0 = nothing
    bool gi_shadowed_ = false;           // this frame's volume sun fit was rendered
    math::mat4 gi_shadow_vp_ = math::mat4::identity();
    f32 gi_shadow_texel_ = 0.0f;
    Forward3DStats stats_;
    ViewOutput out_;                     // the record() in progress

    // M-VIEWS: the members above that belong to one view. Slot 0's live in the members themselves;
    // record() swaps another slot's into them for the duration of its frame (handles and a few
    // matrices, no allocation), so the passes read one set of names whichever view they draw.
#define MCE_FORWARD3D_VIEW_STATE(X)                                                                              \
    X(hdr_msaa_) X(depth_) X(hdr_) X(ldr_) X(fin_src_) X(fin_up_) X(fin_src_size_) X(fin_up_size_) X(bloom_)    \
    X(bloom_size_) X(bloom_levels_) X(size_) X(ao_gbuf_) X(ao_depth_) X(ao_a_) X(ao_b_) X(ssr_) X(ssr_prev_vp_) \
    X(ssr_prev_valid_) X(trans_src_) X(vol_media_) X(vol_light_) X(velocity_) X(velocity_depth_) X(history_)    \
    X(history_size_) X(history_index_) X(history_valid_) X(taa_frame_) X(prev_view_proj_) X(velocity_prev_valid_) \
    X(velocity_prev_vp_used_) X(exposure_buf_) X(exposure_reset_) X(fx_) X(fx_size_) X(fx_next_) X(occ_color_)  \
    X(occ_depth_) X(hiz_) X(hiz_size_)
#define MCE_FORWARD3D_VIEW_FIELD(m) decltype(Forward3D::m) m{};
    struct ViewState {
        MCE_FORWARD3D_VIEW_STATE(MCE_FORWARD3D_VIEW_FIELD)
        bool alive = false;
    };
#undef MCE_FORWARD3D_VIEW_FIELD
    ViewState views_[kMaxViews - 1];     // slots 1 .. kMaxViews - 1
};

}  // namespace mce::render
