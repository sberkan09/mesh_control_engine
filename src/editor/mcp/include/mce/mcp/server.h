#pragma once
// mce_mcp_core — the editor's Model Context Protocol server (editor-mcp.md, FR-EDT-160, ADR-0027).
//
// JSON-RPC 2.0, one message per line (the MCP stdio transport). The transport itself lives in the
// mce_mcp executable; this class only turns request lines into response lines, so tests drive it
// in process and the transcript test drives the real binary over pipes. Headless and ImGui-free:
// it links mce_editor_core (SceneDocument + the EditorScene funnel, EditorSession, ConsoleModel)
// and the engine, never the editor layer, never a socket.
//
// Every scene write goes through the EditorScene command funnel, the same one the inspector and
// the gizmo use; nothing touches disk unless a tool says so (scene_save, cook_run, run_headless).

#include "mce/editor/editor_session.h"
#include "mce/mcp/json_writer.h"
#include "mce/module_host.h"

namespace mce::mcp {

inline constexpr const char* kProtocolVersion = "2025-06-18";  // pinned (editor-mcp.md §7)
inline constexpr const char* kServerName = "mce_mcp";
inline constexpr const char* kServerVersion = "0.1.0";

// JSON-RPC error codes.
inline constexpr i32 kParseError = -32700;
inline constexpr i32 kInvalidRequest = -32600;
inline constexpr i32 kMethodNotFound = -32601;
inline constexpr i32 kInvalidParams = -32602;
inline constexpr i32 kResourceNotFound = -32002;

struct ServerOptions {
    bool allow_cheats = false;  // console_exec may touch Cheat / RequiresRestart convars
};

// Receives one outgoing message (a response or a notification) without the newline.
using SendFn = void (*)(StringView message, void* user);

class Server {
public:
    explicit Server(const ServerOptions& opt = {});
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void set_sink(SendFn fn, void* user);

    // Loads the project's game module (lib<game>.so / .dll) for the server's lifetime: its reflected
    // components join refl_list_components, the cook and the inspector, and its install() runs in
    // every run_headless world (as the project host runs it). Call before open_project, so the
    // first cook already knows the game's components. One module per server.
    Result<void> load_module(StringView path);
    const LoadedModule& module() const { return module_.module; }

    // The project every tool works in (`root` holds assets/); cooks it when nothing is cooked yet.
    // `scene` (relative to assets/) is opened too when given.
    Result<void> open_project(StringView root, StringView scene = {});

    // Handles one incoming message. Requests get exactly one response; notifications none.
    void handle(StringView line);

    // Sends a notification ("notifications/progress", ...); `params` is a JSON object.
    void notify(StringView method, StringView params);

    editor::EditorSession& session() { return session_; }
    const ServerOptions& options() const { return opt_; }
    bool initialized() const { return initialized_; }

    // The open scene's file as the tools see it: path under assets/ and a hash of the bytes last
    // read or written, so scene_save can refuse to overwrite a file changed behind its back.
    struct SceneFile {
        cook::String rel;
        u64 disk_hash = 0;
    };
    SceneFile& scene_file() { return scene_file_; }

private:
    void send(StringView message);
    void respond(cook::JsonValue id, StringView result);
    void respond_error(cook::JsonValue id, i32 code, StringView message);
    void request(cook::JsonValue id, StringView method, cook::JsonValue params);
    void call_tool(cook::JsonValue id, cook::JsonValue params);

    ServerOptions opt_;
    // Before session_: destroyed after it, so no World still holds the module's code and no
    // document its component records when the library unmaps.
    struct ModuleGuard {
        LoadedModule module;
        ~ModuleGuard();
    } module_;
    editor::EditorSession session_;
    SceneFile scene_file_;
    SendFn send_ = nullptr;
    void* send_user_ = nullptr;
    bool initialize_seen_ = false;
    bool initialized_ = false;
};

// Hash of a file's bytes (0 when it cannot be read): scene_save's changed-on-disk check.
u64 file_content_hash(StringView path);

}  // namespace mce::mcp
