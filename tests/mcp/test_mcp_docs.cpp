// mce://docs/... resources, docs_search, the MCP prompts and engine_capabilities (ROADMAP M-AGENT
// step 4) in process. A build tree serves the source tree's docs; --docs points at another folder.

#include <doctest/doctest.h>

#include <string>

#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

std::string read(Client& c, const std::string& uri) {
    auto r = c.request("resources/read", R"({"uri":")" + uri + R"("})");
    const cook::JsonValue contents = r->root().get("result").get("contents");
    return contents.is_array() ? str(contents.at(0).get("text")) : std::string();
}

}  // namespace

TEST_CASE("mce://docs: the guide, the engine reference, samples and examples as resources") {
    Client c;
    c.init();
    auto list = c.request("resources/list");
    const cook::JsonValue res = list->root().get("result").get("resources");
    std::string uris;
    for (u32 i = 0; i < res.size(); ++i) uris += str(res.at(i).get("uri")) + "\n";
    CHECK(uris.find("mce://docs/index\n") != std::string::npos);
    CHECK(uris.find("mce://docs/guide/14-mcp-ai-workflow.md\n") != std::string::npos);
    CHECK(uris.find("mce://docs/engine/modules.md\n") != std::string::npos);
    CHECK(uris.find("mce://docs/samples/\n") != std::string::npos);

    CHECK(read(c, "mce://docs/guide/14-mcp-ai-workflow.md").find("# 14. Working with an AI agent (MCP)") == 0);
    const std::string index = read(c, "mce://docs/index");
    CHECK(index.find("mce://docs/samples/03_platformer/main.cpp") != std::string::npos);
    CHECK(index.find("mce://docs/examples/Kindle/") != std::string::npos);
    CHECK(read(c, "mce://docs/samples/03_platformer/").find("mce://docs/samples/03_platformer/main.cpp") != std::string::npos);
    CHECK(read(c, "mce://docs/samples/03_platformer/main.cpp").find("#include") != std::string::npos);
    CHECK(read(c, "mce://docs/components/LocalTransform").find("\"position\"") != std::string::npos);
    // Only what ships: no other projects, no paths outside, no binaries.
    for (const char* bad : {"mce://docs/guide/../ROADMAP.md", "mce://docs/examples/SolsticeReach/README.md", "mce://docs/nope/x.md",
                            "mce://docs/samples/03_platformer/icon.ico", "mce://docs/components/NoSuchType"}) {
        auto r = c.request("resources/read", R"({"uri":")" + std::string(bad) + R"("})");
        CHECK_MESSAGE(r->root().get("error").is_object(), bad);
    }
    auto templates = c.request("resources/templates/list");
    CHECK(templates->root().get("result").get("resourceTemplates").size() == 2);
}

TEST_CASE("docs_search: ranked sections with their resource URI and a snippet") {
    Client c;
    c.init();
    auto t = c.ok("docs_search", R"({"query":"render_frame golden image_diff"})");
    const cook::JsonValue results = t.v().get("results");
    REQUIRE(results.size() >= 1);
    CHECK(str(results.at(0).get("uri")).find("mce://docs/") == 0);
    bool guide = false;
    for (u32 i = 0; i < results.size(); ++i) guide = guide || str(results.at(i).get("uri")) == "mce://docs/guide/14-mcp-ai-workflow.md";
    CHECK(guide);
    CHECK(str(results.at(0).get("snippet")).size() > 20);
    auto code = c.ok("docs_search", R"({"query":"PathAgent","group":"samples","limit":3})");
    CHECK(code.v().get("results").size() <= 3);
    for (u32 i = 0; i < code.v().get("results").size(); ++i)
        CHECK(str(code.v().get("results").at(i).get("uri")).find("mce://docs/samples/") == 0);
    CHECK(c.fails("docs_search", R"({"query":"!"})").find("words") != std::string::npos);
    CHECK(c.fails("docs_search", R"({"query":"x","group":"blog"})").find("'group'") != std::string::npos);
}

TEST_CASE("prompts: listed, filled with their arguments, missing ones refused") {
    Client c;
    c.init();
    auto list = c.request("prompts/list");
    const cook::JsonValue prompts = list->root().get("result").get("prompts");
    std::string names;
    for (u32 i = 0; i < prompts.size(); ++i) names += str(prompts.at(i).get("name")) + " ";
    CHECK(names == "start_game add_level polish_pass ");
    auto got = c.request("prompts/get", R"({"name":"start_game","arguments":{"brief":"A lighthouse keeper relights lamps."}})");
    const std::string text = str(got->root().get("result").get("messages").at(0).get("content").get("text"));
    CHECK(text.find("A lighthouse keeper relights lamps.") != std::string::npos);
    CHECK(text.find("{brief}") == std::string::npos);
    CHECK(got->root().get("result").get("messages").at(0).get("role").as_string() == "user");
    CHECK(c.request("prompts/get", R"({"name":"start_game"})")->root().get("error").is_object());
    CHECK(c.request("prompts/get", R"({"name":"nope"})")->root().get("error").is_object());
    auto polish = c.request("prompts/get", R"({"name":"polish_pass"})");
    CHECK(str(polish->root().get("result").get("messages").at(0).get("content").get("text")).find("{focus}") == std::string::npos);
}

TEST_CASE("engine_capabilities: features by reach, components, asset types and tools") {
    Client c;
    c.init();
    auto t = c.ok("engine_capabilities");
    const cook::JsonValue v = t.v();
    CHECK(v.get("features").size() > 10);
    std::string comps, exts, tools;
    for (u32 i = 0; i < v.get("components").size(); ++i) comps += str(v.get("components").at(i)) + " ";
    for (u32 i = 0; i < v.get("asset_types").size(); ++i) exts += str(v.get("asset_types").at(i).get("extension")) + " ";
    for (u32 i = 0; i < v.get("tools").size(); ++i) tools += str(v.get("tools").at(i)) + " ";
    CHECK(comps.find("LocalTransform ") != std::string::npos);
    CHECK(exts.find("png ") != std::string::npos);
    CHECK(exts.find("mcscene ") != std::string::npos);
    CHECK(tools.find("docs_search ") != std::string::npos);
    auto physics = c.ok("engine_capabilities", R"({"area":"physics"})");
    CHECK(physics.v().get("features").size() >= 2);
    CHECK_FALSE(physics.v().get("components").valid());
}

TEST_CASE("--docs: another folder is served, and a folder without docs serves nothing") {
    const std::string root = fresh_dir("mcp_docs_other");
    write_file(root + "/user-guide/README.md", "# Other guide\n\n## Section one\n\nzebra stripes everywhere\n");
    mcp::ServerOptions opt;
    const std::string docs = root;
    opt.docs = docs.c_str();
    Client c(opt);
    c.init();
    CHECK(read(c, "mce://docs/guide/README.md").find("# Other guide") == 0);
    auto t = c.ok("docs_search", R"({"query":"zebra"})");
    REQUIRE(t.v().get("results").size() == 1);
    CHECK(str(t.v().get("results").at(0).get("section")) == "Section one");
}
