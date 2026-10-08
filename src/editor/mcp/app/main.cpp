// mce_mcp — the editor's MCP server over stdio (editor-mcp.md §2): the MCP host spawns it and
// speaks newline-delimited JSON-RPC on its stdin/stdout. No socket, no listener, no port (N7).
//
//   mce_mcp --project <dir> [--scene <path under assets/>] [--allow-cheats]
//
// stdout carries protocol messages only: at startup the real stdout is moved to a private
// descriptor and fd 1 becomes a copy of stderr, so the engine's log (and any stray print) lands on
// stderr where MCP hosts expect diagnostics. The process ends when stdin closes.

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "mce/core.h"
#include "mce/mcp/server.h"

namespace {

using namespace mce;

std::FILE* claim_stdout() {
    std::fflush(stdout);
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    const int proto = _dup(_fileno(stdout));
    if (proto < 0 || _dup2(_fileno(stderr), _fileno(stdout)) != 0) return nullptr;
    _setmode(proto, _O_BINARY);
    return _fdopen(proto, "wb");
#else
    const int proto = dup(STDOUT_FILENO);
    if (proto < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return nullptr;
    return fdopen(proto, "wb");
#endif
}

void send_line(StringView message, void* user) {
    auto* out = static_cast<std::FILE*>(user);
    std::fwrite(message.data(), 1, message.size(), out);
    std::fputc('\n', out);
    std::fflush(out);
}

int usage() {
    std::fprintf(stderr,
                 "usage: mce_mcp --project <dir> [--scene <path under assets/>] [--allow-cheats]\n"
                 "  Model Context Protocol server for the MCE editor (JSON-RPC 2.0 over stdio).\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    StringView project, scene;
    mcp::ServerOptions opt;
    for (int i = 1; i < argc; ++i) {
        const StringView a = argv[i];
        if (a == "--project" && i + 1 < argc) project = argv[++i];
        else if (a == "--scene" && i + 1 < argc) scene = argv[++i];
        else if (a == "--allow-cheats") opt.allow_cheats = true;
        else return usage();
    }

    std::FILE* out = claim_stdout();
    if (!out) {
        std::fprintf(stderr, "mce_mcp: cannot set up the stdio transport\n");
        return 1;
    }
    log::LogDesc log_desc;
    log_desc.app_name = "mce_mcp";
    log::init(log_desc);

    mcp::Server server(opt);
    server.set_sink(send_line, out);
    if (!project.empty()) {
        if (Result<void> r = server.open_project(project, scene); !r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            std::fprintf(stderr, "mce_mcp: cannot open project %.*s: %s\n", static_cast<int>(project.size()), project.data(),
                         msg.c_str());
            log::shutdown();
            return 1;
        }
    }

    // One message per line; a line may be long (a big component value), so it grows as needed.
    Array<char> line;
    int c = 0;
    while (c != EOF) {
        line.clear();
        while ((c = std::getc(stdin)) != EOF && c != '\n') line.push_back(static_cast<char>(c));
        usize n = line.size();
        if (n > 0 && line[n - 1] == '\r') --n;
        if (n > 0) server.handle(StringView(line.data(), n));
    }
    log::shutdown();
    std::fclose(out);
    return 0;
}
