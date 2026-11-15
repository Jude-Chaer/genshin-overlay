-- What the player picked: types, saved sets, regions, the types hidden from
-- the bar, kept in selection.json. visible() is the one rule for the map and
-- the minimap.
--
-- A type is on the map when it's picked by hand or a set that's on has it,
-- unless it was turned off by hand while such a set was on. A set is on or
-- off only by its own switch, never because types were picked.

local catalog = require("catalog")
local collected = require("collected")

local selection = {}

local FILE = "selection.json"

selection.types = {}        -- type id -> true, picked by hand
selection.handOff = {}      -- type id -> true, turned off by hand while a set that's on has it
selection.sets = {}         -- { name, types = { [type id] = true }, on, icon = a type id or nil }
selection.on = {}           -- type id -> true, on the map: worked out by changed()
selection.hidden = {}       -- type id -> true, turned off from the bar
selection.hiddenBefore = nil -- while "hide all" is on: what was hidden before it
selection.regions = {}      -- region id -> true, none means everywhere
selection.here = false      -- "Where I am"
selection.bar = {}          -- the picked types in menu order
-- goes up with every change, for what's worked out from the selection
selection.version = 0

local function keys(set)
    local list = {}
    for key in pairs(set or {}) do list[#list + 1] = key end
    table.sort(list)
    return list
end

local function setOf(list)
    local set = {}
    for _, id in ipairs(type(list) == "table" and list or {}) do
        local value = math.tointeger(id)
        if value then set[value] = true end
    end
    return set
end

local function savedSets()
    local list = {}
    for _, set in ipairs(selection.sets) do
        list[#list + 1] = { name = set.name, types = keys(set.types), on = set.on, icon = set.icon }
    end
    return list
end

local function save()
    WriteFile(FILE, JSON_Encode({
        types = keys(selection.types),
        handOff = keys(selection.handOff),
        sets = savedSets(),
        hidden = keys(selection.hidden),
        hideAll = selection.hiddenBefore ~= nil,
        hiddenBefore = keys(selection.hiddenBefore),
        regions = keys(selection.regions),
        here = selection.here,
    }))
end

-- a set that's on has the type, other than the one at skip
local function inOnSet(id, skip)
    for i, set in ipairs(selection.sets) do
        if i ~= skip and set.on and set.types[id] then return true end
    end
    return false
end

-- the bar follows the menu's order, and only has types the item files still have
function selection.changed()
    selection.bar, selection.on = {}, {}
    for _, kind in ipairs(catalog.types) do
        local id = kind.id
        if not selection.handOff[id] and (selection.types[id] or inOnSet(id)) then
            selection.on[id] = true
            selection.bar[#selection.bar + 1] = id
        end
    end
    selection.version = selection.version + 1
end

-- common chests the first time
local function firstStart()
    for _, kind in ipairs(catalog.types) do
        if kind.name == "Common Chest" then return { [kind.id] = true } end
    end
    return {}
end

local saved

function selection.load()
    saved = FileExists(FILE) and JSON_Decode(ReadFile(FILE)) or nil
    if type(saved) ~= "table" then saved = nil end
    if saved then
        selection.types = setOf(saved.types)
        selection.handOff = setOf(saved.handOff)
        selection.sets = {}
        for _, set in ipairs(type(saved.sets) == "table" and saved.sets or {}) do
            if type(set) == "table" and type(set.name) == "string" then
                selection.sets[#selection.sets + 1] = {
                    name = set.name, types = setOf(set.types), on = set.on == true, icon = math.tointeger(set.icon) }
            end
        end
        selection.hidden = setOf(saved.hidden)
        selection.hiddenBefore = saved.hideAll and setOf(saved.hiddenBefore) or nil
        selection.regions = setOf(saved.regions)
        selection.here = saved.here == true
    end
end

-- once the item files are there: a first start picks its types then
function selection.ready()
    if not saved and next(selection.types) == nil then
        selection.types = firstStart()
        if next(selection.types) then save() end
    end
    selection.changed()
end

-- on the map: picked by hand, or brought back while a set has it; off: taken
-- off, staying off while a set that's on has it
function selection.pick(list, on)
    for _, id in ipairs(list) do
        if on then
            selection.handOff[id] = nil
            selection.types[id] = not inOnSet(id) or nil
        else
            selection.types[id] = nil
            selection.handOff[id] = inOnSet(id) or nil
        end
        selection.hidden[id] = nil
    end
    selection.changed()
    save()
end

-- A set on shows all its types; off takes away the ones no other set that's on
-- has and that weren't picked by hand.
function selection.setOn(index, on)
    local set = selection.sets[index]
    if not set or set.on == on then return end
    set.on = on
    for id in pairs(set.types) do
        if on or not inOnSet(id) then selection.handOff[id] = nil end
        if on then selection.hidden[id] = nil end
    end
    selection.changed()
    save()
end

function selection.allSetsOff()
    for _, set in ipairs(selection.sets) do set.on = false end
    selection.handOff = {}
    selection.changed()
    save()
end

-- what's on the map now as a new set, on; its types are the set's then, not
-- picked by hand, so turning it off takes them away
function selection.saveSet(name)
    local types = {}
    for id in pairs(selection.on) do
        types[id] = true
        selection.types[id] = nil
    end
    selection.sets[#selection.sets + 1] = { name = name, types = types, on = true }
    selection.changed()
    save()
end

function selection.renameSet(index, name)
    local set = selection.sets[index]
    if not set or name == "" or name == set.name then return end
    set.name = name
    save()
end

-- the type whose icon the set shows, one of its own
function selection.setIcon(index, id)
    local set = selection.sets[index]
    if not set or not set.types[id] then return end
    set.icon = id
    save()
end

function selection.deleteSet(index)
    if not selection.sets[index] then return end
    selection.setOn(index, false)
    table.remove(selection.sets, index)
    selection.changed()
    save()
end

-- nothing on the map: no types picked, every set off
function selection.clear()
    selection.types, selection.handOff, selection.hidden, selection.hiddenBefore = {}, {}, {}, nil
    for _, set in ipairs(selection.sets) do set.on = false end
    selection.changed()
    save()
end

function selection.toggleHidden(id)
    selection.hidden[id] = not selection.hidden[id] or nil
    -- picking by hand after "hide all" leaves it
    selection.hiddenBefore = nil
    selection.version = selection.version + 1
    save()
end

-- hides every type on the bar, or brings back the ones showing before
function selection.toggleHideAll()
    if selection.hiddenBefore then
        selection.hidden, selection.hiddenBefore = selection.hiddenBefore, nil
    else
        selection.hiddenBefore = {}
        for id in pairs(selection.hidden) do selection.hiddenBefore[id] = true end
        for _, id in ipairs(selection.bar) do selection.hidden[id] = true end
    end
    selection.version = selection.version + 1
    save()
end

function selection.setEverywhere()
    selection.regions, selection.here = {}, false
    selection.version = selection.version + 1
    save()
end

function selection.setHere(on)
    selection.here, selection.regions = on, {}
    selection.version = selection.version + 1
    save()
end

function selection.toggleRegion(id)
    selection.regions[id] = not selection.regions[id] or nil
    selection.here = false
    selection.version = selection.version + 1
    save()
end

-- the region of the last spot the map or minimap showed, for "Where I am"
selection.lastRegion = nil
-- goes up when "Where I am" moves into another region; what the map keeps of
-- the points drawn goes by it as well as by selection.version
selection.whereVersion = 0

-- the regions to draw around a spot, a set of ids or nil for everywhere;
-- "Where I am" outside of every region shows everything
function selection.regionFilter(mapId, x, y)
    local here = catalog.regionAt(mapId, x, y)
    if (here and here.id) ~= (selection.lastRegion and selection.lastRegion.id) then
        selection.lastRegion = here
        -- only what's drawn changes, not the picks: the grid and the counts stay
        if selection.here then selection.whereVersion = selection.whereVersion + 1 end
    end
    if selection.here then return here and { [here.id] = true } or nil end
    if next(selection.regions) then return selection.regions end
    return nil
end

-- the types drawn now: picked and not hidden from the bar
function selection.shownTypes()
    local list = {}
    for _, id in ipairs(selection.bar) do
        if not selection.hidden[id] then list[#list + 1] = id end
    end
    return list
end

-- The shown types of one map in one grid, made again when the selection changes
local shown = { key = nil, cells = {}, cellCount = 0 }

function selection.grid(mapId)
    local key = mapId .. ":" .. selection.version
    if shown.key == key then return shown end
    local cells, count = {}, 0
    for _, typeId in ipairs(selection.shownTypes()) do
        local grid = catalog.on(typeId, mapId)
        if grid then
            for cellKey, list in pairs(grid.cells) do
                local cell = cells[cellKey]
                if not cell then
                    cell = {}
                    cells[cellKey] = cell
                    count = count + 1
                end
                for _, i in ipairs(list) do cell[#cell + 1] = grid.points[i] end
            end
        end
    end
    shown.key, shown.cells, shown.cellCount = key, cells, count
    return shown
end

-- not on the layer showing: above ground anything underground, underground
-- anything but the floor showing
function selection.otherLayer(point, groupId, floorId)
    if groupId == 0 then return point.group ~= 0 end
    return point.group ~= groupId or point.floor ~= floorId
end

-- What visible() goes by, read once a frame: the layer showing, the regions,
-- that place's "hide collected" setting, and keep, a point id that always
-- shows (the one whose guide is open).
function selection.rules(mapId, x, y, groupId, floorId, hideCollected, keep)
    return {
        filter = selection.regionFilter(mapId, x, y),
        groupId = groupId,
        floorId = floorId,
        hideCollected = hideCollected,
        hideOther = GetSetting("hideOtherLayers"),
        keep = keep,
    }
end

-- "up" or "down" for a point on another layer: above ground is up from any
-- underground floor, anything underground is down from above ground and from
-- other floors
function selection.layerArrow(point, groupId, floorId)
    if not selection.otherLayer(point, groupId, floorId) then return nil end
    if groupId ~= 0 and point.group == 0 then return "up" end
    return "down"
end

-- the one rule for the map and the minimap
function selection.visible(point, rules)
    if point.id == rules.keep then return true end
    if rules.filter and not rules.filter[point.area] then return false end
    if rules.hideCollected and collected.has(point.id) then return false end
    if rules.hideOther and selection.otherLayer(point, rules.groupId, rules.floorId) then return false end
    return true
end

return selection
