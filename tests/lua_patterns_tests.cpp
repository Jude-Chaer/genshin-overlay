#include <chrono>
#include <filesystem>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include "extensions.hpp"
#include "lua_patterns.hpp"
#include "settings.hpp"
#include "test_extension.hpp"

using LuaPatterns::Mode;

static Extensions::Extension* load(const std::string& name, const std::string& mainLua = "function Init() end") {
    std::filesystem::remove(Settings::file);
    return TestExtension::load(name, mainLua);
}

TEST_CASE("a search that would run too long is found before it runs") {
    std::string text(100000, 'a');
    auto start = std::chrono::steady_clock::now();
    CHECK(!LuaPatterns::fits(text, ".-.-.-b", Mode::Find));
    CHECK(!LuaPatterns::fits(text, ".-.-.-b", Mode::Gmatch));
    CHECK(!LuaPatterns::fits(text, ".-.-.-b", Mode::Gsub));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));

    CHECK(LuaPatterns::fits(text, "a+", Mode::Find));
    CHECK(LuaPatterns::fits(text, "a", Mode::Gsub));
    CHECK(LuaPatterns::fits(text, "^.-.-.-b", Mode::Gsub, 0, 0));
    // a malformed pattern is left to Lua's own error
    CHECK(LuaPatterns::fits(text, "[a", Mode::Find));
}

TEST_CASE("string.find, match, gmatch and gsub give what Lua gives") {
    Extensions::Extension* ext = load("patterns");
    REQUIRE(ext != nullptr);
    REQUIRE(TestExtension::run(ext, R"lua(
        local wrong = {}
        local function same(name, got, want)
            if got ~= want then wrong[#wrong + 1] = name .. ": " .. tostring(got) end
        end
        local function all(...) return table.concat({ ... }, ",") end
        local function words(text, pattern, init)
            local found = {}
            for w in text:gmatch(pattern, init) do found[#found + 1] = w end
            return table.concat(found, ",")
        end

        same("find", all(("hello world"):find("o w")), "5,7")
        same("find +", all(("hello world"):find("l+")), "3,4")
        same("find init", all(("hello world"):find("o", 6)), "8,8")
        same("find from end", ("hello world"):find("o", -3), nil)
        same("find past end", ("abc"):find("", 10), nil)
        same("find plain", all(("a.b"):find(".", 1, true)), "2,2")
        same("find capture", all(("THE (quick) fox"):find("%((%a+)%)")), "5,11,quick")
        same("match", all(("key = value"):match("(%w+)%s*=%s*(%w+)")), "key,value")
        same("match anchor", ("abc"):match("^b"), nil)
        same("match anchor 2", ("abc"):match("^a"), "a")
        same("match positions", all(("hello"):match("()ll()")), "3,5")
        same("match balance", ("f(a(b)c) d"):match("%b()"), "(a(b)c)")
        same("match back", all(('say "hi" now'):match('(["\'])(.-)%1')), '",hi')
        same("match end", ("abc"):match("c$"), "c")
        same("string.match", string.match("x=1", "%d"), "1")
        same("gmatch", words("one two  three", "%a+"), "one,two,three")
        same("gmatch init", words("one two", "%a+", 4), "two")
        same("gmatch empty", words("ab", ""), ",,")
        same("gsub", all(("hello world"):gsub("o", "0")), "hell0 w0rld,2")
        same("gsub most", all(("hello world"):gsub("o", "0", 1)), "hell0 world,1")
        same("gsub empty", all(("abc"):gsub("", "-")), "-a-b-c-,4")
        same("gsub anchor", all(("hello"):gsub("^h", "H")), "Hello,1")
        same("gsub frontier", all(("THE (QUICK) FOX"):gsub("%f[%a]%a+", string.lower)), "the (quick) fox,3")
        same("gsub swap", all(("x = 1, y = 2"):gsub("(%w+) = (%w+)", "%2 = %1")), "1 = x, 2 = y,2")
        same("gsub table", all(("$a $b"):gsub("%$(%w+)", { a = "1" })), "1 $b,2")
        result = table.concat(wrong, "; ")
    )lua"));
    CHECK((*ext->luaState)["result"].get_or(std::string("not run")) == "");
    TestExtension::unloadAll();
}

TEST_CASE("a search too long for the text is an error for the extension") {
    Extensions::Extension* ext = load("long_search");
    REQUIRE(ext != nullptr);
    auto start = std::chrono::steady_clock::now();
    // one each, the steps of two would stop the call
    REQUIRE(TestExtension::run(ext, R"(
        text = ("a"):rep(100000)
        local ok, problem = pcall(string.find, text, ".-.-.-b")
        refused = not ok
        message = problem
    )"));
    REQUIRE(TestExtension::run(ext, R"(
        refused = refused and not pcall(function() return text:gsub(".-.-.-b", "") end)
    )"));
    REQUIRE(TestExtension::run(ext, R"(
        refused = refused and not pcall(function() for _ in text:gmatch(".-.-.-b") do end end)
        local ok, problem = pcall(string.find, "abc", "[a")
        malformed = not ok and problem:find("malformed pattern", 1, true) ~= nil
    )"));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK((*ext->luaState)["refused"].get_or(false) == true);
    CHECK((*ext->luaState)["message"].get_or(std::string()).find("string.find: the pattern takes too long on this text") != std::string::npos);
    CHECK((*ext->luaState)["malformed"].get_or(false) == true);
    CHECK(ext->stopReason.empty());
    TestExtension::unloadAll();
}

TEST_CASE("searches in a loop count toward the guard's steps") {
    const char* loops[] = {
        // each one refused
        "function Update() local text = ('a'):rep(100000) while true do pcall(string.find, text, '.-.-.-b') end end",
        // each one under the limit, about 9 million steps
        "function Update() local text = ('a'):rep(3000) while true do text:find('.-b') end end",
    };
    for (const char* loop : loops) {
        INFO(loop);
        Extensions::Extension* ext = TestExtension::load("search_loops", loop);
        REQUIRE(ext != nullptr);
        auto start = std::chrono::steady_clock::now();
        TestExtension::turn();
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
        CHECK(!ext->stopReason.empty());
        TestExtension::turn();
        CHECK(ext->stopped);
        TestExtension::unloadAll();
    }
}
