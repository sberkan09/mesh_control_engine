// NavMesh3D over Recast & Detour (ADR-0039). With world3d.cpp, the only translation units that may
// include Recast or Detour (the Recast wall in check_walls.py).
//
// The bake follows Recast's "temporary obstacles" pipeline: per tile, rasterize the triangles that
// touch it, filter, erode by the agent radius, mark the area volumes, split the compact heightfield
// into layers (one per storey) and store each layer compressed. The runtime loads the layers into
// a dtTileCache, which builds the dtNavMesh tiles from them and rebuilds a tile when an obstacle
// over it is added or removed; a dtCrowd moves the agents on that mesh.

#include "mce/physics3d/navmesh3d.h"

#include <DetourAlloc.h>
#include <DetourCommon.h>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourStatus.h>
#include <DetourTileCache.h>
#include <DetourTileCacheBuilder.h>
#include <Recast.h>
#include <RecastAlloc.h>

#include <cfloat>
#include <cmath>
#include <cstring>

#include "mce/hash.h"
#include "mce/log.h"
#include "mce/memory/allocator.h"

namespace mce::physics {

namespace {

// ---- allocation: Recast and Detour allocate through the engine heap ---------------------------------

void* nav_alloc(size_t size) { return mem::heap().allocate(size ? size : 1, mem::kDefaultAlign).ptr; }
void nav_free(void* p) {
    if (p) mem::heap().free(p);
}
void* rc_alloc(size_t size, rcAllocHint) { return nav_alloc(size); }
void* dt_alloc(size_t size, dtAllocHint) { return nav_alloc(size); }

void install_allocators() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    rcAllocSetCustom(rc_alloc, nav_free);
    dtAllocSetCustom(dt_alloc, nav_free);
}

// Recast reports through a context; route its errors to the log (no timers).
class NavContext final : public rcContext {
public:
    NavContext() : rcContext(true) { enableTimer(false); }

protected:
    void doLog(const rcLogCategory category, const char* msg, const int) override {
        if (category == RC_LOG_ERROR) MCE_LOG_WARN("navmesh bake: {}", msg);
    }
};

constexpr u16 kPolyFlagWalk = 1;
constexpr int kMaxLayersPerTile = 32;
constexpr int kMaxPathPolys = 512;
constexpr int kMaxStraight = 256;
constexpr int kQueryNodes = 4096;

// ---- the layer compressor: PackBits run-length coding --------------------------------------------
// Layers are mostly runs (flat heights, one area, few connections), so this keeps a bake a fraction
// of its raw size with a codec that is trivially deterministic.

struct PackBits final : dtTileCacheCompressor {
    int maxCompressedSize(const int size) override { return size + size / 128 + 2; }

    dtStatus compress(const unsigned char* in, const int size, unsigned char* out, const int max_out,
                      int* out_size) override {
        int o = 0;
        int i = 0;
        while (i < size) {
            int run = 1;
            while (i + run < size && run < 128 && in[i + run] == in[i]) ++run;
            if (run >= 2) {
                if (o + 2 > max_out) return DT_FAILURE | DT_BUFFER_TOO_SMALL;
                out[o++] = static_cast<unsigned char>(257 - run);
                out[o++] = in[i];
                i += run;
                continue;
            }
            int lit = 1;  // literals until the next run of 2 (or 128 bytes)
            while (i + lit < size && lit < 128 && !(i + lit + 1 < size && in[i + lit] == in[i + lit + 1])) ++lit;
            if (o + 1 + lit > max_out) return DT_FAILURE | DT_BUFFER_TOO_SMALL;
            out[o++] = static_cast<unsigned char>(lit - 1);
            std::memcpy(out + o, in + i, size_t(lit));
            o += lit;
            i += lit;
        }
        *out_size = o;
        return DT_SUCCESS;
    }

    dtStatus decompress(const unsigned char* in, const int size, unsigned char* out, const int max_out,
                        int* out_size) override {
        int o = 0;
        int i = 0;
        while (i < size) {
            const int c = in[i++];
            if (c < 128) {
                const int lit = c + 1;
                if (i + lit > size || o + lit > max_out) return DT_FAILURE;
                std::memcpy(out + o, in + i, size_t(lit));
                o += lit;
                i += lit;
            } else if (c > 128) {
                const int run = 257 - c;
                if (i >= size || o + run > max_out) return DT_FAILURE;
                std::memset(out + o, in[i++], size_t(run));
                o += run;
            }
        }
        *out_size = o;
        return DT_SUCCESS;
    }
};

// ---- the blob (.mcnav) ----------------------------------------------------------------------------
// Little-endian fields written one by one (no struct padding reaches the bytes):
//   "MCNV" u32 version, the settings, bmin[3] bmax[3], i32 tiles_x tiles_z, u32 layers, u32 links,
//   links (start[3] end[3] radius, u8 area, u8 bidirectional, u16 0, u32 user_id),
//   layers (u32 size, size bytes, zero padding to 4).

constexpr char kMagic[4] = {'M', 'C', 'N', 'V'};
constexpr u32 kVersion = 1;

class Writer {
public:
    explicit Writer(Array<byte>& out) : out_(out) {}
    void raw(const void* p, usize n) {
        const usize at = out_.size();
        out_.resize(at + n);
        if (n) std::memcpy(out_.data() + at, p, n);
    }
    void u8v(u8 v) { raw(&v, 1); }
    void u16v(u16 v) { raw(&v, 2); }
    void u32v(u32 v) { raw(&v, 4); }
    void i32v(i32 v) { raw(&v, 4); }
    void f32v(f32 v) { raw(&v, 4); }
    void vec(vec3 v) {
        f32v(v.x);
        f32v(v.y);
        f32v(v.z);
    }
    void pad4() {
        while (out_.size() % 4) u8v(0);
    }

private:
    Array<byte>& out_;
};

class Reader {
public:
    explicit Reader(Span<const byte> in) : in_(in) {}
    bool ok() const { return ok_; }
    usize pos() const { return pos_; }
    void raw(void* p, usize n) {
        if (!ok_ || pos_ + n > in_.size()) {
            ok_ = false;
            std::memset(p, 0, n);
            return;
        }
        std::memcpy(p, in_.data() + pos_, n);
        pos_ += n;
    }
    const byte* take(usize n) {
        if (!ok_ || pos_ + n > in_.size()) {
            ok_ = false;
            return nullptr;
        }
        const byte* p = in_.data() + pos_;
        pos_ += n;
        return p;
    }
    u8 u8v() {
        u8 v;
        raw(&v, 1);
        return v;
    }
    u16 u16v() {
        u16 v;
        raw(&v, 2);
        return v;
    }
    u32 u32v() {
        u32 v;
        raw(&v, 4);
        return v;
    }
    i32 i32v() {
        i32 v;
        raw(&v, 4);
        return v;
    }
    f32 f32v() {
        f32 v;
        raw(&v, 4);
        return v;
    }
    vec3 vec() {
        vec3 v;
        v.x = f32v();
        v.y = f32v();
        v.z = f32v();
        return v;
    }
    void pad4() {
        while (ok_ && pos_ % 4) u8v();
    }

private:
    Span<const byte> in_;
    usize pos_ = 0;
    bool ok_ = true;
};

void write_settings(Writer& w, const NavBakeSettings3D& s) {
    w.f32v(s.cell_size);
    w.f32v(s.cell_height);
    w.f32v(s.agent_height);
    w.f32v(s.agent_radius);
    w.f32v(s.agent_max_climb);
    w.f32v(s.agent_max_slope_deg);
    w.f32v(s.region_min_size);
    w.f32v(s.region_merge_size);
    w.f32v(s.edge_max_len);
    w.f32v(s.edge_max_error);
    w.f32v(s.detail_sample_dist);
    w.f32v(s.detail_sample_max_error);
    w.u32v(s.tile_size);
    w.u32v(s.max_obstacles);
    w.vec(s.bounds.min);
    w.vec(s.bounds.max);
}

NavBakeSettings3D read_settings(Reader& r) {
    NavBakeSettings3D s;
    s.cell_size = r.f32v();
    s.cell_height = r.f32v();
    s.agent_height = r.f32v();
    s.agent_radius = r.f32v();
    s.agent_max_climb = r.f32v();
    s.agent_max_slope_deg = r.f32v();
    s.region_min_size = r.f32v();
    s.region_merge_size = r.f32v();
    s.edge_max_len = r.f32v();
    s.edge_max_error = r.f32v();
    s.detail_sample_dist = r.f32v();
    s.detail_sample_max_error = r.f32v();
    s.tile_size = r.u32v();
    s.max_obstacles = r.u32v();
    s.bounds.min = r.vec();
    s.bounds.max = r.vec();
    return s;
}

struct BlobHeader {
    NavBakeSettings3D settings;
    vec3 bmin;
    vec3 bmax;
    i32 tiles_x = 0;
    i32 tiles_z = 0;
    u32 layers = 0;
    u32 links = 0;
};

bool settings_valid(const NavBakeSettings3D& s) {
    auto pos = [](f32 v) { return std::isfinite(v) && v > 0.0f; };
    return pos(s.cell_size) && pos(s.cell_height) && pos(s.agent_height) && std::isfinite(s.agent_radius) &&
           s.agent_radius >= 0.0f && std::isfinite(s.agent_max_climb) && s.agent_max_climb >= 0.0f &&
           s.agent_max_slope_deg >= 0.0f && s.agent_max_slope_deg < 90.0f && s.tile_size >= 8 && s.tile_size <= 200 &&
           s.max_obstacles >= 1 && s.max_obstacles <= 4096 && std::isfinite(s.edge_max_error) && s.edge_max_error >= 0.0f;
}

// Reads the header and links; false when the blob is not a navmesh bake.
bool read_header(Reader& r, BlobHeader& h, Array<NavLinkDesc3D>* links) {
    char magic[4];
    r.raw(magic, 4);
    if (!r.ok() || std::memcmp(magic, kMagic, 4) != 0) return false;
    if (r.u32v() != kVersion) return false;
    h.settings = read_settings(r);
    h.bmin = r.vec();
    h.bmax = r.vec();
    h.tiles_x = r.i32v();
    h.tiles_z = r.i32v();
    h.layers = r.u32v();
    h.links = r.u32v();
    if (!r.ok() || !settings_valid(h.settings)) return false;
    if (h.tiles_x <= 0 || h.tiles_z <= 0 || i64(h.tiles_x) * h.tiles_z > 4096) return false;
    if (h.layers > u32(h.tiles_x * h.tiles_z * kMaxLayersPerTile) || h.links > 65536) return false;
    for (u32 i = 0; i < h.links; ++i) {
        NavLinkDesc3D l;
        l.start = r.vec();
        l.end = r.vec();
        l.radius = r.f32v();
        l.area = r.u8v();
        l.bidirectional = r.u8v() != 0;
        r.u16v();
        l.user_id = r.u32v();
        if (links) links->push_back(l);
    }
    return r.ok();
}

u8 to_rc_area(u8 area) {
    if (area == kNavAreaGround) return RC_WALKABLE_AREA;
    return area < kNavAreaCount ? area : RC_WALKABLE_AREA;
}

u32 next_pow2(u32 v) {
    u32 p = 1;
    while (p < v) p <<= 1;
    return p;
}

u32 ilog2(u32 v) {
    u32 r = 0;
    while ((1u << r) < v) ++r;
    return r;
}

// ---- one tile of the bake -----------------------------------------------------------------------------

struct TileInput {
    const NavGeometry3D* geom = nullptr;
    const rcConfig* cfg = nullptr;
    Span<const u32> tris;  // triangle indices into geom touching this tile
};

// Appends the tile's layers to `w`; returns how many, or -1 on failure.
int bake_tile(NavContext& ctx, const TileInput& in, int tx, int tz, PackBits& comp, Writer& w) {
    const rcConfig& cfg = *in.cfg;
    const NavGeometry3D& g = *in.geom;
    if (in.tris.empty()) return 0;

    rcConfig tcfg = cfg;
    const f32 tcs = f32(cfg.tileSize) * cfg.cs;
    tcfg.bmin[0] = cfg.bmin[0] + f32(tx) * tcs - f32(cfg.borderSize) * cfg.cs;
    tcfg.bmin[2] = cfg.bmin[2] + f32(tz) * tcs - f32(cfg.borderSize) * cfg.cs;
    tcfg.bmax[0] = cfg.bmin[0] + f32(tx + 1) * tcs + f32(cfg.borderSize) * cfg.cs;
    tcfg.bmax[2] = cfg.bmin[2] + f32(tz + 1) * tcs + f32(cfg.borderSize) * cfg.cs;

    struct Scope {
        rcHeightfield* solid = nullptr;
        rcCompactHeightfield* chf = nullptr;
        rcHeightfieldLayerSet* lset = nullptr;
        ~Scope() {
            rcFreeHeightField(solid);
            rcFreeCompactHeightfield(chf);
            rcFreeHeightfieldLayerSet(lset);
        }
    } s;

    s.solid = rcAllocHeightfield();
    if (!s.solid || !rcCreateHeightfield(&ctx, *s.solid, tcfg.width, tcfg.height, tcfg.bmin, tcfg.bmax, tcfg.cs, tcfg.ch))
        return -1;

    const usize n = in.tris.size();
    Array<int> idx;
    idx.resize(n * 3);
    Array<u8> areas;
    areas.resize(n, 0);
    for (usize i = 0; i < n; ++i) {
        const u32 t = in.tris[i];
        idx[i * 3 + 0] = int(g.indices[t * 3 + 0]);
        idx[i * 3 + 1] = int(g.indices[t * 3 + 1]);
        idx[i * 3 + 2] = int(g.indices[t * 3 + 2]);
    }
    const f32* verts = &g.vertices[0].x;
    const int nverts = int(g.vertices.size());
    rcMarkWalkableTriangles(&ctx, tcfg.walkableSlopeAngle, verts, nverts, idx.data(), int(n), areas.data());
    for (usize i = 0; i < n; ++i) {
        const u8 src = g.areas[in.tris[i]];
        areas[i] = (src == kNavAreaBlocked || areas[i] == RC_NULL_AREA) ? u8(RC_NULL_AREA) : to_rc_area(src);
    }
    if (!rcRasterizeTriangles(&ctx, verts, nverts, idx.data(), areas.data(), int(n), *s.solid, tcfg.walkableClimb))
        return -1;

    rcFilterLowHangingWalkableObstacles(&ctx, tcfg.walkableClimb, *s.solid);
    rcFilterLedgeSpans(&ctx, tcfg.walkableHeight, tcfg.walkableClimb, *s.solid);
    rcFilterWalkableLowHeightSpans(&ctx, tcfg.walkableHeight, *s.solid);

    s.chf = rcAllocCompactHeightfield();
    if (!s.chf || !rcBuildCompactHeightfield(&ctx, tcfg.walkableHeight, tcfg.walkableClimb, *s.solid, *s.chf)) return -1;
    if (!rcErodeWalkableArea(&ctx, tcfg.walkableRadius, *s.chf)) return -1;
    for (const NavAreaVolume3D& v : g.volumes) {
        const f32 bmin[3] = {v.center.x - v.half.x, v.center.y - v.half.y, v.center.z - v.half.z};
        const f32 bmax[3] = {v.center.x + v.half.x, v.center.y + v.half.y, v.center.z + v.half.z};
        if (bmax[0] < tcfg.bmin[0] || bmin[0] > tcfg.bmax[0] || bmax[2] < tcfg.bmin[2] || bmin[2] > tcfg.bmax[2]) continue;
        rcMarkBoxArea(&ctx, bmin, bmax, v.area == kNavAreaBlocked ? u8(RC_NULL_AREA) : to_rc_area(v.area), *s.chf);
    }

    s.lset = rcAllocHeightfieldLayerSet();
    if (!s.lset || !rcBuildHeightfieldLayers(&ctx, *s.chf, tcfg.borderSize, tcfg.walkableHeight, *s.lset)) return -1;

    const int count = s.lset->nlayers < kMaxLayersPerTile ? s.lset->nlayers : kMaxLayersPerTile;
    for (int i = 0; i < count; ++i) {
        const rcHeightfieldLayer& layer = s.lset->layers[i];
        dtTileCacheLayerHeader header;
        std::memset(&header, 0, sizeof(header));  // its padding is copied into the blob
        header.magic = DT_TILECACHE_MAGIC;
        header.version = DT_TILECACHE_VERSION;
        header.tx = tx;
        header.ty = tz;
        header.tlayer = i;
        dtVcopy(header.bmin, layer.bmin);
        dtVcopy(header.bmax, layer.bmax);
        header.width = static_cast<unsigned char>(layer.width);
        header.height = static_cast<unsigned char>(layer.height);
        header.minx = static_cast<unsigned char>(layer.minx);
        header.maxx = static_cast<unsigned char>(layer.maxx);
        header.miny = static_cast<unsigned char>(layer.miny);
        header.maxy = static_cast<unsigned char>(layer.maxy);
        header.hmin = static_cast<unsigned short>(layer.hmin);
        header.hmax = static_cast<unsigned short>(layer.hmax);
        unsigned char* data = nullptr;
        int size = 0;
        if (dtStatusFailed(dtBuildTileCacheLayer(&comp, &header, layer.heights, layer.areas, layer.cons, &data, &size)))
            return -1;
        w.u32v(u32(size));
        w.raw(data, usize(size));
        w.pad4();
        dtFree(data);
    }
    return count;
}

// The tile cache hands each rebuilt tile to this: ground area, walk flags, the off-mesh links.
struct MeshProcess final : dtTileCacheMeshProcess {
    Array<f32> verts;  // 6 per link
    Array<f32> radii;
    Array<u8> dirs;
    Array<u8> areas;
    Array<u16> flags;
    Array<u32> ids;

    void process(dtNavMeshCreateParams* params, unsigned char* poly_areas, unsigned short* poly_flags) override {
        for (int i = 0; i < params->polyCount; ++i) {
            if (poly_areas[i] == DT_TILECACHE_WALKABLE_AREA) poly_areas[i] = kNavAreaGround;
            poly_flags[i] = kPolyFlagWalk;
        }
        const int n = int(radii.size());
        if (n == 0) return;
        params->offMeshConVerts = verts.data();
        params->offMeshConRad = radii.data();
        params->offMeshConDir = dirs.data();
        params->offMeshConAreas = areas.data();
        params->offMeshConFlags = flags.data();
        params->offMeshConUserID = ids.data();
        params->offMeshConCount = n;
    }
};

}  // namespace

// ---- NavGeometry3D ---------------------------------------------------------------------------------------

void NavGeometry3D::add_mesh(Span<const vec3> verts, Span<const u32> tris, const math::mat4& transform, u8 area) {
    const u32 base = u32(vertices.size());
    for (const vec3& v : verts) vertices.push_back(math::transform_point(transform, v));
    for (usize t = 0; t + 2 < tris.size(); t += 3) {
        if (tris[t] >= verts.size() || tris[t + 1] >= verts.size() || tris[t + 2] >= verts.size()) continue;
        indices.push_back(base + tris[t]);
        indices.push_back(base + tris[t + 1]);
        indices.push_back(base + tris[t + 2]);
        areas.push_back(area);
    }
}

void NavGeometry3D::add_box(const math::mat4& transform, vec3 h, u8 area) {
    const vec3 corners[8] = {{-h.x, -h.y, -h.z}, {h.x, -h.y, -h.z}, {h.x, -h.y, h.z}, {-h.x, -h.y, h.z},
                             {-h.x, h.y, -h.z},  {h.x, h.y, -h.z},  {h.x, h.y, h.z},  {-h.x, h.y, h.z}};
    // Counter-clockwise seen from outside.
    static constexpr u32 kTris[36] = {4, 7, 6, 4, 6, 5,   // +Y
                                      0, 1, 2, 0, 2, 3,   // -Y
                                      3, 2, 6, 3, 6, 7,   // +Z
                                      0, 4, 5, 0, 5, 1,   // -Z
                                      1, 5, 6, 1, 6, 2,   // +X
                                      0, 3, 7, 0, 7, 4};  // -X
    add_mesh(Span<const vec3>(corners, 8), Span<const u32>(kTris, 36), transform, area);
}

math::aabb3 NavGeometry3D::bounds() const {
    math::aabb3 b{{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
    for (const vec3& v : vertices) {
        b.min = {v.x < b.min.x ? v.x : b.min.x, v.y < b.min.y ? v.y : b.min.y, v.z < b.min.z ? v.z : b.min.z};
        b.max = {v.x > b.max.x ? v.x : b.max.x, v.y > b.max.y ? v.y : b.max.y, v.z > b.max.z ? v.z : b.max.z};
    }
    return b;
}

// ---- the bake ----------------------------------------------------------------------------------------------

bool bake_navmesh(const NavGeometry3D& g, const NavBakeSettings3D& s, Array<byte>& out, NavBakeStats3D* stats) {
    install_allocators();
    out.clear();
    if (!settings_valid(s)) {
        MCE_LOG_WARN("navmesh bake: settings out of range");
        return false;
    }
    if (g.indices.empty() || g.areas.size() * 3 != g.indices.size()) {
        MCE_LOG_WARN("navmesh bake: no triangles");
        return false;
    }

    math::aabb3 b = s.bounds;
    if (b.min.x > b.max.x || b.min.y > b.max.y || b.min.z > b.max.z) {
        b = g.bounds();
        b.max.y += s.agent_height;  // room to stand on the highest surface
    }

    rcConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.cs = s.cell_size;
    cfg.ch = s.cell_height;
    cfg.walkableSlopeAngle = s.agent_max_slope_deg;
    cfg.walkableHeight = int(std::ceil(s.agent_height / cfg.ch));
    cfg.walkableClimb = int(std::floor(s.agent_max_climb / cfg.ch));
    cfg.walkableRadius = int(std::ceil(s.agent_radius / cfg.cs));
    cfg.maxEdgeLen = int(s.edge_max_len / cfg.cs);
    cfg.maxSimplificationError = s.edge_max_error;
    cfg.minRegionArea = int(s.region_min_size * s.region_min_size);
    cfg.mergeRegionArea = int(s.region_merge_size * s.region_merge_size);
    cfg.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    cfg.tileSize = int(s.tile_size);
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    cfg.detailSampleDist = s.detail_sample_dist < 0.9f ? 0.0f : cfg.cs * s.detail_sample_dist;
    cfg.detailSampleMaxError = cfg.ch * s.detail_sample_max_error;
    cfg.bmin[0] = b.min.x;
    cfg.bmin[1] = b.min.y;
    cfg.bmin[2] = b.min.z;
    cfg.bmax[0] = b.max.x;
    cfg.bmax[1] = b.max.y;
    cfg.bmax[2] = b.max.z;
    if (cfg.width > 255 || cfg.height > 255) {
        MCE_LOG_WARN("navmesh bake: tile_size plus the agent border exceeds 255 cells");
        return false;
    }

    int gw = 0, gh = 0;
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &gw, &gh);
    const int ts = cfg.tileSize;
    const int tw = (gw + ts - 1) / ts;
    const int th = (gh + ts - 1) / ts;
    if (tw <= 0 || th <= 0 || i64(tw) * th > 4096) {
        MCE_LOG_WARN("navmesh bake: {} x {} tiles (max 4096); raise tile_size or cell_size", tw, th);
        return false;
    }

    // Bucket triangles by the tiles (with their borders) their xz bounds touch, in triangle order.
    Array<Array<u32>> buckets;
    buckets.resize(usize(tw) * usize(th));
    const f32 tcs = f32(ts) * cfg.cs;
    const f32 border = f32(cfg.borderSize) * cfg.cs;
    const u32 ntris = g.triangle_count();
    for (u32 t = 0; t < ntris; ++t) {
        const vec3& a = g.vertices[g.indices[t * 3 + 0]];
        const vec3& c1 = g.vertices[g.indices[t * 3 + 1]];
        const vec3& c2 = g.vertices[g.indices[t * 3 + 2]];
        const f32 minx = std::fmin(a.x, std::fmin(c1.x, c2.x)), maxx = std::fmax(a.x, std::fmax(c1.x, c2.x));
        const f32 minz = std::fmin(a.z, std::fmin(c1.z, c2.z)), maxz = std::fmax(a.z, std::fmax(c1.z, c2.z));
        int x0 = int(std::floor((minx - cfg.bmin[0] - border) / tcs));
        int x1 = int(std::floor((maxx - cfg.bmin[0] + border) / tcs));
        int z0 = int(std::floor((minz - cfg.bmin[2] - border) / tcs));
        int z1 = int(std::floor((maxz - cfg.bmin[2] + border) / tcs));
        if (x1 < 0 || z1 < 0 || x0 >= tw || z0 >= th) continue;
        x0 = x0 < 0 ? 0 : x0;
        z0 = z0 < 0 ? 0 : z0;
        x1 = x1 >= tw ? tw - 1 : x1;
        z1 = z1 >= th ? th - 1 : z1;
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) buckets[usize(z) * usize(tw) + usize(x)].push_back(t);
    }

    Writer w(out);
    w.raw(kMagic, 4);
    w.u32v(kVersion);
    NavBakeSettings3D stored = s;
    stored.bounds = b;
    write_settings(w, stored);
    w.vec(b.min);
    w.vec(b.max);
    w.i32v(tw);
    w.i32v(th);
    const usize layers_at = out.size();
    w.u32v(0);  // patched below
    w.u32v(u32(g.links.size()));
    for (const NavLinkDesc3D& l : g.links) {
        w.vec(l.start);
        w.vec(l.end);
        w.f32v(l.radius);
        w.u8v(l.area < kNavAreaCount ? l.area : kNavAreaGround);
        w.u8v(l.bidirectional ? 1 : 0);
        w.u16v(0);
        w.u32v(l.user_id);
    }

    NavContext ctx;
    PackBits comp;
    u32 layers = 0;
    for (int z = 0; z < th; ++z) {
        for (int x = 0; x < tw; ++x) {
            TileInput in;
            in.geom = &g;
            in.cfg = &cfg;
            in.tris = buckets[usize(z) * usize(tw) + usize(x)].span();
            const int n = bake_tile(ctx, in, x, z, comp, w);
            if (n < 0) {
                MCE_LOG_WARN("navmesh bake: tile ({}, {}) failed", x, z);
                out.clear();
                return false;
            }
            layers += u32(n);
        }
    }
    std::memcpy(out.data() + layers_at, &layers, 4);

    if (stats) {
        stats->tiles_x = u32(tw);
        stats->tiles_z = u32(th);
        stats->layers = layers;
        stats->triangles = ntris;
        stats->bytes = out.size();
    }
    return true;
}

bool navmesh_blob_valid(Span<const byte> blob) {
    Reader r(blob);
    BlobHeader h;
    if (!read_header(r, h, nullptr)) return false;
    for (u32 i = 0; i < h.layers; ++i) {
        const u32 size = r.u32v();
        if (size < u32(sizeof(dtTileCacheLayerHeader)) || !r.take(size)) return false;
        r.pad4();
    }
    return r.ok() && r.pos() == blob.size();
}

// ---- NavMesh3D ------------------------------------------------------------------------------------------

struct NavMesh3D::Impl {
    struct Agent {
        NavAgentParams3D params;
        vec3 target;
        bool live = false;
        bool has_target = false;
        bool arrived = false;
        bool failed = false;
    };

    NavBakeSettings3D settings;
    math::aabb3 bounds;
    i32 tiles_x = 0;
    i32 tiles_z = 0;
    PackBits comp;
    dtTileCacheAlloc talloc;
    MeshProcess proc;
    dtTileCache* cache = nullptr;
    dtNavMesh* nav = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtCrowd* crowd = nullptr;
    dtQueryFilter filter;
    Array<Agent> agents;
    u32 obstacles = 0;

    Impl() { reset_filter(); }
    ~Impl() { release(); }

    void reset_filter() {
        filter = dtQueryFilter();
        filter.setIncludeFlags(kPolyFlagWalk);
        filter.setExcludeFlags(0);
        for (u32 i = 0; i < kNavAreaCount; ++i) filter.setAreaCost(int(i), 1.0f);
    }

    void release() {
        dtFreeCrowd(crowd);
        dtFreeNavMeshQuery(query);
        dtFreeNavMesh(nav);
        dtFreeTileCache(cache);
        crowd = nullptr;
        query = nullptr;
        nav = nullptr;
        cache = nullptr;
        agents.clear();
        obstacles = 0;
    }

    static const f32* fp(const vec3& v) { return &v.x; }
    static vec3 v3(const f32* p) { return {p[0], p[1], p[2]}; }

    dtPolyRef nearest(vec3 p, vec3 ext, vec3* out) const {
        dtPolyRef ref = 0;
        f32 pt[3] = {p.x, p.y, p.z};
        if (!query || dtStatusFailed(query->findNearestPoly(fp(p), fp(ext), &filter, &ref, pt)) || !ref) return 0;
        if (out) *out = v3(pt);
        return ref;
    }

    vec3 agent_extents(i32 i) const {
        const NavAgentParams3D& p = agents[usize(i)].params;
        const f32 r = p.radius > 0.5f ? p.radius : 0.5f;
        return {r * 2.0f, p.height, r * 2.0f};
    }

    dtCrowdAgentParams crowd_params(const NavAgentParams3D& p) const {
        dtCrowdAgentParams cp;
        std::memset(&cp, 0, sizeof(cp));
        cp.radius = p.radius;
        cp.height = p.height;
        cp.maxAcceleration = p.max_acceleration;
        cp.maxSpeed = p.max_speed;
        cp.collisionQueryRange = p.radius * 12.0f;
        cp.pathOptimizationRange = p.radius * 30.0f;
        cp.separationWeight = p.separation;
        u8 flags = 0;
        if (p.anticipate_turns) flags |= DT_CROWD_ANTICIPATE_TURNS;
        // Topology only: the visibility shortcut raycasts, and Detour's raycast ignores area
        // costs, so it would cut straight through an expensive area the path went around.
        if (p.optimize_path) flags |= DT_CROWD_OPTIMIZE_TOPO;
        if (p.avoid_agents) flags |= DT_CROWD_OBSTACLE_AVOIDANCE;
        if (p.separation > 0.0f) flags |= DT_CROWD_SEPARATION;
        cp.updateFlags = flags;
        cp.obstacleAvoidanceType = p.avoidance_quality > 3 ? 3 : p.avoidance_quality;
        cp.queryFilterType = 0;
        return cp;
    }

    bool valid_agent(i32 i) const { return crowd && i >= 0 && usize(i) < agents.size() && agents[usize(i)].live; }
};

NavMesh3D::NavMesh3D() : impl_(mem::heap(), mem::make<Impl>(mem::heap())) {}
NavMesh3D::~NavMesh3D() = default;
NavMesh3D::NavMesh3D(NavMesh3D&&) noexcept = default;
NavMesh3D& NavMesh3D::operator=(NavMesh3D&&) noexcept = default;

bool NavMesh3D::load(Span<const byte> blob, u32 max_agents) {
    install_allocators();
    Impl& m = *impl_;
    m.release();

    Reader r(blob);
    BlobHeader h;
    Array<NavLinkDesc3D> links;
    if (!read_header(r, h, &links)) {
        MCE_LOG_WARN("navmesh: not a navmesh bake");
        return false;
    }
    m.settings = h.settings;
    m.bounds = {h.bmin, h.bmax};
    m.tiles_x = h.tiles_x;
    m.tiles_z = h.tiles_z;

    m.proc = MeshProcess();
    for (const NavLinkDesc3D& l : links) {
        for (const vec3& p : {l.start, l.end}) {
            m.proc.verts.push_back(p.x);
            m.proc.verts.push_back(p.y);
            m.proc.verts.push_back(p.z);
        }
        m.proc.radii.push_back(l.radius);
        m.proc.dirs.push_back(l.bidirectional ? DT_OFFMESH_CON_BIDIR : 0);
        m.proc.areas.push_back(l.area);
        m.proc.flags.push_back(kPolyFlagWalk);
        m.proc.ids.push_back(l.user_id);
    }

    const u32 max_tiles = next_pow2(h.layers > u32(h.tiles_x * h.tiles_z) ? h.layers : u32(h.tiles_x * h.tiles_z));
    const u32 tile_bits = ilog2(max_tiles);
    if (tile_bits > 14) {
        MCE_LOG_WARN("navmesh: too many tile layers ({})", h.layers);
        return false;
    }
    u32 poly_bits = 22 - tile_bits;
    poly_bits = poly_bits > 16 ? 16 : poly_bits;

    dtTileCacheParams tp;
    std::memset(&tp, 0, sizeof(tp));
    tp.orig[0] = h.bmin.x;
    tp.orig[1] = h.bmin.y;
    tp.orig[2] = h.bmin.z;
    tp.cs = h.settings.cell_size;
    tp.ch = h.settings.cell_height;
    tp.width = int(h.settings.tile_size);
    tp.height = int(h.settings.tile_size);
    tp.walkableHeight = h.settings.agent_height;
    tp.walkableRadius = h.settings.agent_radius;
    tp.walkableClimb = h.settings.agent_max_climb;
    tp.maxSimplificationError = h.settings.edge_max_error;
    tp.maxTiles = int(max_tiles);
    tp.maxObstacles = int(h.settings.max_obstacles);
    m.cache = dtAllocTileCache();
    if (!m.cache || dtStatusFailed(m.cache->init(&tp, &m.talloc, &m.comp, &m.proc))) {
        m.release();
        return false;
    }

    dtNavMeshParams np;
    std::memset(&np, 0, sizeof(np));
    np.orig[0] = h.bmin.x;
    np.orig[1] = h.bmin.y;
    np.orig[2] = h.bmin.z;
    np.tileWidth = f32(h.settings.tile_size) * h.settings.cell_size;
    np.tileHeight = np.tileWidth;
    np.maxTiles = int(max_tiles);
    np.maxPolys = int(1u << poly_bits);
    m.nav = dtAllocNavMesh();
    if (!m.nav || dtStatusFailed(m.nav->init(&np))) {
        m.release();
        return false;
    }

    for (u32 i = 0; i < h.layers; ++i) {
        const u32 size = r.u32v();
        const byte* src = r.take(size);
        r.pad4();
        if (!src || size < u32(sizeof(dtTileCacheLayerHeader))) {
            MCE_LOG_WARN("navmesh: truncated bake");
            m.release();
            return false;
        }
        auto* data = static_cast<unsigned char*>(dtAlloc(size, DT_ALLOC_PERM));
        std::memcpy(data, src, size);
        if (dtStatusFailed(m.cache->addTile(data, int(size), DT_COMPRESSEDTILE_FREE_DATA, nullptr))) {
            dtFree(data);
            MCE_LOG_WARN("navmesh: a tile layer did not load");
            m.release();
            return false;
        }
    }
    for (i32 z = 0; z < h.tiles_z; ++z)
        for (i32 x = 0; x < h.tiles_x; ++x) m.cache->buildNavMeshTilesAt(x, z, m.nav);

    m.query = dtAllocNavMeshQuery();
    if (!m.query || dtStatusFailed(m.query->init(m.nav, kQueryNodes))) {
        m.release();
        return false;
    }

    const u32 agents = max_agents ? max_agents : 1;
    m.crowd = dtAllocCrowd();
    if (!m.crowd || !m.crowd->init(int(agents), 2.0f, m.nav)) {
        m.release();
        return false;
    }
    // Avoidance qualities 0..3, as Recast's demo tunes them.
    dtObstacleAvoidanceParams ap;
    std::memcpy(&ap, m.crowd->getObstacleAvoidanceParams(0), sizeof(ap));
    ap.velBias = 0.5f;
    ap.adaptiveDivs = 5;
    ap.adaptiveRings = 2;
    ap.adaptiveDepth = 1;
    m.crowd->setObstacleAvoidanceParams(0, &ap);
    ap.adaptiveDivs = 5;
    ap.adaptiveRings = 2;
    ap.adaptiveDepth = 2;
    m.crowd->setObstacleAvoidanceParams(1, &ap);
    ap.adaptiveDivs = 7;
    ap.adaptiveRings = 2;
    ap.adaptiveDepth = 3;
    m.crowd->setObstacleAvoidanceParams(2, &ap);
    ap.adaptiveDivs = 7;
    ap.adaptiveRings = 3;
    ap.adaptiveDepth = 3;
    m.crowd->setObstacleAvoidanceParams(3, &ap);
    *m.crowd->getEditableFilter(0) = m.filter;
    m.agents.resize(agents);
    return true;
}

void NavMesh3D::clear() { impl_->release(); }
bool NavMesh3D::loaded() const { return impl_->nav != nullptr; }
const NavBakeSettings3D& NavMesh3D::settings() const { return impl_->settings; }
math::aabb3 NavMesh3D::bounds() const { return impl_->bounds; }

void NavMesh3D::set_area_cost(u8 area, f32 cost) {
    Impl& m = *impl_;
    if (area == kNavAreaBlocked || area >= kNavAreaCount || !std::isfinite(cost) || cost <= 0.0f) return;
    m.filter.setAreaCost(area, cost);
    if (m.crowd) m.crowd->getEditableFilter(0)->setAreaCost(area, cost);
}

f32 NavMesh3D::area_cost(u8 area) const { return area < kNavAreaCount ? impl_->filter.getAreaCost(area) : 0.0f; }

std::optional<vec3> NavMesh3D::nearest_point(vec3 p, vec3 extents) const {
    vec3 out;
    if (!impl_->nearest(p, extents, &out)) return std::nullopt;
    return out;
}

u8 NavMesh3D::area_at(vec3 p, vec3 extents) const {
    const dtPolyRef ref = impl_->nearest(p, extents, nullptr);
    if (!ref) return kNavAreaBlocked;
    unsigned char area = 0;
    if (dtStatusFailed(impl_->nav->getPolyArea(ref, &area))) return kNavAreaBlocked;
    return area;
}

NavPath3D NavMesh3D::find_path(vec3 from, vec3 to, Array<vec3>& out) const {
    out.clear();
    const Impl& m = *impl_;
    const vec3 ext{2.0f, 4.0f, 2.0f};
    vec3 a, b;
    const dtPolyRef sref = m.nearest(from, ext, &a);
    const dtPolyRef eref = m.nearest(to, ext, &b);
    if (!sref || !eref) return NavPath3D::None;

    dtPolyRef polys[kMaxPathPolys];
    int npolys = 0;
    const dtStatus st = m.query->findPath(sref, eref, Impl::fp(a), Impl::fp(b), &m.filter, polys, &npolys, kMaxPathPolys);
    if (dtStatusFailed(st) || npolys == 0) return NavPath3D::None;
    vec3 end = b;
    const bool partial = polys[npolys - 1] != eref;
    if (partial) {
        f32 c[3];
        if (dtStatusFailed(m.query->closestPointOnPoly(polys[npolys - 1], Impl::fp(b), c, nullptr))) return NavPath3D::None;
        end = Impl::v3(c);
    }
    f32 pts[kMaxStraight * 3];
    int npts = 0;
    if (dtStatusFailed(m.query->findStraightPath(Impl::fp(a), Impl::fp(end), polys, npolys, pts, nullptr, nullptr, &npts,
                                                 kMaxStraight)))
        return NavPath3D::None;
    for (int i = 0; i < npts; ++i) out.push_back(Impl::v3(&pts[i * 3]));
    return partial ? NavPath3D::Partial : NavPath3D::Complete;
}

std::optional<vec3> NavMesh3D::raycast(vec3 from, vec3 to) const {
    const Impl& m = *impl_;
    vec3 a;
    const dtPolyRef sref = m.nearest(from, {2.0f, 4.0f, 2.0f}, &a);
    if (!sref) return std::nullopt;
    f32 t = 0.0f;
    f32 normal[3];
    dtPolyRef polys[kMaxPathPolys];
    int npolys = 0;
    if (dtStatusFailed(m.query->raycast(sref, Impl::fp(a), Impl::fp(to), &m.filter, &t, normal, polys, &npolys, kMaxPathPolys)))
        return std::nullopt;
    if (t > 1.0f) return std::nullopt;  // FLT_MAX: reached the end
    vec3 hit = a + (to - a) * t;
    f32 h = hit.y;
    if (npolys > 0 && dtStatusSucceed(m.query->getPolyHeight(polys[npolys - 1], Impl::fp(hit), &h))) hit.y = h;
    return hit;
}

u32 NavMesh3D::add_obstacle(vec3 center, vec3 half, f32 yaw) {
    Impl& m = *impl_;
    if (!m.cache) return 0;
    dtObstacleRef ref = 0;
    dtStatus st;
    if (yaw == 0.0f) {
        const vec3 bmin = center - half;
        const vec3 bmax = center + half;
        st = m.cache->addBoxObstacle(Impl::fp(bmin), Impl::fp(bmax), &ref);
    } else {
        st = m.cache->addBoxObstacle(Impl::fp(center), Impl::fp(half), yaw, &ref);
    }
    if (dtStatusFailed(st)) return 0;
    ++m.obstacles;
    return u32(ref);
}

void NavMesh3D::remove_obstacle(u32 id) {
    Impl& m = *impl_;
    if (!m.cache || !id) return;
    if (!m.cache->getObstacleByRef(dtObstacleRef(id))) return;
    if (dtStatusSucceed(m.cache->removeObstacle(dtObstacleRef(id))) && m.obstacles) --m.obstacles;
}

u32 NavMesh3D::obstacle_count() const { return impl_->obstacles; }

i32 NavMesh3D::add_agent(vec3 position, const NavAgentParams3D& params) {
    Impl& m = *impl_;
    if (!m.crowd) return -1;
    const dtCrowdAgentParams cp = m.crowd_params(params);
    const int idx = m.crowd->addAgent(Impl::fp(position), &cp);
    if (idx < 0) return -1;
    Impl::Agent& a = m.agents[usize(idx)];
    a = Impl::Agent{};
    a.live = true;
    a.params = params;
    return idx;
}

void NavMesh3D::remove_agent(i32 agent) {
    Impl& m = *impl_;
    if (!m.valid_agent(agent)) return;
    m.crowd->removeAgent(agent);
    m.agents[usize(agent)] = Impl::Agent{};
}

void NavMesh3D::set_agent_params(i32 agent, const NavAgentParams3D& params) {
    Impl& m = *impl_;
    if (!m.valid_agent(agent)) return;
    m.agents[usize(agent)].params = params;
    const dtCrowdAgentParams cp = m.crowd_params(params);
    m.crowd->updateAgentParameters(agent, &cp);
}

bool NavMesh3D::set_agent_target(i32 agent, vec3 target) {
    Impl& m = *impl_;
    if (!m.valid_agent(agent)) return false;
    Impl::Agent& a = m.agents[usize(agent)];
    a.has_target = true;
    a.arrived = false;
    a.target = target;
    vec3 p;
    const dtPolyRef ref = m.nearest(target, m.agent_extents(agent), &p);
    if (!ref) {
        a.failed = true;
        m.crowd->resetMoveTarget(agent);
        return false;
    }
    a.failed = false;
    return m.crowd->requestMoveTarget(agent, ref, Impl::fp(p));
}

void NavMesh3D::stop_agent(i32 agent) {
    Impl& m = *impl_;
    if (!m.valid_agent(agent)) return;
    Impl::Agent& a = m.agents[usize(agent)];
    a.has_target = false;
    a.arrived = false;
    a.failed = false;
    m.crowd->resetMoveTarget(agent);
}

void NavMesh3D::move_agent(i32 agent, vec3 position) {
    Impl& m = *impl_;
    if (!m.valid_agent(agent)) return;
    dtCrowdAgent* ag = m.crowd->getEditableAgent(agent);
    const vec3 cur = Impl::v3(ag->npos);
    const vec3 d = position - cur;
    if (d.x * d.x + d.z * d.z < 1e-8f && std::fabs(d.y) < 1e-4f) return;
    if (ag->state == DT_CROWDAGENT_STATE_WALKING && d.x * d.x + d.z * d.z < 1.0f) {
        // A small move (a controller slid the agent): slide the corridor along.
        ag->corridor.movePosition(Impl::fp(position), m.query, &m.filter);
        dtVcopy(ag->npos, ag->corridor.getPos());
        return;
    }
    vec3 p;
    const dtPolyRef ref = m.nearest(position, m.agent_extents(agent), &p);
    if (!ref) {
        ag->state = DT_CROWDAGENT_STATE_INVALID;
        dtVcopy(ag->npos, Impl::fp(position));
        return;
    }
    ag->corridor.reset(ref, Impl::fp(p));
    ag->boundary.reset();
    ag->partial = false;
    ag->topologyOptTime = 0.0f;
    ag->targetReplanTime = 0.0f;
    ag->nneis = 0;
    dtVset(ag->dvel, 0, 0, 0);
    dtVset(ag->nvel, 0, 0, 0);
    dtVset(ag->vel, 0, 0, 0);
    dtVcopy(ag->npos, Impl::fp(p));
    ag->state = DT_CROWDAGENT_STATE_WALKING;
    const Impl::Agent& a = m.agents[usize(agent)];
    if (a.has_target && !a.arrived) set_agent_target(agent, a.target);
}

bool NavMesh3D::agent_status(i32 agent, NavAgentStatus3D& out) const {
    const Impl& m = *impl_;
    if (!m.valid_agent(agent)) return false;
    const dtCrowdAgent* ag = m.crowd->getAgent(agent);
    const Impl::Agent& a = m.agents[usize(agent)];
    out = NavAgentStatus3D{};
    out.position = Impl::v3(ag->npos);
    out.velocity = Impl::v3(ag->vel);
    out.target = a.has_target ? a.target : out.position;
    out.next_corner = ag->ncorners > 0 ? Impl::v3(ag->cornerVerts) : out.position;
    const vec3 d = out.target - out.position;
    out.distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    out.partial = a.has_target && ag->partial;
    if (ag->state == DT_CROWDAGENT_STATE_INVALID)
        out.state = NavAgentState3D::OffMesh;
    else if (!a.has_target)
        out.state = NavAgentState3D::Idle;
    else if (a.failed || ag->targetState == DT_CROWDAGENT_TARGET_FAILED || ag->partial)
        out.state = NavAgentState3D::NoPath;
    else if (a.arrived)
        out.state = NavAgentState3D::Arrived;
    else
        out.state = NavAgentState3D::Moving;
    return true;
}

void NavMesh3D::agent_corners(i32 agent, Array<vec3>& out) const {
    out.clear();
    const Impl& m = *impl_;
    if (!m.valid_agent(agent)) return;
    const dtCrowdAgent* ag = m.crowd->getAgent(agent);
    for (int i = 0; i < ag->ncorners; ++i) out.push_back(Impl::v3(&ag->cornerVerts[i * 3]));
}

u32 NavMesh3D::agent_count() const {
    u32 n = 0;
    for (const Impl::Agent& a : impl_->agents) n += a.live ? 1u : 0u;
    return n;
}

void NavMesh3D::update(f32 dt) {
    Impl& m = *impl_;
    if (!m.nav) return;
    bool up_to_date = false;
    for (int i = 0; i < 256 && !up_to_date; ++i) {
        if (dtStatusFailed(m.cache->update(dt, m.nav, &up_to_date))) break;
    }
    m.crowd->update(dt, nullptr);

    // Arrival: stop agents that reached their target (or the end of a partial path).
    for (usize i = 0; i < m.agents.size(); ++i) {
        Impl::Agent& a = m.agents[i];
        if (!a.live || !a.has_target || a.arrived) continue;
        const dtCrowdAgent* ag = m.crowd->getAgent(int(i));
        if (ag->state != DT_CROWDAGENT_STATE_WALKING || ag->targetState != DT_CROWDAGENT_TARGET_VALID) continue;
        const f32* goal = ag->partial ? ag->corridor.getTarget() : ag->targetPos;
        const f32 dx = goal[0] - ag->npos[0];
        const f32 dz = goal[2] - ag->npos[2];
        const f32 d2 = dx * dx + dz * dz;
        const f32 r = a.params.arrive_distance;
        // Within arrive_distance, or stalled inside the crowd's slow-down radius (other agents
        // standing at the goal keep it from getting closer).
        const f32 slow = a.params.radius * 2.0f;
        const f32 stall = 0.05f * a.params.max_speed;
        const bool stalled = d2 <= slow * slow && dtVlenSqr(ag->vel) <= stall * stall;
        if ((d2 <= r * r || stalled) && std::fabs(goal[1] - ag->npos[1]) < a.params.height) {
            if (ag->partial) {
                a.failed = true;
            } else {
                a.arrived = true;
            }
            m.crowd->resetMoveTarget(int(i));
        }
    }
}

u64 NavMesh3D::hash_state(u64 seed) const {
    const Impl& m = *impl_;
    Hasher64 h(seed);
    h.bytes(&m.obstacles, sizeof(m.obstacles));
    if (!m.crowd) return h.value();
    for (usize i = 0; i < m.agents.size(); ++i) {
        const Impl::Agent& a = m.agents[i];
        if (!a.live) continue;
        const dtCrowdAgent* ag = m.crowd->getAgent(int(i));
        const u32 idx = u32(i);
        h.bytes(&idx, sizeof(idx));
        h.bytes(ag->npos, sizeof(ag->npos));
        h.bytes(ag->vel, sizeof(ag->vel));
        const u8 flags[4] = {ag->state, ag->targetState, u8(a.arrived), u8(a.failed)};
        h.bytes(flags, sizeof(flags));
    }
    return h.value();
}

void NavMesh3D::debug_triangles(Array<vec3>& verts, Array<u8>& areas, f32 lift, Array<vec3>* links) const {
    verts.clear();
    areas.clear();
    if (links) links->clear();
    const dtNavMesh* nav = impl_->nav;
    if (!nav) return;
    for (int i = 0; i < nav->getMaxTiles(); ++i) {
        const dtMeshTile* tile = nav->getTile(i);
        if (!tile || !tile->header) continue;
        for (int ip = 0; ip < tile->header->polyCount; ++ip) {
            const dtPoly& p = tile->polys[ip];
            if (p.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) {
                if (links) {
                    links->push_back(Impl::v3(&tile->verts[p.verts[0] * 3]));
                    links->push_back(Impl::v3(&tile->verts[p.verts[1] * 3]));
                }
                continue;
            }
            const dtPolyDetail& pd = tile->detailMeshes[ip];
            for (int j = 0; j < pd.triCount; ++j) {
                const unsigned char* t = &tile->detailTris[(pd.triBase + u32(j)) * 4];
                for (int k = 0; k < 3; ++k) {
                    const f32* v = t[k] < p.vertCount ? &tile->verts[p.verts[t[k]] * 3]
                                                      : &tile->detailVerts[(pd.vertBase + t[k] - p.vertCount) * 3];
                    verts.push_back({v[0], v[1] + lift, v[2]});
                }
                areas.push_back(p.getArea());
            }
        }
    }
}

NavMesh3D::Stats NavMesh3D::stats() const {
    Stats s;
    const dtNavMesh* nav = impl_->nav;
    s.obstacles = impl_->obstacles;
    s.agents = agent_count();
    if (!nav) return s;
    for (int i = 0; i < nav->getMaxTiles(); ++i) {
        const dtMeshTile* tile = nav->getTile(i);
        if (!tile || !tile->header) continue;
        ++s.tiles;
        s.polys += u32(tile->header->polyCount - tile->header->offMeshConCount);
        s.links += u32(tile->header->offMeshConCount);
    }
    return s;
}

}  // namespace mce::physics
