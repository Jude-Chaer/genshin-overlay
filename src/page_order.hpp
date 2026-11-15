#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// The player's order of the extensions in the page list, and their folders.
// Extensions are named by their folder, folders hold extensions only.

namespace PageOrder {
    struct Item {
        // the extension's folder, or the folder's name
        std::string key;
        bool folder = false;
        bool open = true;
        std::vector<std::string> inside;
    };
    using List = std::vector<Item>;

    // A spot in the list: top is the index in the list, inside the index in
    // that folder, -1 for the folder row itself or an extension outside folders.
    struct Place {
        int top = -1;
        int inside = -1;
        bool operator==(const Place& other) const { return top == other.top && inside == other.inside; }
    };

    extern const std::string NewFolderName;

    // The saved list fitted to the extensions there now, given in load order:
    // missing ones and empty folders are dropped, new ones go at the end.
    extern List Arrange(const List& saved, const std::vector<std::string>& present);

    // where an extension is, {-1, -1} when it isn't in the list
    extern Place Find(const List& list, const std::string& extension);
    // the extensions in the order they're shown, folders' ones included
    extern std::vector<std::string> Flat(const List& list);

    // Moves the item at from so it sits before the item at before, both in
    // the list as it is now. before.top == size puts it at the end of the
    // list, before.inside == the folder's size at the end of that folder.
    // A folder can't go into a folder. Folders left empty are removed.
    // Gives the item's new place, {-1, -1} when nothing moved.
    extern Place Move(List& list, Place from, Place before);
    extern Place Up(List& list, Place at);
    extern Place Down(List& list, Place at);
    extern Place ToTop(List& list, Place at);
    extern Place ToBottom(List& list, Place at);
    extern Place IntoFolder(List& list, Place at, int folder);
    // out of its folder, right after it
    extern Place OutOfFolder(List& list, Place at);
    // A new folder holding the extension, where the extension was, or right
    // after the folder it was in. Named NewFolderName, with a number after it
    // when that name is taken.
    extern Place NewFolder(List& list, Place at);
    // Drops the extension at from onto another item: onto a folder it goes in
    // at the end, onto an extension in a folder right after it, and onto an
    // extension outside folders the two make a new folder there.
    extern Place Onto(List& list, Place from, Place target);
    extern bool Rename(List& list, int folder, const std::string& name);
    // the folder's extensions in its place
    extern bool Ungroup(List& list, int folder);

    // Earlier lists for undo and redo. Did is called with the list as it was
    // before a change, which clears redo. A list Undo or Redo gives back can
    // name extensions removed since, so it goes through Arrange.
    class History {
    public:
        void Did(const List& before);
        bool Undo(List& now);
        bool Redo(List& now);
        bool CanUndo() const { return !undo.empty(); }
        bool CanRedo() const { return !redo.empty(); }
        void Clear();
        static constexpr size_t Limit = 50;
    private:
        std::vector<List> undo;
        std::vector<List> redo;
    };

    // saved as ["minimap", {"folder": "Folder", "open": true, "items": ["map"]}]
    extern nlohmann::json ToJson(const List& list);
    // anything that isn't one of those is skipped
    extern List FromJson(const nlohmann::json& json);
}
