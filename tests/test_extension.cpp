#include <windows.h>
#include "test_extension.hpp"
#include "helpers.hpp"
#include <fstream>

namespace TestExtension {
    std::filesystem::path folder(const std::string& name) {
        std::filesystem::path root = std::filesystem::temp_directory_path() / "genshin-overlay-tests" / "extensions";
        std::filesystem::create_directories(root);
        // the file functions compare cleaned up paths, so hand them a clean one
        return std::filesystem::canonical(root) / name;
    }

    Extensions::Extension* load(const std::string& name, const std::string& mainLua,
        const std::map<std::string, std::string>& otherFiles) {
        std::filesystem::path path = folder(name);
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        std::ofstream(path / "main.lua", std::ios::binary) << mainLua;
        for (const auto& [file, text] : otherFiles) {
            // names are UTF-8, as from Lua
            std::ofstream(path / Helpers::fromUtf8(file), std::ios::binary) << text;
        }

        size_t before = Extensions::registeredExtensions.size();
        Extensions::loadLuaExtension(path / "main.lua", path);
        if (Extensions::registeredExtensions.size() == before) return nullptr;
        return Extensions::registeredExtensions.back().get();
    }

    bool run(Extensions::Extension* extension, const std::string& lua) {
        bool ok = false;
        Extensions::runForExtension(extension, [&]() {
            ok = extension->luaState->safe_script(lua, sol::script_pass_on_error).valid();
            });
        return ok;
    }

    // each takes its keybinds and intervals with it, see Extensions::unload
    void unloadAll() {
        Extensions::registeredExtensions.clear();
    }
}
