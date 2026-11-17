-- The item files as Markers uses them: the types in their groups, the regions,
-- and each type's points split by map into a grid, so drawing only looks at
-- the cells on screen.

local catalog = {}

-- grid cells in map units
catalog.CELL = 256

catalog.version = 0
catalog.groups = {}     -- { id, name, types = { { id, name, total } } }
catalog.types = {}      -- every type in menu order
catalog.typeOf = {}     -- type id -> type
catalog.regions = {}    -- { id, name, map, left, top, right, bottom }
catalog.regionOf = {}   -- region id -> region

-- type id -> map id -> { points = { ... }, cells = { [key] = { indexes } } }
local loaded = {}
local icons = {}

-- loading a texture takes a moment, so a big group fills in over a few frames
local ICONS_PER_FRAME = 8
local iconsThisFrame = 0

local function cellKey(cx, cy)
    return cx * 65536 + cy
end
catalog.cellKey = cellKey

local function clear()
    catalog.groups, catalog.types, catalog.typeOf = {}, {}, {}
    catalog.regions, catalog.regionOf = {}, {}
    loaded, icons = {}, {}
end

-- the type whose icon stands for its group, by HoYoLAB's names
local GROUP_ICON = {
    ["Chests"] = "Precious Chest",
    ["Puzzle Chest"] = "Mini Puzzle",
    ["Animals"] = "Crimson Fox",
    ["Ores"] = "Crystal Chunk",
}
-- groups moved up to just before another
local BEFORE = { ["Experience"] = "Local Specialties" }

-- HoYoLAB writes some names with a no-break space ("Precious Chest")
local function plain(name)
    return (name:gsub("\194\160", " "))
end

catalog.plain = plain

local function findGroup(name)
    for i, group in ipairs(catalog.groups) do
        if group.name == name then return i end
    end
    return nil
end

local function read()
    clear()
    for _, group in ipairs(GetItemTypes() or {}) do
        local types = {}
        for _, kind in ipairs(group.types) do
            local total = 0
            for _, n in pairs(kind.counts) do total = total + n end
            if total > 0 then
                local entry = { id = kind.id, name = kind.name, total = total, group = group.id }
                types[#types + 1] = entry
                catalog.types[#catalog.types + 1] = entry
                catalog.typeOf[kind.id] = entry
            end
        end
        if #types > 0 then
            local icon = types[1].id
            for _, kind in ipairs(types) do
                if plain(kind.name) == GROUP_ICON[group.name] then icon = kind.id end
            end
            catalog.groups[#catalog.groups + 1] = { id = group.id, name = group.name, types = types, icon = icon }
        end
    end
    for name, before in pairs(BEFORE) do
        local from, to = findGroup(name), findGroup(before)
        if from and to and from > to then table.insert(catalog.groups, to, table.remove(catalog.groups, from)) end
    end
    -- the types in the groups' order, for the bar
    catalog.types = {}
    for _, group in ipairs(catalog.groups) do
        for _, kind in ipairs(group.types) do catalog.types[#catalog.types + 1] = kind end
    end
    for _, region in ipairs(GetItemRegions() or {}) do
        local entry = {
            id = region.id, name = region.name, map = region.map,
            left = region.box.left, top = region.box.top, right = region.box.right, bottom = region.box.bottom,
        }
        catalog.regions[#catalog.regions + 1] = entry
        catalog.regionOf[region.id] = entry
    end
end

-- true when a new list came in since the last call, everything kept from the old one is gone then
function catalog.update()
    iconsThisFrame = 0
    local version = GetItems().version
    if version == catalog.version then return false end
    catalog.version = version
    read()
    return true
end

local function load(typeId)
    local byMap = {}
    for _, point in ipairs(GetItemPositions(typeId) or {}) do
        local map = byMap[point.map]
        if not map then
            map = { points = {}, cells = {} }
            byMap[point.map] = map
        end
        local n = #map.points + 1
        point.type = typeId
        map.points[n] = point
        local key = cellKey(math.floor(point.x / catalog.CELL), math.floor(point.y / catalog.CELL))
        local cell = map.cells[key]
        if not cell then
            cell = {}
            map.cells[key] = cell
        end
        cell[#cell + 1] = n
    end
    loaded[typeId] = byMap
    return byMap
end

-- a type's points on one map with their grid, nil when it has none there
function catalog.on(typeId, mapId)
    local byMap = loaded[typeId] or load(typeId)
    return byMap[mapId]
end

-- every point of a type, all maps
function catalog.each(typeId, fn)
    for _, map in pairs(loaded[typeId] or load(typeId)) do
        for _, point in ipairs(map.points) do fn(point) end
    end
end

function catalog.icon(typeId)
    local icon = icons[typeId]
    if icon then return icon end
    if iconsThisFrame >= ICONS_PER_FRAME then return 0 end
    iconsThisFrame = iconsThisFrame + 1
    icon = GetItemIcon(typeId) or 0
    icons[typeId] = icon
    return icon
end

-- the region a spot is in; the boxes overlap at the edges, the smallest one wins
function catalog.regionAt(mapId, x, y)
    local best, bestSize
    for _, region in ipairs(catalog.regions) do
        if region.map == mapId and x >= region.left and x <= region.right and y >= region.top and y <= region.bottom then
            local size = (region.right - region.left) * (region.bottom - region.top)
            if not best or size < bestSize then best, bestSize = region, size end
        end
    end
    return best
end

return catalog
