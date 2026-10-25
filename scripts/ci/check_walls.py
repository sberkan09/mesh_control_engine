#!/usr/bin/env python3
"""Include/symbol grep walls (ARCHITECTURE §1.2). Exit code 1 on any violation.

Each wall: a regex that may only appear under the allowed path prefixes. A line containing
`mce-allow` is exempt (use sparingly, with a reason in the same comment).
"""

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCAN_DIRS = ["src", "samples", "games", "projects", "templates", "tests", "tools"]
EXTS = {".h", ".hpp", ".inl", ".c", ".cc", ".cpp", ".mm", ".swift", ".java"}

# (name, pattern, allowed path prefixes, scanned path prefixes)
WALLS = [
    ("GL wall", re.compile(r"\bgl[A-Z]\w*\s*\("), ["src/engine/rhi_gl/"], ["src/", "samples/", "games/"]),
    ("SDL wall", re.compile(r"\bSDL_Event\b"), ["src/engine/platform/"], ["src/", "samples/", "games/"]),
    # SDL headers: platform, plus the sanctioned native_handle() consumers (GL context, ImGui backend).
    (
        "SDL include wall",
        re.compile(r"[<\"]SDL3/"),
        ["src/engine/platform/", "src/engine/rhi_gl/", "src/engine/rhi_metal/", "src/engine/rhi_sdlgpu/", "src/editor/layer/"],
        ["src/", "samples/", "games/"],
    ),
    ("glad wall", re.compile(r"[<\"]glad/"), ["src/engine/rhi_gl/"], ["src/", "samples/", "games/", "tests/"]),
    # Metal (ADR-0028, docs/architecture/macos-metal.md §3): the mirror of the GL wall.
    (
        "Metal wall",
        re.compile(r"\bMTL[A-Z]\w*|[<\"]Metal/|[<\"]QuartzCore/"),
        ["src/engine/rhi_metal/"],
        ["src/", "samples/", "games/", "tests/"],
    ),
    # SDL GPU (ADR-0038): the API lives in the SDL3 GPU backend alone, the mirror of the GL wall.
    ("SDL GPU wall", re.compile(r"\bSDL_\w*GPU\w*"), ["src/engine/rhi_sdlgpu/"], ["src/", "samples/", "games/", "tests/"]),
    # WebGPU (ADR-0039): the API (emdawnwebgpu's webgpu.h) lives in the browser backend alone, and Tint
    # (SPIR-V -> WGSL) in its translator file alone.
    ("WebGPU wall", re.compile(r"\bWGPU[A-Z]\w*|\bwgpu[A-Z]\w*\s*\(|[<\"]webgpu/"), ["src/engine/rhi_webgpu/src/"],
     ["src/", "samples/", "games/", "tests/", "tools/"]),
    ("Tint wall", re.compile(r"[<\"]src/tint/|\btint::"), ["src/engine/rhi_webgpu/src/shader_wgsl.cpp"],
     ["src/", "samples/", "games/", "tests/", "tools/"]),
    # The GLSL -> MSL translator (glslang, SPIRV-Cross) is private to the Metal backend; glslang is
    # also the shader cook's validator, inside the cook library (ADR-0033), the SDL GPU backend's
    # GLSL -> SPIR-V translator, SPIRV-Cross also its SPIR-V -> MSL pass on Apple (ADR-0038), and the
    # first half of the WebGPU backend's GLSL -> WGSL translator (ADR-0039).
    (
        "shader-cross wall",
        re.compile(r"[<\"](glslang|SPIRV)/|[<\"]spirv_\w+\.hpp|\bspirv_cross::|\bglslang::"),
        ["src/engine/rhi_metal/", "src/engine/rhi_sdlgpu/src/shader_spirv.cpp", "src/engine/rhi_sdlgpu/src/shader_msl.cpp",
         "src/engine/rhi_webgpu/src/shader_wgsl.cpp", "src/engine/asset/cook/src/shader_importer.cpp"],
        ["src/", "samples/", "games/", "tests/", "tools/"],
    ),
    # astc-encoder (M-MOBILE step 8) is cook-time only: the texture importer writes ASTC blocks, runtimes upload them.
    ("astcenc wall", re.compile(r"[<\"]astcenc\.h|\bastcenc_\w+\("), ["src/engine/asset/cook/src/texture_importer.cpp"],
     ["src/", "samples/", "games/", "tests/", "tools/"]),
    # bc7enc_rdo (M-CONTENT 1b-ii) is cook-time only: texture_bc.cpp writes BC blocks, runtimes upload them. The test
    # reads them back through the same headers (bc7decomp for the reference decode).
    ("bc7enc wall", re.compile(r"[<\"](bc7enc|rgbcx|bc7decomp)\.h|\bbc7enc_\w+\(|\brgbcx::"),
     ["src/engine/asset/cook/src/texture_bc.cpp", "tests/asset/test_cook_bc.cpp"],
     ["src/", "samples/", "games/", "tests/", "tools/"]),
    # ufbx (M-IMPORT3D step 3, ADR-0047) is cook-time only: the FBX importer reads .fbx into the mesh back end, runtimes
    # load the cooked CHNK_MESH. ufbx_impl.c compiles it; the smoke test loads in-test FBX through it.
    ("ufbx wall", re.compile(r"[<\"]ufbx\.[ch]\b|\bufbx_\w+\s*\("),
     ["src/engine/asset/cook/src/fbx_importer.cpp", "src/engine/asset/cook/src/ufbx_impl.c",
      "tests/asset/test_ufbx_smoke.cpp"],
     ["src/", "samples/", "games/", "tests/", "tools/"]),
    # dr_libs (M-IMPORTMEDIA step 2) is cook-time only: audio_decode.cpp turns .mp3 and .flac into PCM for the audio
    # importer, runtimes load the cooked CHNK_SFX. drlibs_impl.c compiles the decoders.
    ("dr_libs wall", re.compile(r"[<\"]dr_(mp3|flac|wav)\.h|\bdr(mp3|flac)_\w+\s*\("),
     ["src/engine/asset/cook/src/audio_decode.cpp", "src/engine/asset/cook/src/drlibs_impl.c"],
     ["src/", "samples/", "games/", "tests/", "tools/"]),
    # tinyexr (M-VISUAL3 step 4) is cook-time only: the texture importer reads .exr into RGBA16F, runtimes upload it.
    ("tinyexr wall", re.compile(r"[<\"]tinyexr\.h|\b(Load|Parse|Free|Init|Is)EXR\w*\s*\("),
     ["src/engine/asset/cook/src/texture_importer.cpp", "src/engine/asset/cook/src/tinyexr_impl.cpp"],
     ["src/", "samples/", "games/", "tests/", "tools/"]),
    # The store wall (M-MONETIZE steps 3-4, M-MOBILE 11c): StoreKit, Play Billing, Play In-App Review,
    # Google Mobile Ads, UMP, ATT, Firebase (M-LIVEOPS step 1), GameKit and Play Games (step 2) and JNI only
    # behind mce/platform/store.h, ads.h, review.h, firebase.h and game_services.h.
    (
        "store wall",
        re.compile(r"[<\"](StoreKit|jni)[/.]|^\s*import\s+StoreKit|\bJNIEnv\b|\bSK(Payment|Product|Store)\w*|com\.android\.billingclient|com\.google\.android\.play\.core\.review|\bAppStore\.requestReview\b|com\.google\.android\.(gms\.ads|ump)|^\s*import\s+(GoogleMobileAds|UserMessagingPlatform|AppTrackingTransparency|Firebase\w*|GameKit)\b|com\.google\.firebase|com\.google\.android\.gms\.games|\bGKLocalPlayer\b"),
        ["src/engine/platform/"],
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    # Steamworks (ADR-0042): the flat API is opened at run time by mce_platform's steam*.cpp only; the
    # test stand-ins for steam_api live under tests/platform/.
    (
        "Steam wall",
        re.compile(r"\bSteamAPI_\w+|[<\"]steam/|\bISteam[A-Z]\w*|\bSteamworks::"),
        ["src/engine/platform/src/steam", "tests/platform/"],
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    ("ImGui wall", re.compile(r"\bImGui(::|zmo)|[<\"]imgui"), ["src/editor/layer/"], ["src/", "samples/", "games/"]),
    ("JPH wall", re.compile(r"\bJPH::|[<\"]Jolt/"), ["src/engine/physics3d/"], ["src/", "samples/", "games/", "tests/"]),
    # Recast & Detour (ADR-0040): navigation, private to mce_physics3d's sources.
    (
        "Recast wall",
        re.compile(r"[<\"](Recast|Detour)\w*\.h|\b(rc|dt)[A-Z]\w*\s*[(<*&]|\bdt(Nav|Crowd|Tile|Poly|Query)\w*"),
        ["src/engine/physics3d/src/"],
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    # pl_mpeg (ADR-0043): the video decoder, private to mce_render's two video sources; everything
    # else goes through render::VideoDecoder.
    (
        "pl_mpeg wall",
        re.compile(r"[<\"]pl_mpeg\.h|\bplm_[a-z_]+\s*\(|\bplm_\w+_t\b"),
        ["src/engine/render/src/video.cpp", "src/engine/render/src/plmpeg_impl.c"],
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    ("Box2D wall", re.compile(r"\bb2[A-Z]\w*|[<\"]box2d/"), ["tests/physics2d/oracle/"], ["src/", "samples/", "games/", "tests/", "tools/"]),
    ("DirectXMath wall", re.compile(r"[<\"]DirectX\w*\.h|\bDirectX::"), ["tests/core/test_math_oracle"], ["src/", "samples/", "games/", "tests/", "tools/"]),
    # Cook-time-only third-party code (FR-AST-010): stb lives in the cook library, never in a
    # runtime target; runtime code never includes the cook library.
    ("stb wall", re.compile(r"\bstb(i|tt|rp)_\w*\s*\(|[<\"]stb_\w+\.h"), ["src/engine/asset/cook/"], ["src/", "samples/", "games/", "tests/", "tools/"]),
    ("meshopt wall", re.compile(r"\bmeshopt_\w*\s*\(|[<\"]meshoptimizer\.h"), ["src/engine/asset/cook/", "tests/asset/test_cook_gltf_ext.cpp", "tests/fuzz/test_mesh_import.cpp"], ["src/", "samples/", "games/", "tests/", "tools/"]),
    # Ogg Vorbis (ADR-0044): stb_vorbis decodes, private to mce_asset's VorbisReader (cook and runtime
    # alike); libvorbis encodes, private to the cook's vorbis_encode.cpp. Everything else goes through
    # asset::VorbisReader / cook::encode_vorbis.
    (
        "stb_vorbis wall",
        re.compile(r"\bstb_vorbis_\w*\s*\(|[<\"]stb_vorbis\.c"),
        ["src/engine/asset/src/vorbis.cpp", "src/engine/asset/src/stb_vorbis_impl.c"],
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    (
        "libvorbis wall",
        re.compile(r"[<\"](vorbis|ogg)/|\b(vorbis|ogg)_[a-z_]+\s*\(|\bvorbis_(info|dsp_state|block|comment)\b"),
        ["src/engine/asset/cook/src/vorbis_encode.cpp"],
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    ("cook wall", re.compile(r"[<\"]mce/cook/"), ["src/engine/asset/cook/", "src/editor/", "tools/cook/"], ["src/", "samples/", "games/"]),
    (
        "engine container ban",
        re.compile(r"std::(vector|unordered_map|unordered_set|string\b|list\b|map\b)|#include\s*<(iostream|sstream|fstream)>"),
        [],
        ["src/engine/"],
    ),
    # ENet (ADR-0035): the optional networking module's transport, private to mce_net's sources.
    (
        "ENet wall",
        re.compile(r"[<\"]enet/|\benet_[a-z_]+\s*\(|\bENet[A-Z]\w*"),
        ["src/engine/net/src/"],
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    # Sockets (M-NET2 step 6): the WebSocket transport's TCP layer is private to mce_net's sources,
    # like ENet. Everything else reaches the network through net::Transport.
    (
        "socket wall",
        re.compile(r"[<\"](sys/socket\.h|netinet/|arpa/inet\.h|netdb\.h|winsock2?\.h|ws2tcpip\.h|emscripten/websocket\.h)"),
        ["src/engine/net/src/", "tests/fuzz/test_websocket.cpp"],  # the fuzz test is a raw TCP peer
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    # WebRTC (M-NET3 step 4): libdatachannel's C API is private to rtc_native.cpp (the browser's
    # RTCPeerConnection to rtc_web.cpp). Everything else reaches a direct link through net::RtcHost.
    (
        "WebRTC wall",
        re.compile(r"[<\"](rtc|juice)/|\brtc(Create|Set|Delete|Send|Add|Init|Cleanup|Preload|Get|Is)[A-Z]?\w*\s*\("),
        ["src/engine/net/src/rtc_native.cpp"],
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    # Luau (ADR-0041, M-SCRIPT): the VM headers stay inside mce_script's sources, the compiler's
    # (luacode.h, Luau/*.h) inside mce_script_compiler's.
    (
        "Luau wall",
        re.compile(r"#\s*include\s*[<\"](lua|lualib|luaconf)\.h[>\"]"),
        ["src/engine/script/src/"],
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    (
        "Luau compiler wall",
        re.compile(r"#\s*include\s*[<\"](luacode\.h|Luau/)"),
        ["src/engine/script/compiler/"],
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    # Bytecode trust (M-SCRIPT2 step 0, SBA-198): Luau's loader trusts its input, so only the game's own cooked pack may be
    # mounted with allow_code (the player's pack, the editor project's cook, the headless run's pack), and raw bytecode
    # reaches a ScriptHost only through the script module (scene scripts load it from those mounts, compile() makes it
    # from source). A mod, a base pack, fetched config or any user file is data-only; to add a mount site, name it here.
    (
        "bytecode trust wall (allow_code)",
        re.compile(r"\ballow_code\s*=\s*true\b"),
        ["src/player/src/player.cpp", "src/editor/core/src/project.cpp", "src/editor/mcp/src/headless_run.cpp", "tests/"],
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    (
        "bytecode trust wall (add_bytecode)",
        re.compile(r"\badd_bytecode\s*\("),
        ["src/engine/script/", "tests/"],
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    # Editor MCP server (ADR-0027, N7): stdio is its only transport, no socket API in its code.
    (
        "MCP socket wall",
        re.compile(r"\b(socket|bind|listen|accept|connect|getaddrinfo)\s*\(|[<\"](sys/socket\.h|netinet/|arpa/inet\.h|winsock2?\.h|ws2tcpip\.h)"),
        [],
        ["src/editor/mcp/"],
    ),
]

# Path-pattern walls: (name, pattern, regex over the repo-relative path of files that may NOT match).
PATH_WALLS = [
    # Gameplay never includes the runtime UI (ui.md §4.4, FR-UI-030): UI intents reach gameplay as
    # events through the runtime state, and HUD values are read by binding, not pushed by game code.
    (
        "UI gameplay wall",
        re.compile(r"[<\"]mce/ui/"),
        re.compile(r"^(samples/[^/]+/game/|(projects/[^/]+/|templates/[^/]+/)?games/[^/]+/module/)"),
    ),
]

# CMake link walls: (name, pattern, allowed path prefixes). Scans every CMakeLists.txt under
# SCAN_DIRS, comment lines excluded, so a third-party target can only be linked where its wall says.
LINK_WALLS = [
    # Jolt (ADR-0003): linked by mce_physics3d alone, privately.
    ("Jolt link wall", re.compile(r"(?<![\w/.-])Jolt(?![\w/.-])"), ["src/engine/physics3d/"]),
    # The MCP server (and its JSON-RPC layer) is editor tooling: never linked by a runtime target.
    ("MCP link wall", re.compile(r"\bmce_mcp_core\b"), ["src/editor/mcp/", "tests/mcp/"]),
    # The cook library carries the JSON codec (FR-AST-010: the runtime never parses JSON).
    ("cook link wall", re.compile(r"\bmce_asset_cook\b"), ["src/engine/asset/", "src/editor/", "tools/", "tests/"]),
    # Networking is opt-in (ADR-0035): only games, samples and tests link mce_net; no engine
    # library, host, tool or the editor does, so the engine and every other game stay network-free.
    ("net opt-in wall", re.compile(r"\bmce_net\b"), ["src/engine/net/", "samples/", "games/", "projects/", "templates/", "tests/"]),
    ("Recast link wall", re.compile(r"\bmce_recast\b"), ["src/engine/physics3d/"]),
    ("ENet link wall", re.compile(r"\bmce_enet\b"), ["src/engine/net/"]),
    ("libdatachannel link wall", re.compile(r"\b(datachannel-static|juice-static|usrsctp)\b"), ["src/engine/net/"]),
    ("bc7enc link wall", re.compile(r"\bmce_bc7enc\b"), ["src/engine/asset/cook/", "tests/asset/"]),
    ("ufbx link wall", re.compile(r"\bmce_ufbx\b"), ["src/engine/asset/cook/", "tests/asset/"]),
    ("dr_libs link wall", re.compile(r"\bmce_drlibs\b"), ["src/engine/asset/cook/"]),
    ("libvorbis link wall", re.compile(r"\bmce_vorbisenc\b"), ["src/engine/asset/cook/"]),
    # Luau scripting (ADR-0041, M-SCRIPT) is opt-in: no engine library links mce_script (it is not
    # in MCE_ENGINE_LIBS); the stock hosts (player, editor, mce_mcp), its sample and tests do. The
    # compiler links into the cook, the editor, tools and tests, never the player (it loads the
    # bytecode mce_cook writes; tests/script's player_no_luau_compiler checks the binary too).
    # Luau's targets are linked by mce_script and mce_script_compiler alone.
    ("script opt-in wall", re.compile(r"\bmce_script\b"),
     ["src/engine/script/", "src/player/", "src/editor/", "samples/script_headless/", "tests/script/", "tests/fuzz/"]),
    ("script compiler wall", re.compile(r"\bmce_script_compiler\b"),
     ["src/engine/script/", "src/engine/asset/cook/", "src/editor/", "tools/", "samples/script_headless/", "tests/script/", "tests/fuzz/"]),
    ("Luau link wall", re.compile(r"\bLuau\.\w+"), ["src/engine/script/"]),
    # The visual-novel kit is opt-in the same way (M-NOVEL): games, samples and tests link it (its
    # dialogue box, mce_kit_novel_ui, a game's host). The authoring tools (editor, mce_mcp, mce_scene_diff, mce_scene_lint, mce_scene_stats, mce_cook)
    # link its components so a novel scene opens and cooks anywhere (M-DATA-REACH 8); the player and
    # the engine libraries never do.
    ("novel kit opt-in wall", re.compile(r"\bmce_kit_novel(_ui)?\b"),
     ["src/engine/kit_novel/", "src/editor/core/", "src/editor/mcp/", "src/editor/diff/", "src/editor/lint/", "src/editor/stats/", "tools/cook/", "samples/", "games/", "projects/", "templates/", "tests/"]),
]

# The umbrella's library list (root CMakeLists.txt): the opt-in modules (mce_net, ADR-0035; mce_kit_novel)
# must never join it.
ENGINE_LIBS_RE = re.compile(r"set\(MCE_ENGINE_LIBS\b(.*?)\)", re.S)


def umbrella_violations() -> list:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = ENGINE_LIBS_RE.search(text)
    out = []
    for lib in ("mce_net", "mce_kit_novel", "mce_kit_novel_ui", "mce_script"):
        if m and re.search(r"\b%s\b" % lib, m.group(1)):
            out.append(f"CMakeLists.txt: opt-in wall: {lib} is listed in MCE_ENGINE_LIBS")
    return out


# M-FUZZ 4: a parser of untrusted bytes without a fuzz target is a hole. Every ChunkType must be named in
# tests/fuzz/test_chunk_readers.cpp (its reader walks mutated payloads), and every importer must have a source
# the importers fuzz test reads (a committed sample, project, template or game file with one of its extensions)
# or a corpus of its own under tests/fuzz/. The allow-lists carry the reason; a stale entry is a violation too.
FUZZ_CHUNK_EXEMPT = {
    "Atlas": "reserved type: no reader, no importer writes it",
    "AudioPacket": "reserved type: no reader, no importer writes it",
}
FUZZ_IMPORTER_EXEMPT: dict = {}
FUZZ_SOURCE_DIRS = ["samples", "projects", "templates", "games"]


def fuzz_coverage_violations() -> list:
    out = []
    header = (ROOT / "src/engine/asset/include/mce/asset/container.h").read_text(encoding="utf-8")
    m = re.search(r"enum class ChunkType\s*:\s*\w+\s*\{(.*?)\};", header, re.S)
    names = []
    if m:
        for line in m.group(1).splitlines():
            mm = re.match(r"\s*([A-Z]\w*)\s*(=\s*\d+)?\s*,", line)
            if mm:
                names.append(mm.group(1))
    readers = (ROOT / "tests/fuzz/test_chunk_readers.cpp").read_text(encoding="utf-8")
    for n in names:
        covered = re.search(r"ChunkType::%s\b" % n, readers) is not None
        if n in FUZZ_CHUNK_EXEMPT:
            if covered:
                out.append(f"tests/fuzz: fuzz wall: ChunkType::{n} is covered; drop it from FUZZ_CHUNK_EXEMPT in check_walls.py")
        elif not covered:
            out.append(f"tests/fuzz/test_chunk_readers.cpp: fuzz wall: ChunkType::{n} has no reader in the fuzz test "
                       "(add a case to read_chunk and has_reader, or an entry with a reason to FUZZ_CHUNK_EXEMPT)")
    for n in FUZZ_CHUNK_EXEMPT:
        if n not in names:
            out.append(f"scripts/ci/check_walls.py: fuzz wall: FUZZ_CHUNK_EXEMPT names {n}, which is not a ChunkType")

    have = set()
    for d in FUZZ_SOURCE_DIRS:
        base = ROOT / d
        if base.exists():
            have |= {f.suffix for f in base.rglob("*") if f.is_file()}
    fuzz_text = "".join(p.read_text(encoding="utf-8", errors="replace") for p in (ROOT / "tests/fuzz").glob("*.cpp"))
    cook = ROOT / "src/engine/asset/cook/src"
    files = sorted(p.name for p in cook.glob("*_importer.cpp"))
    for name in files:
        exts = set(re.findall(r'"(\.[a-z0-9]+)"', (cook / name).read_text(encoding="utf-8", errors="replace")))
        covered = any(e in have or f'"{e}"' in fuzz_text for e in exts)
        if name in FUZZ_IMPORTER_EXEMPT:
            if covered:
                out.append(f"scripts/ci/check_walls.py: fuzz wall: {name} is covered; drop it from FUZZ_IMPORTER_EXEMPT")
        elif not covered:
            out.append(f"src/engine/asset/cook/src/{name}: fuzz wall: no source of its extensions "
                       f"({', '.join(sorted(exts)) or 'none found'}) in {', '.join(FUZZ_SOURCE_DIRS)} and no corpus in "
                       "tests/fuzz (add one, or an entry with a reason to FUZZ_IMPORTER_EXEMPT)")
    for name in FUZZ_IMPORTER_EXEMPT:
        if name not in files:
            out.append(f"scripts/ci/check_walls.py: fuzz wall: FUZZ_IMPORTER_EXEMPT names {name}, which does not exist")
    return out


def link_violations() -> list:
    out = []
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.exists():
            continue
        for path in base.rglob("CMakeLists.txt"):
            rel = path.relative_to(ROOT).as_posix()
            for name, pattern, allowed in LINK_WALLS:
                if any(rel.startswith(a) for a in allowed):
                    continue
                for no, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                    code = line.split("#", 1)[0]
                    if pattern.search(code):
                        out.append(f"{rel}:{no}: {name}: {line.strip()}")
    return out


def main() -> int:
    violations = link_violations() + umbrella_violations() + fuzz_coverage_violations()
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.suffix not in EXTS or not path.is_file():
                continue
            rel = path.relative_to(ROOT).as_posix()
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            for name, pattern, forbidden_in in PATH_WALLS:
                if not forbidden_in.match(rel):
                    continue
                for no, line in enumerate(lines, 1):
                    if pattern.search(line):
                        violations.append(f"{rel}:{no}: {name}: {line.strip()}")
            for name, pattern, allowed, scanned in WALLS:
                if not any(rel.startswith(s) for s in scanned):
                    continue
                if any(rel.startswith(a) for a in allowed):
                    continue
                for no, line in enumerate(lines, 1):
                    if "mce-allow" in line:
                        continue
                    if pattern.search(line):
                        violations.append(f"{rel}:{no}: {name}: {line.strip()}")
    # M3: a changed header that reaches many translation units is a warning, never a failure.
    subprocess.run([sys.executable, str(ROOT / "scripts/ci/include_fanout.py")], check=False)
    if violations:
        print("Wall violations:")
        for v in violations:
            print("  " + v)
        return 1
    print("All walls hold.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
