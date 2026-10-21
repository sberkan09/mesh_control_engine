#pragma once
// An in-process MCP client over mce::mcp::Server for the M-AGENT tool tests: sends JSON-RPC lines,
// parses the answers, and keeps a tool result's image content (base64 PNG) decoded.

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/cook/image.h"
#include "mce/mcp/server.h"

namespace mcp_client {

using namespace mce;

inline StringView sv(const std::string& s) { return StringView(s.data(), s.size()); }

struct Parsed {
    cook::JsonDoc doc;
    cook::JsonValue root() const { return doc.root(); }
};

inline std::unique_ptr<Parsed> parse(const std::string& text) {
    auto p = std::make_unique<Parsed>();
    cook::JsonError err;
    REQUIRE_MESSAGE(p->doc.parse(sv(text), err), text);
    return p;
}

inline std::string str(cook::JsonValue v) { return v.is_string() ? std::string(v.as_string()) : std::string(); }

inline std::vector<u8> base64_decode(const std::string& s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::vector<u8> out;
    u32 acc = 0;
    int bits = 0;
    for (char c : s) {
        const int v = val(c);
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

struct Client {
    mcp::Server server;
    std::vector<std::string> out;
    int next_id = 1;

    explicit Client(const mcp::ServerOptions& opt = {}) : server(opt) {
        server.set_sink([](StringView m, void* u) { static_cast<Client*>(u)->out.emplace_back(m); }, this);
    }
    std::unique_ptr<Parsed> request(const std::string& method, const std::string& params = "{}") {
        out.clear();
        server.handle(sv(R"({"jsonrpc":"2.0","id":)" + std::to_string(next_id++) + R"(,"method":")" + method + R"(","params":)" +
                         params + "}"));
        REQUIRE(!out.empty());
        // stdio framing: one message per line, whatever a tool puts in it.
        CHECK(out.back().find_first_of("\r\n") == std::string::npos);
        return parse(out.back());
    }
    void init() {
        auto r = request("initialize", R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}})");
        REQUIRE(r->root().get("result").is_object());
        out.clear();
        server.handle(sv(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }

    struct Tool {
        bool error = true;
        std::string text;
        std::unique_ptr<Parsed> value;   // structuredContent, when it succeeded
        std::vector<cook::Image> images;  // decoded image content items
        cook::JsonValue v() const { return value->root(); }
    };
    Tool tool(const std::string& name, const std::string& args = "{}") {
        auto r = request("tools/call", R"({"name":")" + name + R"(","arguments":)" + args + "}");
        const cook::JsonValue res = r->root().get("result");
        REQUIRE(res.is_object());
        Tool t;
        t.error = res.get("isError").as_bool();
        const cook::JsonValue content = res.get("content");
        t.text = str(content.at(0).get("text"));
        if (!t.error) t.value = parse(t.text);
        for (u32 i = 1; i < content.size(); ++i) {
            REQUIRE(str(content.at(i).get("type")) == "image");
            CHECK(str(content.at(i).get("mimeType")) == "image/png");
            const std::vector<u8> png = base64_decode(str(content.at(i).get("data")));
            Result<cook::Image> img = cook::decode_png(Span<const byte>(reinterpret_cast<const byte*>(png.data()), png.size()));
            REQUIRE(img);
            t.images.push_back(static_cast<cook::Image&&>(img.value()));
        }
        return t;
    }
    Tool ok(const std::string& name, const std::string& args = "{}") {
        Tool t = tool(name, args);
        REQUIRE_MESSAGE(!t.error, (name + ": " + t.text));
        return t;
    }
    std::string fails(const std::string& name, const std::string& args = "{}") {
        Tool t = tool(name, args);
        CHECK_MESSAGE(t.error, (name + " should fail: " + t.text));
        return t.text;
    }
};

}  // namespace mcp_client
