#pragma once
#include <cstddef>
#include <string>
#include <lua.hpp>

// string.find, match, gmatch and gsub match their pattern in C, where the
// guard's count hook never runs. Before each of them the same search is
// walked once with a step limit; one that would go past it is an error
// instead of a frozen overlay. Under the limit Lua's own function runs as
// before.
namespace LuaPatterns {
    // steps one call may take, a few hundredths of a second
    inline const long long MaxSteps = 20000000;

    enum class Mode { Find, Gmatch, Gsub };

    // whether the search ends within MaxSteps. A malformed pattern counts as
    // fitting, Lua's own function gives its error. init is 0 based, already
    // inside the subject; maxReplacements is for Gsub.
    bool fits(const std::string& subject, const std::string& pattern, Mode mode, size_t init = 0,
        long long maxReplacements = -1);

    // puts the step limit in front of the four functions in L's string table.
    // spent gets the steps each walk took, so they count toward the caller's
    // own limit.
    void guard(lua_State* L, void (*spent)(lua_State* L, long long steps) = nullptr);
}
