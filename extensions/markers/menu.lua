-- The menu page: search, where (everywhere, where I am, picked regions), the
-- saved sets, and the groups down the side with their types as rows beside.

local catalog = require("catalog")
local selection = require("selection")
local collected = require("collected")

local menu = {}

local search = ""

-- found and total, worked out again only when something they count changed
local typeCounts, regionCounts = {}, {}

local function countKey()
    return catalog.version .. ":" .. collected.version .. ":" .. selection.version
end

-- the regions counted: the ones picked, the one the player is in for "Where I am", or all
local function countedRegions()
    if selection.here then
        local here = selection.lastRegion
        return here and { [here.id] = true } or nil
    end
    if next(selection.regions) then return selection.regions end
    return nil
end

local function typeCount(typeId)
    local key = countKey()
    local c = typeCounts[typeId]
    if c and c.key == key then return c.found, c.total end
    local filter = countedRegions()
    local found, total = 0, 0
    catalog.each(typeId, function(point)
        if not filter or filter[point.area] then
            total = total + 1
            if collected.has(point.id) then found = found + 1 end
        end
    end)
    typeCounts[typeId] = { key = key, found = found, total = total }
    return found, total
end

-- per region, over the types picked
local function regionCount(regionId)
    local key = countKey()
    if regionCounts.key ~= key then
        local counts = {}
        for _, typeId in ipairs(selection.bar) do
            catalog.each(typeId, function(point)
                local c = counts[point.area]
                if not c then
                    c = { found = 0, total = 0 }
                    counts[point.area] = c
                end
                c.total = c.total + 1
                if collected.has(point.id) then c.found = c.found + 1 end
            end)
        end
        regionCounts = { key = key, counts = counts }
    end
    local c = regionCounts.counts[regionId]
    if not c then return 0, 0 end
    return c.found, c.total
end

local function chip(label, on, icon, note)
    return ImGUI_Toggle(label, on, { look = "chip", icon = icon, note = note })
end

-- "Pick regions" stays open while none is picked yet
local picking = false

local WHERE = { "Everywhere", "Where I am", "Pick regions" }

local function whereNow()
    if picking or next(selection.regions) ~= nil then return "Pick regions" end
    return selection.here and "Where I am" or "Everywhere"
end

-- the search bar with Everywhere / Where I am / Pick regions at its end
local function searchAndWhere()
    local now = whereNow()
    local picked
    search, picked = ImGUI_InputText("Search", search,
        { look = "search", hint = "Search map items", tabs = WHERE, tab = now })
    if picked and picked ~= now then
        picking = picked == "Pick regions"
        if picked == "Everywhere" then selection.setEverywhere()
        elseif picked == "Where I am" then selection.setHere(true)
        elseif selection.here then selection.setHere(false) end
    end
end

-- the regions as chips, only while picking them
local function regionChips()
    if whereNow() ~= "Pick regions" then return end
    for _, region in ipairs(catalog.regions) do
        local on = selection.regions[region.id] == true
        local note
        if #selection.bar > 0 then
            local found, total = regionCount(region.id)
            if total > 0 then note = found .. "/" .. total end
        end
        if chip(region.name .. "##region" .. region.id, on, nil, note) ~= on then selection.toggleRegion(region.id) end
    end
end

-- "All" while one of the types is off, "None" once all are on
local function allButton(types, id)
    local ids, all = {}, true
    for _, kind in ipairs(types) do
        ids[#ids + 1] = kind.id
        if not selection.on[kind.id] then all = false end
    end
    ImGUI_Button((all and "None" or "All") .. "##all" .. id, function() selection.pick(ids, not all) end)
end

-- each type a row: its icon, name, found / total and a box
local function typeRows(types)
    for _, kind in ipairs(types) do
        local on = selection.on[kind.id] == true
        local found, total = typeCount(kind.id)
        local now = ImGUI_Toggle(kind.name .. "##type" .. kind.id, on,
            { look = "checkbox", icon = catalog.icon(kind.id), note = found .. " / " .. total })
        if now ~= on then selection.pick({ kind.id }, now) end
    end
end

local function matching(text)
    local out, want = {}, text:lower()
    for _, kind in ipairs(catalog.types) do
        if catalog.plain(kind.name):lower():find(want, 1, true) then out[#out + 1] = kind end
    end
    return out
end

-- the icon a set shows: the one picked for it, else its first type's in menu order
local function setIcon(set)
    if set.icon and set.types[set.icon] then return catalog.icon(set.icon) end
    for _, kind in ipairs(catalog.types) do
        if set.types[kind.id] then return catalog.icon(kind.id) end
    end
    return nil
end

local function countIn(set)
    local n = 0
    for id in pairs(set.types) do
        if catalog.typeOf[id] then n = n + 1 end
    end
    return n
end

-- the set being renamed, nil for none, and its box
local renaming = nil
local renameBox = nil
-- the box for a new set's name while saving one, nil while not saving
local naming = nil
-- the set just saved, told under the sets until something else is done with them
local justSaved = nil

-- a set's right-click menu: its types to pick the icon from, in menu order;
-- kept per set until a type in it changes or an icon was still loading
local menus = setmetatable({}, { __mode = "k" })
local function setMenu(set)
    local kept = menus[set]
    if kept and kept.version == catalog.version and kept.count == countIn(set) and kept.complete then
        return kept.items, kept.byName
    end
    local icons, byName, complete = {}, {}, true
    for _, kind in ipairs(catalog.types) do
        if set.types[kind.id] and #icons < 64 then
            -- the id keeps two types with the same name apart
            local name = kind.name .. "##icon" .. kind.id
            local icon = catalog.icon(kind.id)
            if icon == 0 then complete = false end
            icons[#icons + 1] = { name = name, icon = icon }
            byName[name] = kind.id
        end
    end
    local items = { { name = "Rename" }, { name = "Icon", items = icons }, { name = "Delete" } }
    menus[set] = { items = items, byName = byName, version = catalog.version, count = countIn(set), complete = complete }
    return items, byName
end

-- a name box with its buttons, box = { text = ... } holding what's typed: gives
-- the name once it's sent (the button or Enter), false when cancelled (Cancel
-- or Esc), nil while still typing
local function nameBox(id, box, sendLabel)
    local key
    box.text, key = ImGUI_InputText("Name##" .. id, box.text)
    local result = nil
    local name = box.text:match("^%s*(.-)%s*$")
    if key == "enter" and name ~= "" then result = name end
    if key == "escape" then result = false end
    ImGUI_Button(sendLabel .. "##" .. id, function() if name ~= "" then result = name end end, { look = "main" })
    ImGUI_SameLine()
    ImGUI_Button("Cancel##" .. id, function() result = false end)
    return result
end

-- the Sets page beside the list: each set a row, on or off by itself, with
-- a right-click to rename it, pick its icon or delete it; a new set is what's
-- on the map now
local function setsPage()
    ImGUI_Text("Sets", { look = "title" })
    local anyOn = false
    for _, each in ipairs(selection.sets) do anyOn = anyOn or each.on end
    if anyOn then
        ImGUI_SameLine()
        ImGUI_Button("All off##sets", function()
            selection.allSetsOff()
            justSaved = nil
        end)
    end

    if naming then
        local name = nameBox("newset" .. #selection.sets, naming, "Save")
        if name then
            selection.saveSet(name)
            justSaved = name
        end
        if name ~= nil then naming = nil end
    elseif #selection.bar > 0 then
        local count = #selection.bar
        ImGUI_Button("New set from the " .. (count == 1 and "1 type" or count .. " types") .. " on the map", function()
            naming = { text = "Set " .. (#selection.sets + 1) }
            justSaved = nil
        end, { look = "main" })
    end
    if justSaved then ImGUI_Empty("\"" .. justSaved .. "\" is saved and on.") end
    if #selection.sets == 0 then
        if not naming then ImGUI_Empty("A set is a few types to turn on and off together. Pick types, then save them here.") end
        return
    end

    for i, set in ipairs(selection.sets) do
        local n = countIn(set)
        local now = ImGUI_Toggle(set.name .. "##set" .. i, set.on,
            { look = "checkbox", icon = setIcon(set), note = n == 1 and "1 type" or n .. " types" })
        ImGUI_Tooltip("Right-click to rename it, change its icon or delete it.")
        if now ~= set.on then
            selection.setOn(i, now)
            justSaved = nil
        end
        local items, byName = setMenu(set)
        local picked, from = ImGUI_RightClick("set" .. i, items)
        if picked then justSaved = nil end
        if from == "Icon" then selection.setIcon(i, byName[picked])
        elseif picked == "Rename" then renaming, renameBox = i, { text = set.name }
        elseif picked == "Delete" then
            selection.deleteSet(i)
            renaming = nil
            break
        end
        if renaming == i then
            local name = nameBox("rename" .. i, renameBox, "Rename")
            if name then selection.renameSet(i, name) end
            if name ~= nil then renaming = nil end
        end
    end
end

-- the Sets entry's line in the list, like a group's
local function setsSummary()
    local on = 0
    for _, set in ipairs(selection.sets) do
        if set.on then on = on + 1 end
    end
    return on > 0 and (on .. " on") or nil
end

-- a group's summary in the list: how many of its types are on
local function groupSummary(group)
    local on = 0
    for _, kind in ipairs(group.types) do
        if selection.on[kind.id] then on = on + 1 end
    end
    return on == #group.types and "All on" or on > 0 and (on .. " on") or nil
end

function menu.draw()
    local items = GetItems()
    if #catalog.groups == 0 then
        if items.state == "downloading" then
            ImGUI_Progress("Getting the map items", items.percent / 100)
        else
            ImGUI_Empty("No map items yet, nothing to pick from.")
        end
        return
    end

    searchAndWhere()
    regionChips()

    -- what's on the map, and Clear beside it; it asks first, it takes everything off
    local count = #selection.bar
    ImGUI_Text(count == 0 and "Nothing on the map" or count == 1 and "1 type on the map" or count .. " types on the map",
        { look = "title" })
    if count > 0 then
        ImGUI_SameLine()
        ImGUI_Button("Clear all##clear", function() selection.clear() end, { look = "risky" })
    end
    if search ~= "" then
        local list = matching(search)
        if #list == 0 then
            ImGUI_Empty("No type has \"" .. search .. "\" in its name.")
        else
            typeRows(list)
        end
        return
    end

    -- the list: Sets first, then the groups
    local summary = setsSummary()
    local list = { { name = "Sets", glyph = "sets", summary = summary, on = summary ~= nil, line = true } }
    for _, group in ipairs(catalog.groups) do
        summary = groupSummary(group)
        list[#list + 1] = { name = group.name, icon = catalog.icon(group.icon), summary = summary, on = summary ~= nil }
    end
    -- the first group opens at first, the Sets page only once picked
    local picked = ImGUI_SideList("groups", list, { open = 2 }) or 2
    if picked == 1 then
        setsPage()
        return
    end
    local open = catalog.groups[picked - 1]
    if not open then return end
    ImGUI_Text(open.name, { look = "title" })
    allButton(open.types, "group" .. open.id)
    typeRows(open.types)
end

return menu
