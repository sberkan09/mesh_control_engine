#!/usr/bin/env python3
"""Include/symbol grep walls (ARCHITECTURE §1.2). Exit code 1 on any violation.

Each wall: a regex that may only appear under the allowed path prefixes. A line containing
`mce-allow` is exempt (use sparingly, with a reason in the same comment).
"""

import pathlib
import re
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
    # The store wall (M-MONETIZE steps 3-4, M-MOBILE 11c): StoreKit, Play Billing, Play In-App Review,
    # Google Mobile Ads, UMP, ATT, Firebase (M-LIVEOPS step 1), GameKit and Play Games (step 2) and JNI only
    # behind mce/platform/store.h, ads.h, review.h, firebase.h and game_services.h.
    (
        "store wall",
        re.compile(r"[<\"](StoreKit|jni)[/.]|^\s*import\s+StoreKit|\bJNIEnv\b|\bSK(Payment|Product|Store)\w*|com\.android\.billingclient|com\.google\.android\.play\.core\.review|\bAppStore\.requestReview\b|com\.google\.android\.(gms\.ads|ump)|^\s*import\s+(GoogleMobileAds|UserMessagingPlatform|AppTrackingTransparency|Firebase\w*|GameKit)\b|com\.google\.firebase|com\.google\.android\.gms\.games|\bGKLocalPlayer\b"),
        ["src/engine/platform/"],
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
    # pl_mpeg (ADR-0042): the video decoder, private to mce_render's two video sources; everything
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
    ("meshopt wall", re.compile(r"\bmeshopt_\w*\s*\(|[<\"]meshoptimizer\.h"), ["src/engine/asset/cook/"], ["src/", "samples/", "games/", "tests/", "tools/"]),
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
        ["src/engine/net/src/"],
        ["src/", "samples/", "games/", "projects/", "templates/", "tests/", "tools/"],
    ),
    # Luau (ADR-0041 spike): the VM and compiler headers stay inside mce_script's sources.
    (
        "Luau wall",
        re.compile(r"#\s*include\s*[<\"](lua|lualib|luacode|luaconf)\.h[>\"]"),
        ["src/engine/script/src/"],
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
    # The Luau scripting spike (ADR-0041) is opt-in: its test and its headless sample link it, no
    # engine library, host or tool does; Luau's targets are linked by mce_script alone.
    ("script opt-in wall", re.compile(r"\bmce_script\b"), ["src/engine/script/", "samples/script_headless/", "tests/script/"]),
    ("Luau link wall", re.compile(r"\bLuau\.\w+"), ["src/engine/script/"]),
    # The visual-novel kit is opt-in the same way (M-NOVEL): games, samples and tests link it (its
    # dialogue box, mce_kit_novel_ui, a game's host). The authoring tools (editor, mce_mcp, mce_cook)
    # link its components so a novel scene opens and cooks anywhere (M-DATA-REACH 8); the player and
    # the engine libraries never do.
    ("novel kit opt-in wall", re.compile(r"\bmce_kit_novel(_ui)?\b"),
     ["src/engine/kit_novel/", "src/editor/core/", "src/editor/mcp/", "tools/cook/", "samples/", "games/", "projects/", "templates/", "tests/"]),
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
    violations = link_violations() + umbrella_violations()
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
    if violations:
        print("Wall violations:")
        for v in violations:
            print("  " + v)
        return 1
    print("All walls hold.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
