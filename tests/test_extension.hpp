#pragma once
#include <filesystem>
#include <map>
#include <string>
#include "extensions.hpp"

// Extensions to test with. Each one gets its own folder in the temp folder and
// is loaded the way the overlay loads the ones in its extensions folder.

namespace TestExtension {
    // where the extension with this name is put
    extern std::filesystem::path folder(const std::string& name);
    // Writes main.lua and the other files (name, what's in it) into a fresh
    // folder and loads it. nullptr if it didn't start.
    extern Extensions::Extension* load(const std::string& name, const std::string& mainLua,
        const std::map<std::string, std::string>& otherFiles = {});
    // runs Lua as that extension, false if it stopped with an error
    extern bool run(Extensions::Extension* extension, const std::string& lua);
    // one turn of the overlay's loop with a drawn frame: answers, then Update
    extern void turn();
    // lets go of every extension and what they registered
    extern void unloadAll();
}
