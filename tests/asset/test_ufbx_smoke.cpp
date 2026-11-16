// ufbx pin smoke (M-IMPORT3D step 3a, ADR-0047): the pinned ufbx loads an ASCII FBX 7.4 cube written here, twice to the
// same bytes (raw and with the unit/axis conversion the step-3b importer will ask for), and treats hostile input as an
// error: every truncation, a few hundred seeded byte flips, and files that claim huge arrays under a memory limit. The
// FBX importer itself is step 3b; this file and fbx_importer.cpp are the only ones the "ufbx wall" lets name ufbx.

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ufbx.h>

namespace {

// An ASCII FBX 7.4 unit cube (+-1, 8 vertices, 6 quads). `unit_cm` is GlobalSettings' UnitScaleFactor (centimetres per
// file unit), `up_axis` its UpAxis (1 = Y, 2 = Z).
std::string cube_fbx(double unit_cm = 100.0, int up_axis = 1) {
    // Y up: Maya's and Blender's export (front +Z); Z up: 3ds Max's (front -Y). Both right-handed.
    const int front = up_axis == 1 ? 2 : 1;
    const int front_sign = up_axis == 1 ? 1 : -1;
    std::string s;
    s += "; FBX 7.4.0 project file\n";
    s += "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n\tCreator: \"mce test_ufbx_smoke\"\n}\n";
    s += "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n";
    s += "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\"," + std::to_string(up_axis) + "\n";
    s += "\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n";
    s += "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\"," + std::to_string(front) + "\n";
    s += "\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\"," + std::to_string(front_sign) + "\n";
    s += "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n";
    s += "\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n";
    s += "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\"," + std::to_string(unit_cm) + "\n";
    s += "\t}\n}\n";
    s += "Objects:  {\n";
    s += "\tGeometry: 1000, \"Geometry::Cube\", \"Mesh\" {\n";
    s += "\t\tVertices: *24 {\n\t\t\ta: -1,-1,-1,1,-1,-1,1,1,-1,-1,1,-1,-1,-1,1,1,-1,1,1,1,1,-1,1,1\n\t\t}\n";
    // A face's last index is stored as ~i (FBX convention).
    s += "\t\tPolygonVertexIndex: *24 {\n\t\t\ta: 0,3,2,-2,4,5,6,-8,0,1,5,-5,2,3,7,-7,1,2,6,-6,3,0,4,-8\n\t\t}\n";
    s += "\t\tGeometryVersion: 124\n";
    s += "\t}\n";
    s += "\tModel: 2000, \"Model::Cube\", \"Mesh\" {\n\t\tVersion: 232\n\t}\n";
    s += "}\n";
    s += "Connections:  {\n\tC: \"OO\",2000,0\n\tC: \"OO\",1000,2000\n}\n";
    return s;
}

// Counts what ufbx asks for; refuses nothing itself (ufbx's own memory_limit does the refusing).
struct CountingAllocator {
    size_t live = 0;
    size_t peak = 0;
    size_t largest = 0;
};

void* counting_alloc(void* user, size_t size) {
    auto* a = static_cast<CountingAllocator*>(user);
    a->live += size;
    if (a->live > a->peak) a->peak = a->live;
    if (size > a->largest) a->largest = size;
    return std::malloc(size == 0 ? 1 : size);
}

void* counting_realloc(void* user, void* old_ptr, size_t old_size, size_t new_size) {
    auto* a = static_cast<CountingAllocator*>(user);
    a->live = a->live - old_size + new_size;
    if (a->live > a->peak) a->peak = a->live;
    if (new_size > a->largest) a->largest = new_size;
    return std::realloc(old_ptr, new_size == 0 ? 1 : new_size);
}

void counting_free(void* user, void* ptr, size_t size) {
    auto* a = static_cast<CountingAllocator*>(user);
    a->live -= size;
    std::free(ptr);
}

constexpr size_t kMemoryLimit = size_t{64} << 20;  // what the step-3b importer will pass (a budget, not ufbx's default)

ufbx_load_opts limited_opts(CountingAllocator* counter = nullptr) {
    ufbx_load_opts opts = {};
    opts.file_format = UFBX_FILE_FORMAT_FBX;
    opts.temp_allocator.memory_limit = kMemoryLimit;
    opts.result_allocator.memory_limit = kMemoryLimit;
    if (counter) {
        for (ufbx_allocator_opts* ao : {&opts.temp_allocator, &opts.result_allocator}) {
            ao->allocator.alloc_fn = counting_alloc;
            ao->allocator.realloc_fn = counting_realloc;
            ao->allocator.free_fn = counting_free;
            ao->allocator.user = counter;
        }
    }
    return opts;
}

struct Loaded {
    ufbx_scene* scene = nullptr;
    ufbx_error error = {};
    Loaded() = default;
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;
    ~Loaded() { ufbx_free_scene(scene); }
};

void load(Loaded& out, const void* data, size_t size, const ufbx_load_opts& opts) {
    out.scene = ufbx_load_memory(data, size, &opts, &out.error);
}

std::vector<double> positions(const ufbx_scene* scene) {
    std::vector<double> out;
    for (size_t m = 0; m < scene->meshes.count; ++m) {
        const ufbx_mesh* mesh = scene->meshes.data[m];
        for (size_t i = 0; i < mesh->vertices.count; ++i) {
            const ufbx_vec3 v = mesh->vertices.data[i];
            out.push_back(static_cast<double>(v.x));
            out.push_back(static_cast<double>(v.y));
            out.push_back(static_cast<double>(v.z));
        }
    }
    return out;
}

// Loads a mutated copy; success or a reported error are both fine, a crash (or ASan report) is the failure.
size_t load_and_count_errors(const std::string& bytes) {
    Loaded l;
    const ufbx_load_opts opts = limited_opts();
    load(l, bytes.data(), bytes.size(), opts);
    if (!l.scene) {
        CHECK(l.error.type != UFBX_ERROR_NONE);
        return 1;
    }
    // A scene that loaded must be walkable: touch every mesh's arrays.
    for (size_t m = 0; m < l.scene->meshes.count; ++m) {
        const ufbx_mesh* mesh = l.scene->meshes.data[m];
        for (size_t i = 0; i < mesh->vertex_indices.count; ++i) CHECK(mesh->vertex_indices.data[i] < mesh->num_vertices);
    }
    return 0;
}

// Binary FBX 7.4 writer, just enough for a node tree with array properties (32-bit offsets below version 7500).
struct BinaryFbx {
    std::string bytes;

    BinaryFbx() {
        bytes.append("Kaydara FBX Binary  \0", 21);
        bytes.push_back('\x1a');
        bytes.push_back('\0');
        u32(7400);
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
    }
    void patch_u32(size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes[at + static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xffu);
    }
    // Opens a node with one property written by `props` (which returns the property count); returns the end-offset slot.
    template <class Props>
    size_t begin(const char* name, Props props) {
        const size_t node = bytes.size();
        u32(0);  // end offset, patched by end()
        u32(0);  // property count
        u32(0);  // property bytes
        bytes.push_back(static_cast<char>(std::strlen(name)));
        bytes.append(name);
        const size_t prop_start = bytes.size();
        const uint32_t count = props();
        patch_u32(node + 4, count);
        patch_u32(node + 8, static_cast<uint32_t>(bytes.size() - prop_start));
        return node;
    }
    void end(size_t node, bool has_children) {
        if (has_children) bytes.append(13, '\0');
        patch_u32(node, static_cast<uint32_t>(bytes.size()));
    }
    // An array property claiming `count` doubles stored (encoding 1 = zlib) in `payload`.
    void double_array_claim(uint32_t count, uint32_t encoding, const std::string& payload) {
        bytes.push_back('d');
        u32(count);
        u32(encoding);
        u32(static_cast<uint32_t>(payload.size()));
        bytes.append(payload);
    }
    void finish() { bytes.append(13, '\0'); }
};

std::string binary_huge_vertices(uint32_t encoding) {
    BinaryFbx f;
    const size_t objects = f.begin("Objects", [] { return 0u; });
    const size_t geom = f.begin("Geometry", [&f] {
        f.bytes.push_back('L');
        for (int i = 0; i < 8; ++i) f.bytes.push_back(i == 0 ? '\x01' : '\0');
        return 1u;
    });
    // 0x20000000 doubles = 4 GiB claimed. Zlib payload: a valid header, then an empty final fixed block and an
    // Adler-32 of 1; the uncompressed claim is the bait. Encoding 0 claims the same with 16 bytes of data.
    const std::string zlib("\x78\x9c\x03\x00\x00\x00\x00\x01", 8);
    const std::string raw(16, '\0');
    const size_t verts = f.begin("Vertices", [&] {
        f.double_array_claim(0x20000000u, encoding, encoding == 1 ? zlib : raw);
        return 1u;
    });
    f.end(verts, false);
    f.end(geom, true);
    f.end(objects, true);
    f.finish();
    return f.bytes;
}

}  // namespace

TEST_CASE("ufbx: the pinned version is the one ADR-0047 names") {
    CHECK(UFBX_HEADER_VERSION == ufbx_pack_version(0, 23, 1));
    CHECK(ufbx_source_version == UFBX_HEADER_VERSION);
}

TEST_CASE("ufbx: an ASCII FBX 7.4 cube loads from memory") {
    const std::string fbx = cube_fbx();
    Loaded l;
    const ufbx_load_opts opts = limited_opts();
    load(l, fbx.data(), fbx.size(), opts);
    INFO(std::string(l.error.description.data, l.error.description.length));
    REQUIRE(l.scene != nullptr);
    CHECK(l.scene->metadata.ascii);
    CHECK(l.scene->metadata.version == 7400);
    REQUIRE(l.scene->meshes.count == 1);
    const ufbx_mesh* mesh = l.scene->meshes.data[0];
    CHECK(mesh->num_vertices == 8);
    CHECK(mesh->num_faces == 6);
    CHECK(mesh->num_indices == 24);
    CHECK(mesh->num_triangles == 12);
    CHECK(mesh->max_face_triangles == 2);
    REQUIRE(mesh->instances.count == 1);
    CHECK(std::string(mesh->instances.data[0]->name.data) == "Cube");
    for (size_t f = 0; f < mesh->num_faces; ++f) CHECK(mesh->faces.data[f].num_indices == 4);
    const std::vector<double> p = positions(l.scene);
    REQUIRE(p.size() == 24);
    for (double c : p) CHECK((c == -1.0 || c == 1.0));
}

TEST_CASE("ufbx: two loads give identical positions, raw and converted to +Y up metres") {
    // The step-3b settings: target axes and unit with the conversion applied to the geometry. A Z-up file in
    // centimetres exercises both the axis rotation and the 0.01 scale.
    for (int converted = 0; converted < 2; ++converted) {
        CAPTURE(converted);
        const std::string fbx = cube_fbx(1.0, 2);
        ufbx_load_opts opts = limited_opts();
        if (converted) {
            opts.target_axes = ufbx_axes_right_handed_y_up;
            opts.target_unit_meters = 1.0;
            opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
        }
        Loaded a;
        Loaded b;
        load(a, fbx.data(), fbx.size(), opts);
        load(b, fbx.data(), fbx.size(), opts);
        REQUIRE(a.scene != nullptr);
        REQUIRE(b.scene != nullptr);
        const std::vector<double> pa = positions(a.scene);
        const std::vector<double> pb = positions(b.scene);
        REQUIRE(pa.size() == 24);
        REQUIRE(pa.size() == pb.size());
        CHECK(std::memcmp(pa.data(), pb.data(), pa.size() * sizeof(double)) == 0);
        CHECK(a.scene->settings.axes.up == UFBX_COORDINATE_AXIS_POSITIVE_Z);  // the file's own axes, always
        CHECK(a.scene->settings.original_unit_meters == doctest::Approx(0.01));
        const ufbx_node* node = a.scene->meshes.data[0]->instances.data[0];
        const ufbx_matrix& g = node->geometry_to_world;
        const double det = g.m00 * (g.m11 * g.m22 - g.m12 * g.m21) - g.m01 * (g.m10 * g.m22 - g.m12 * g.m20) +
                           g.m02 * (g.m10 * g.m21 - g.m11 * g.m20);
        if (converted) {
            // MODIFY_GEOMETRY bakes the unit scale into the vertices; the Z-up -> Y-up turn stays a rotation in the
            // node's adjust transform (no mirror: a right-handed file into a right-handed target).
            for (double c : pa) CHECK(std::abs(std::abs(c) - 0.01) < 1e-12);
            CHECK(det == doctest::Approx(1.0));
            for (size_t i = 0; i < a.scene->meshes.data[0]->vertices.count; ++i) {
                const ufbx_vec3 w = ufbx_transform_position(&g, a.scene->meshes.data[0]->vertices.data[i]);
                const ufbx_vec3 r = a.scene->meshes.data[0]->vertices.data[i];
                // World +Y is the file's +Z.
                CHECK(std::abs(w.y - r.z) < 1e-12);
            }
        } else {
            for (double c : pa) CHECK((c == -1.0 || c == 1.0));
            CHECK(det == doctest::Approx(1.0));
        }
    }
}

TEST_CASE("ufbx: truncated and byte-flipped files return errors or walkable scenes, never crash") {
    const std::string fbx = cube_fbx();
    size_t errors = 0;
    // Every prefix (the cube is ~1.3 KB): cut lines, cut numbers, a missing closing brace.
    for (size_t n = 0; n < fbx.size(); ++n) errors += load_and_count_errors(fbx.substr(0, n));
    CHECK(errors > fbx.size() / 2);
    // 400 seeded mutations: 1 to 8 bytes replaced with arbitrary values (an LCG, so every host runs the same files).
    uint64_t state = 0x9e3779b97f4a7c15ull;
    auto next = [&state] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 33);
    };
    for (int round = 0; round < 400; ++round) {
        std::string m = fbx;
        const uint32_t flips = 1 + next() % 8;
        for (uint32_t i = 0; i < flips; ++i) m[next() % m.size()] = static_cast<char>(next() & 0xffu);
        load_and_count_errors(m);
    }
    // The binary huge-claim file below, each of its bytes flipped once (the binary parser's turn).
    const std::string bin = binary_huge_vertices(1);
    for (size_t i = 0; i < bin.size(); ++i) {
        std::string m = bin;
        m[i] = static_cast<char>(m[i] ^ 0x5a);
        load_and_count_errors(m);
    }
}

TEST_CASE("ufbx: a file claiming gigabytes errors (or ignores the claim) instead of allocating them") {
    struct Case {
        const char* name;
        std::string bytes;
        bool loads;  // ASCII array counts are only a hint: ufbx reads the values present and ignores the claim
    };
    std::string ascii_claim = cube_fbx();
    const std::string from = "Vertices: *24 {";
    ascii_claim.replace(ascii_claim.find(from), from.size(), "Vertices: *536870912 {");
    const Case cases[] = {
        {"binary, zlib array of 2^29 doubles", binary_huge_vertices(1), false},
        {"binary, raw array of 2^29 doubles", binary_huge_vertices(0), false},
        {"ascii, *536870912 header on 24 values", ascii_claim, true},
    };
    for (const Case& c : cases) {
        const std::string name = c.name;
        CAPTURE(name);
        CountingAllocator counter;
        {
            Loaded l;
            const ufbx_load_opts opts = limited_opts(&counter);
            load(l, c.bytes.data(), c.bytes.size(), opts);
            const std::string error(l.error.description.data, l.error.description.length);
            CAPTURE(error);
            if (c.loads) {
                REQUIRE(l.scene != nullptr);
                REQUIRE(l.scene->meshes.count == 1);
                CHECK(l.scene->meshes.data[0]->num_vertices == 8);
            } else {
                CHECK(l.scene == nullptr);
                CHECK(l.error.type == UFBX_ERROR_MEMORY_LIMIT);
            }
        }
        // Nothing near the claim was ever requested, and the live total stayed inside the two limits (temp + result).
        CHECK(counter.largest <= kMemoryLimit);
        CHECK(counter.peak <= 2 * kMemoryLimit);
        CHECK(counter.live == 0);
    }
}

TEST_CASE("ufbx: the memory limit itself is enforced") {
    // A limit below what even the cube needs: ufbx must report MEMORY_LIMIT, not allocate past it.
    const std::string fbx = cube_fbx();
    CountingAllocator counter;
    ufbx_load_opts opts = limited_opts(&counter);
    opts.temp_allocator.memory_limit = 4096;
    opts.result_allocator.memory_limit = 4096;
    Loaded l;
    load(l, fbx.data(), fbx.size(), opts);
    CHECK(l.scene == nullptr);
    CHECK(l.error.type == UFBX_ERROR_MEMORY_LIMIT);
    CHECK(counter.live == 0);
}
