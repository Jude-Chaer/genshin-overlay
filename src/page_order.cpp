#include "page_order.hpp"
#include <set>

namespace PageOrder {
    const std::string NewFolderName = "Folder";

    static const Place Nowhere{ -1, -1 };

    List Arrange(const List& saved, const std::vector<std::string>& present) {
        std::set<std::string> here(present.begin(), present.end());
        std::set<std::string> used;
        List list;
        for (const Item& item : saved) {
            if (item.folder) {
                Item folder{ item.key.empty() ? NewFolderName : item.key, true, item.open, {} };
                for (const std::string& key : item.inside) {
                    if (here.count(key) && used.insert(key).second) folder.inside.push_back(key);
                }
                if (!folder.inside.empty()) list.push_back(folder);
            }
            else if (here.count(item.key) && used.insert(item.key).second) {
                list.push_back({ item.key });
            }
        }
        for (const std::string& key : present) {
            if (used.insert(key).second) list.push_back({ key });
        }
        return list;
    }

    Place Find(const List& list, const std::string& extension) {
        for (int top = 0; top < (int)list.size(); top++) {
            const Item& item = list[top];
            if (!item.folder) {
                if (item.key == extension) return { top, -1 };
                continue;
            }
            for (int inside = 0; inside < (int)item.inside.size(); inside++) {
                if (item.inside[inside] == extension) return { top, inside };
            }
        }
        return Nowhere;
    }

    std::vector<std::string> Flat(const List& list) {
        std::vector<std::string> keys;
        for (const Item& item : list) {
            if (!item.folder) keys.push_back(item.key);
            else keys.insert(keys.end(), item.inside.begin(), item.inside.end());
        }
        return keys;
    }

    static bool isFolder(const List& list, int top) {
        return top >= 0 && top < (int)list.size() && list[top].folder;
    }

    // an extension outside folders, a folder row, or an extension in a folder
    static bool valid(const List& list, Place at) {
        if (at.top < 0 || at.top >= (int)list.size()) return false;
        if (at.inside < 0) return at.inside == -1;
        return list[at.top].folder && at.inside < (int)list[at.top].inside.size();
    }

    static bool isExtension(const List& list, Place at) {
        return valid(list, at) && (at.inside >= 0 || !list[at.top].folder);
    }

    // removes empty folders, keeping at pointing at the same item
    static Place tidy(List& list, Place at) {
        for (int top = (int)list.size() - 1; top >= 0; top--) {
            if (!list[top].folder || !list[top].inside.empty()) continue;
            list.erase(list.begin() + top);
            if (top < at.top) at.top--;
        }
        return at;
    }

    static std::string freeName(const List& list) {
        std::set<std::string> taken;
        for (const Item& item : list) {
            if (item.folder) taken.insert(item.key);
        }
        std::string name = NewFolderName;
        for (int n = 2; taken.count(name); n++) name = NewFolderName + " " + std::to_string(n);
        return name;
    }

    Place Move(List& list, Place from, Place before) {
        if (!valid(list, from)) return Nowhere;
        Item item = from.inside >= 0 ? Item{ list[from.top].inside[from.inside] } : list[from.top];

        if (before.inside >= 0) {
            if (item.folder || !isFolder(list, before.top)) return Nowhere;
            if (before.inside > (int)list[before.top].inside.size()) return Nowhere;
        }
        else if (before.inside != -1 || before.top < 0 || before.top > (int)list.size()) {
            return Nowhere;
        }

        bool sameFolder = from.inside >= 0 && before.inside >= 0 && from.top == before.top;
        bool sameLevel = sameFolder || (from.inside == -1 && before.inside == -1);
        if (sameLevel) {
            int at = sameFolder ? from.inside : from.top;
            int to = sameFolder ? before.inside : before.top;
            if (to == at || to == at + 1) return Nowhere;
        }

        if (from.inside >= 0) {
            auto& inside = list[from.top].inside;
            inside.erase(inside.begin() + from.inside);
            if (sameFolder && before.inside > from.inside) before.inside--;
        }
        else {
            list.erase(list.begin() + from.top);
            if (before.top > from.top) before.top--;
        }

        if (before.inside >= 0) {
            auto& inside = list[before.top].inside;
            inside.insert(inside.begin() + before.inside, item.key);
        }
        else {
            list.insert(list.begin() + before.top, item);
        }
        return tidy(list, before);
    }

    Place Up(List& list, Place at) {
        if (!valid(list, at)) return Nowhere;
        if (at.inside == 0) return Nowhere;
        if (at.inside > 0) return Move(list, at, { at.top, at.inside - 1 });
        return Move(list, at, { at.top - 1, -1 });
    }

    Place Down(List& list, Place at) {
        if (!valid(list, at)) return Nowhere;
        if (at.inside >= 0) {
            if (at.inside + 1 >= (int)list[at.top].inside.size()) return Nowhere;
            return Move(list, at, { at.top, at.inside + 2 });
        }
        if (at.top + 1 >= (int)list.size()) return Nowhere;
        return Move(list, at, { at.top + 2, -1 });
    }

    Place ToTop(List& list, Place at) {
        if (!valid(list, at)) return Nowhere;
        if (at.inside >= 0) return Move(list, at, { at.top, 0 });
        return Move(list, at, { 0, -1 });
    }

    Place ToBottom(List& list, Place at) {
        if (!valid(list, at)) return Nowhere;
        if (at.inside >= 0) return Move(list, at, { at.top, (int)list[at.top].inside.size() });
        return Move(list, at, { (int)list.size(), -1 });
    }

    Place IntoFolder(List& list, Place at, int folder) {
        if (!isExtension(list, at) || !isFolder(list, folder) || at.top == folder) return Nowhere;
        return Move(list, at, { folder, (int)list[folder].inside.size() });
    }

    Place OutOfFolder(List& list, Place at) {
        if (!isExtension(list, at) || at.inside < 0) return Nowhere;
        return Move(list, at, { at.top + 1, -1 });
    }

    Place NewFolder(List& list, Place at) {
        if (!isExtension(list, at)) return Nowhere;
        Item folder{ freeName(list), true, true, {} };
        if (at.inside < 0) {
            folder.inside.push_back(list[at.top].key);
            list[at.top] = folder;
            return { at.top, 0 };
        }
        auto& inside = list[at.top].inside;
        folder.inside.push_back(inside[at.inside]);
        inside.erase(inside.begin() + at.inside);
        list.insert(list.begin() + at.top + 1, folder);
        return tidy(list, { at.top + 1, 0 });
    }

    Place Onto(List& list, Place from, Place target) {
        if (!isExtension(list, from) || !valid(list, target) || from == target) return Nowhere;
        if (target.inside >= 0) return Move(list, from, { target.top, target.inside + 1 });
        if (list[target.top].folder) return IntoFolder(list, from, target.top);
        // wrapping the target in place leaves from where it was
        NewFolder(list, target);
        return IntoFolder(list, from, target.top);
    }

    static std::string trimmed(const std::string& name) {
        size_t start = name.find_first_not_of(" \t");
        if (start == std::string::npos) return "";
        return name.substr(start, name.find_last_not_of(" \t") + 1 - start);
    }

    NameCheck CheckName(const List& list, int folder, const std::string& name) {
        std::string wanted = trimmed(name);
        if (wanted.empty()) return NameCheck::Empty;
        // ImGui reads ## as the start of an id
        if (wanted.find("##") != std::string::npos) return NameCheck::HasHashes;
        if (folder >= 0 && folder < (int)list.size() && list[folder].key == wanted) return NameCheck::Same;
        for (const auto& item : list)
            if (item.folder && item.key == wanted) return NameCheck::Taken;
        return NameCheck::Fine;
    }

    bool Rename(List& list, int folder, const std::string& name) {
        if (!isFolder(list, folder) || CheckName(list, folder, name) != NameCheck::Fine) return false;
        list[folder].key = trimmed(name);
        return true;
    }

    bool Ungroup(List& list, int folder) {
        if (!isFolder(list, folder)) return false;
        std::vector<std::string> keys = list[folder].inside;
        list.erase(list.begin() + folder);
        for (int i = 0; i < (int)keys.size(); i++) list.insert(list.begin() + folder + i, Item{ keys[i] });
        return true;
    }

    void History::Did(const List& before, const std::string& what) {
        undo.push_back({ before, what });
        if (undo.size() > Limit) undo.erase(undo.begin());
        redo.clear();
    }

    bool History::Undo(List& now) {
        if (undo.empty()) return false;
        redo.push_back({ now, undo.back().what });
        now = undo.back().list;
        undo.pop_back();
        return true;
    }

    bool History::Redo(List& now) {
        if (redo.empty()) return false;
        undo.push_back({ now, redo.back().what });
        now = redo.back().list;
        redo.pop_back();
        return true;
    }

    void History::Clear() {
        undo.clear();
        redo.clear();
    }

    nlohmann::json ToJson(const List& list) {
        nlohmann::json json = nlohmann::json::array();
        for (const Item& item : list) {
            if (item.folder) json.push_back({ { "folder", item.key }, { "open", item.open }, { "items", item.inside } });
            else json.push_back(item.key);
        }
        return json;
    }

    List FromJson(const nlohmann::json& json) {
        List list;
        if (!json.is_array()) return list;
        for (const auto& entry : json) {
            if (entry.is_string()) {
                list.push_back({ entry.get<std::string>() });
                continue;
            }
            if (!entry.is_object()) continue;
            auto name = entry.find("folder");
            if (name == entry.end() || !name->is_string()) continue;
            Item folder{ name->get<std::string>(), true, true, {} };
            auto open = entry.find("open");
            if (open != entry.end() && open->is_boolean()) folder.open = open->get<bool>();
            auto items = entry.find("items");
            if (items != entry.end() && items->is_array()) {
                for (const auto& key : *items) {
                    if (key.is_string()) folder.inside.push_back(key.get<std::string>());
                }
            }
            list.push_back(folder);
        }
        return list;
    }
}
