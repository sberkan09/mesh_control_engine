#include "mce/mcp/server.h"

#include "mce/asset/xxhash.h"
#include "mce/cook/fs.h"
#include "mce/editor/play_session.h"
#include "tools.h"

namespace mce::mcp {

namespace {

constexpr usize kMaxMessageBytes = 8u << 20;

// Protocol versions this server speaks; an unknown request version gets the newest one back
// (the client then decides whether it can continue).
constexpr const char* kSupportedVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

constexpr const char* kInstructions =
    "Mesh Control Engine editor. One project per process. Scenes are .mcscene documents of entities "
    "(id, name, reflected components). Discover component types with refl_list_components / "
    "refl_describe_component; read with scene_info, scene_list_entities, scene_get_entity, asset_list; edit "
    "with entity_* and component_* (values use the .mcscene spelling: vectors as arrays, assets as "
    "\"guid:<uuid>\", entity references as \"$<entity id>\", enums by name). Edits stay in memory until "
    "scene_save. Verify with cook_run, cook_errors and run_headless (deterministic: same scene + ticks = same "
    "state_hash).";

bool valid_id(cook::JsonValue id) { return id.is_string() || (id.is_number() && id.is_integer()); }

}  // namespace

u64 file_content_hash(StringView path) {
    cook::String text;
    if (!cook::fs::read_text(path, text)) return 0;
    return asset::xxh64(text.view()) | 1;  // never 0: 0 means "unreadable"
}

Server::Server(const ServerOptions& opt) : opt_(opt) {}
Server::~Server() = default;

Server::ModuleGuard::~ModuleGuard() {
    if (!module.loaded()) return;
    editor::set_play_game_install(nullptr);
    unload_game_module(module);
}

Result<void> Server::load_module(StringView path) {
    if (module_.module.loaded()) return Error(Errc::AlreadyExists, "mcp.load_module", module_.module.source_path.view());
    MCE_TRY_ASSIGN(module_.module, open_game_module(path));
    editor::set_play_game_install(module_.module.info->install);
    return ok();
}

void Server::set_sink(SendFn fn, void* user) {
    send_ = fn;
    send_user_ = user;
}

Result<void> Server::open_project(StringView root, StringView scene) {
    MCE_TRY(session_.open_project(root));
    scene_file_ = SceneFile{};
    if (!scene.empty()) {
        cook::String err;
        if (!open_scene_file(*this, scene, err)) return Error(Errc::NotFound, "mcp.open_project", err.view());
    }
    return ok();
}

void Server::send(StringView message) {
    if (send_) send_(message, send_user_);
}

void Server::notify(StringView method, StringView params) {
    cook::String msg;
    JsonWriter w(msg);
    w.begin_object().key("jsonrpc").str("2.0").key("method").str(method).key("params").raw(params).end_object();
    send(msg.view());
}

void Server::respond(cook::JsonValue id, StringView result) {
    cook::String msg;
    JsonWriter w(msg);
    w.begin_object().key("jsonrpc").str("2.0").key("id").copy(id).key("result").raw(result).end_object();
    send(msg.view());
}

void Server::respond_error(cook::JsonValue id, i32 code, StringView message) {
    cook::String msg;
    JsonWriter w(msg);
    w.begin_object().key("jsonrpc").str("2.0").key("id");
    if (valid_id(id)) w.copy(id);
    else w.null();
    w.key("error").begin_object().key("code").num_i(code).key("message").str(message).end_object();
    w.end_object();
    send(msg.view());
}

void Server::handle(StringView line) {
    if (line.size() > kMaxMessageBytes) return respond_error({}, kParseError, "message too large");
    cook::JsonDoc doc;
    cook::JsonError jerr;
    if (!doc.parse(line, jerr)) {
        cook::String msg;
        cook::appendf(msg, "parse error at %u:%u: %s", jerr.line, jerr.col, jerr.message.c_str());
        return respond_error({}, kParseError, msg.view());
    }
    const cook::JsonValue root = doc.root();
    if (root.is_array()) return respond_error({}, kInvalidRequest, "batches are not supported");
    if (!root.is_object()) return respond_error({}, kInvalidRequest, "a message must be a JSON object");
    const cook::JsonValue id = root.get("id");
    const cook::JsonValue version = root.get("jsonrpc");
    if (!version.is_string() || version.as_string() != "2.0")
        return respond_error(id, kInvalidRequest, "\"jsonrpc\" must be \"2.0\"");
    const cook::JsonValue method = root.get("method");
    if (!method.valid()) {
        if (root.get("result").valid() || root.get("error").valid()) return;  // a response to us: none expected
        return respond_error(id, kInvalidRequest, "missing \"method\"");
    }
    if (!method.is_string()) return respond_error(id, kInvalidRequest, "\"method\" must be a string");
    if (id.valid() && !valid_id(id)) return respond_error({}, kInvalidRequest, "\"id\" must be a string or an integer");
    const cook::JsonValue params = root.get("params");

    if (!id.valid()) {  // notification: never answered
        if (method.as_string() == "notifications/initialized" && initialize_seen_) initialized_ = true;
        return;  // notifications/cancelled: calls run to completion before the next message is read
    }
    if (params.valid() && !params.is_object()) return respond_error(id, kInvalidParams, "\"params\" must be an object");
    request(id, method.as_string(), params);
}

void Server::request(cook::JsonValue id, StringView method, cook::JsonValue params) {
    cook::String result;
    JsonWriter w(result);
    if (method == "initialize") {
        const cook::JsonValue want = params.valid() ? params.get("protocolVersion") : cook::JsonValue();
        StringView version = kProtocolVersion;
        for (const char* v : kSupportedVersions)
            if (want.is_string() && want.as_string() == v) version = v;
        initialize_seen_ = true;
        w.begin_object();
        w.key("protocolVersion").str(version);
        w.key("capabilities").begin_object();
        w.key("tools").begin_object().key("listChanged").boolean(false).end_object();
        w.key("resources").begin_object().key("subscribe").boolean(false).key("listChanged").boolean(false).end_object();
        w.end_object();
        w.key("serverInfo").begin_object().key("name").str(kServerName).key("version").str(kServerVersion).end_object();
        w.key("instructions").str(kInstructions);
        w.end_object();
        return respond(id, result.view());
    }
    if (method == "ping") return respond(id, "{}");
    if (!initialize_seen_) return respond_error(id, kInvalidRequest, "not initialized: send \"initialize\" first");

    if (method == "tools/list") {
        w.begin_object().key("tools").begin_array();
        for (const ToolDef& t : tool_table()) {
            w.begin_object();
            w.key("name").str(t.name);
            w.key("description").str(t.description);
            w.key("inputSchema").raw(t.input_schema);
            w.end_object();
        }
        w.end_array().end_object();
        return respond(id, result.view());
    }
    if (method == "tools/call") return call_tool(id, params);
    if (method == "resources/list") {
        w.begin_object().key("resources").begin_array();
        list_resources(w);
        w.end_array().end_object();
        return respond(id, result.view());
    }
    if (method == "resources/templates/list") return respond(id, "{\"resourceTemplates\":[]}");
    if (method == "resources/read") {
        const cook::JsonValue uri = params.valid() ? params.get("uri") : cook::JsonValue();
        if (!uri.is_string()) return respond_error(id, kInvalidParams, "\"uri\" must be a string");
        if (!read_resource(*this, uri.as_string(), w)) {
            cook::String msg("resource not found: ");
            msg += uri.as_string();
            return respond_error(id, kResourceNotFound, msg.view());
        }
        return respond(id, result.view());
    }
    cook::String msg("method not found: ");
    msg += method;
    respond_error(id, kMethodNotFound, msg.view());
}

void Server::call_tool(cook::JsonValue id, cook::JsonValue params) {
    const cook::JsonValue name = params.valid() ? params.get("name") : cook::JsonValue();
    if (!name.is_string()) return respond_error(id, kInvalidParams, "\"name\" must be a string");
    const ToolDef* tool = find_tool(name.as_string());
    if (!tool) {
        cook::String msg("unknown tool: ");
        msg += name.as_string();
        return respond_error(id, kInvalidParams, msg.view());
    }
    const cook::JsonValue args = params.get("arguments");
    if (args.valid() && !args.is_object()) return respond_error(id, kInvalidParams, "\"arguments\" must be an object");
    const cook::JsonValue meta = params.get("_meta");
    const cook::JsonValue token = meta.is_object() ? meta.get("progressToken") : cook::JsonValue();

    cook::String structured;
    JsonWriter out(structured);
    cook::String err;
    Array<ToolContent> extra;
    ToolCall call{*this, args, valid_id(token) ? token : cook::JsonValue(), out, err, &extra};
    // Each call that can edit is one undo entry named after the tool (undo/redo move the history).
    const StringView tool_name(tool->name);
    const bool group = tool_name != "scene_undo" && tool_name != "scene_redo";
    if (group) session_.scene().begin_edit(tool_name);
    const bool ok = tool->fn(call);
    if (group) session_.scene().end_edit();
    MCE_ASSERT_MSG(!ok || (out.depth() == 0 && !structured.empty()), "tool wrote an incomplete result");

    cook::String result;
    JsonWriter w(result);
    w.begin_object().key("content").begin_array().begin_object().key("type").str("text").key("text");
    w.str(ok ? structured.view() : err.view());
    w.end_object();
    if (ok)
        for (const ToolContent& item : extra)
            w.begin_object().key("type").str("image").key("data").str(item.base64.view()).key("mimeType").str(item.mime.view()).end_object();
    w.end_array();
    if (ok) w.key("structuredContent").raw(structured.view());
    w.key("isError").boolean(!ok);
    w.end_object();
    respond(id, result.view());
}

}  // namespace mce::mcp
