metadata = {
    name = "Markers",
    description = "Draws HoYoLAB's markers on the in-game map and minimap. Pick the types and regions below.",
    author = "genshin-overlay",
    version = "0.3.0",
}

local SELECTION_FILE = "selection.json"
local DONE_FILE = "done.json"
-- what's on the first time: common chests
local DEFAULT_TYPES = { 17 }

-- HoYoLAB's groups, each with its types: { id, name, types = { { id, name, count } } }
local groups = {}
local typeNames = {}
local allTypes = {}
-- every map's regions with the box each one covers, and the maps they're on
local regions = {}
local maps = {}

-- what the user picked
local selected = {}  -- type id -> true
local hidden = {}    -- type id -> true, turned off from the bar on the map
-- while "hide all" is on: the ones that were hidden before it, to go back to
local hiddenBefore
local regionOn = {}  -- region id -> true, none means everywhere
local onlyHere = false
-- groups open in the menu: group id -> true
local openGroups = {}
-- the picked types in the menu's order, for the bar on the map
local barTypes = {}

local points = {}    -- type id -> map id -> its points there
local icons = {}     -- type id -> texture, 0 when it has none
-- HoYoLAB point id -> true, marked from a marker's guide
local done = {}
-- the marker whose guide is open: { id, type, map, x, y }, with every marker
-- on its spot in stack and which one it is in at. And which comment shows.
local openGuide
local commentAt = 1
-- where the guide was last frame, a tap there isn't for the markers under it
local guideRect
-- the region of the last spot the map or minimap showed, for the menu
local lastRegion

function RegisterSettings()
    DefineExtensionSetting("mapScale", "Marker size on the map (%)", "int", 100, 20, 200)
    DefineExtensionSetting("minimapScale", "Marker size on the minimap (%)", "int", 100, 20, 200)
    DefineExtensionSetting("hideDoneMinimap", "Hide collected ones on the minimap", "bool", true)
    DefineExtensionSetting("hideDoneMap", "Hide collected ones on the map", "bool", false)
    DefineExtensionSetting("mapBar", "Show the picked types as a bar on the map", "bool", true)
    DefineExtensionSetting("mergeClose", "Merge markers that overlap into one with a count", "bool", false)
    DefineExtensionSetting("tapGuides", "Tap a marker on the map to see its guide", "bool", true)
    DefineExtensionSetting("guideScale", "Guide size (%)", "int", 100, 60, 200)
end

-- Loading a texture takes a moment, so opening a big group only loads a few a frame
local ICONS_PER_FRAME = 8
local iconsThisFrame = 0

local function iconOf(id)
    local icon = icons[id]
    if icon then return icon end
    if iconsThisFrame >= ICONS_PER_FRAME then return 0 end
    iconsThisFrame = iconsThisFrame + 1
    icon = GetItemIcon(id) or 0
    icons[id] = icon
    return icon
end

-- the item files have a type's points on every map together, split by map here
local function loadPoints(id)
    local byMap = {}
    for _, point in ipairs(GetItemPositions(id) or {}) do
        local list = byMap[point.map_id]
        if not list then
            list = {}
            byMap[point.map_id] = list
        end
        -- points underground say which floor they're on
        local group = point.point_group
        list[#list + 1] = {
            id = point.id,
            x = point.x_pos,
            y = point.y_pos,
            area = point.area_id,
            group = group and group.group_id or 0,
            floor = group and group.floor_id or 0,
        }
    end
    points[id] = byMap
end

-- ids read back from a file, older saves have them as 17.0
local function ids(list)
    local whole = {}
    for _, id in ipairs(list or {}) do
        local value = math.tointeger(id)
        if value then whole[#whole + 1] = value end
    end
    return whole
end

local function keys(set)
    local list = {}
    for key in pairs(set) do list[#list + 1] = key end
    return list
end

local function setOf(list)
    local set = {}
    for _, id in ipairs(ids(list)) do set[id] = true end
    return set
end

-- Sync is kept per side, so more places can join later with the same rule.
-- base is what both had after the last sync, nil until the first one.
local SYNC_FILE = "sync.json"
local sync = { hoyolab = { on = false, last = 0 }, game = { on = true } }

local function saveSync()
    local h = sync.hoyolab
    WriteFile(SYNC_FILE, JSON_Encode({
        hoyolab = { on = h.on, base = h.base and keys(h.base), last = h.last },
        game = { on = sync.game.on },
    }))
end

local function loadSync()
    local saved = FileExists(SYNC_FILE) and JSON_Decode(ReadFile(SYNC_FILE))
    if saved and type(saved.game) == "table" then sync.game.on = saved.game.on ~= false end
    local h = saved and saved.hoyolab
    if type(h) ~= "table" then return end
    sync.hoyolab.on = h.on == true
    sync.hoyolab.base = h.on == true and h.base and setOf(h.base) or nil
    sync.hoyolab.last = tonumber(h.last) or 0
end

local function updateBar()
    barTypes = {}
    for _, group in ipairs(groups) do
        for _, kind in ipairs(group.types) do
            if selected[kind.id] then barTypes[#barTypes + 1] = kind.id end
        end
    end
end

local function saveSelection()
    WriteFile(SELECTION_FILE, JSON_Encode({
        types = keys(selected),
        hidden = keys(hidden),
        hideAll = hiddenBefore ~= nil,
        hiddenBefore = keys(hiddenBefore or {}),
        regions = keys(regionOn),
        onlyHere = onlyHere,
        openGroups = keys(openGroups),
    }))
end

-- turns a list of types on or off together, saved once at the end
local function setTypes(list, on)
    for _, id in ipairs(list) do
        selected[id] = on or nil
        hidden[id] = nil
        if on and not points[id] then loadPoints(id) end
    end
    updateBar()
    saveSelection()
end

-- The overlay downloads the item files on start, the first time they may not
-- be there yet when Init runs. Update tries again until they are.
local function loadItemFiles()
    groups, typeNames, allTypes, regions, maps, icons = {}, {}, {}, {}, {}, {}
    -- each map's file has the regions of all of them
    local seen = {}
    for _, area in ipairs(GetItemAreas(2) or {}) do
        regions[#regions + 1] = area
        if not seen[area.map_id] then
            seen[area.map_id] = true
            maps[#maps + 1] = area.map_id
        end
    end

    -- Each map has its own list of groups and types, mostly the same ones.
    -- They go into one list, a type's count is all maps together.
    local groupOf, typeOf = {}, {}
    for _, mapId in ipairs(maps) do
        for _, group in ipairs(GetItemTree(mapId) or {}) do
            for _, kind in ipairs(group.children or {}) do
                -- types with nothing on this map aren't worth a tile
                local count = kind.point_count or 0
                if count > 0 then
                    local merged = groupOf[group.id]
                    if not merged then
                        merged = { id = group.id, name = group.name, types = {} }
                        groupOf[group.id] = merged
                        groups[#groups + 1] = merged
                    end
                    local known = typeOf[kind.id]
                    if known then
                        known.count = known.count + count
                    else
                        known = { id = kind.id, name = kind.name, count = count }
                        typeOf[kind.id] = known
                        merged.types[#merged.types + 1] = known
                        typeNames[kind.id] = kind.name
                        allTypes[#allTypes + 1] = known
                    end
                end
            end
        end
    end
    for id in pairs(selected) do loadPoints(id) end
    updateBar()
end

function Init()
    local saved = FileExists(SELECTION_FILE) and JSON_Decode(ReadFile(SELECTION_FILE))
    for _, id in ipairs(saved and ids(saved.types) or DEFAULT_TYPES) do
        selected[id] = true
    end
    if saved then
        for _, id in ipairs(ids(saved.hidden)) do hidden[id] = true end
        if saved.hideAll then
            hiddenBefore = {}
            for _, id in ipairs(ids(saved.hiddenBefore)) do hiddenBefore[id] = true end
        end
        for _, id in ipairs(ids(saved.regions)) do regionOn[id] = true end
        onlyHere = saved.onlyHere == true
        for _, id in ipairs(ids(saved.openGroups)) do openGroups[id] = true end
    end
    loadItemFiles()
    if #groups == 0 then
        print("Markers: no item files yet, trying again while the overlay runs")
    end

    if FileExists(DONE_FILE) then
        for _, id in ipairs(ids(JSON_Decode(ReadFile(DONE_FILE)))) do done[id] = true end
    end
    loadSync()
end

-- the region a spot on a map is in. The boxes overlap at the edges, the smallest one wins.
local function regionAt(mapId, x, y)
    local best, bestSize
    for _, area in ipairs(regions) do
        if area.map_id == mapId and x >= area.l_x and x <= area.r_x and y >= area.l_y and y <= area.r_y then
            local size = (area.r_x - area.l_x) * (area.r_y - area.l_y)
            if not best or size < bestSize then best, bestSize = area, size end
        end
    end
    return best
end

-- Which regions to draw for a spot: a set of ids, or nil for everywhere.
-- "Where I am" outside of every region shows everything.
local function regionFilter(mapId, x, y)
    local here = regionAt(mapId, x, y)
    lastRegion = here
    if onlyHere then
        return here and { [here.id] = true } or nil
    end
    if next(regionOn) then return regionOn end
    return nil
end

-- markers at 100%: pixels across on the map, part of the minimap's radius
local MAP_MARKER = 50
local MINIMAP_MARKER = 0.08

-- how much of a done marker still shows
local DONE_OPACITY = 0.4

-- a color with its opacity (the last byte) times a
local function faded(color, a)
    return color - (color & 0xFF) + math.floor((color & 0xFF) * a)
end

-- A round badge like the game's map icons: light ring, dark inside, the
-- item in the middle. Done ones get a grey ring and the whole badge fades.
-- The one whose guide is open gets a blue ring.
local function drawMarker(x, y, half, icon, isDone, isOpen)
    local a = isDone and DONE_OPACITY or 1.0
    local ring = isOpen and 0x4FB8FFFF or isDone and 0x969696FF or 0xECE5D8FF
    if isOpen then a = 1.0 end
    DrawCircle(x, y + half * 0.08, half, faded(0x00000050, a))
    DrawCircle(x, y, half * 0.94, faded(ring, a))
    DrawCircle(x, y, half * 0.8, faded(0x3B4255FF, a))
    local item = half * 0.6
    DrawImage(icon, x - item, y - item, x + item, y + item, a)
end

-- underground only the floor you're on, above ground only what's above ground
local function onFloor(point, groupId, floorId)
    return point.group == groupId and (point.group == 0 or point.floor == floorId)
end

local function shown(id, mapId)
    return selected[id] and not hidden[id] and points[id] and points[id][mapId]
end

-- Hides every type on the bar, or brings back the ones that were showing
-- before it was tapped
local function toggleHideAll()
    if hiddenBefore then
        hidden, hiddenBefore = hiddenBefore, nil
    else
        hiddenBefore = {}
        for id in pairs(hidden) do hiddenBefore[id] = true end
        for _, id in ipairs(barTypes) do hidden[id] = true end
    end
    saveSelection()
end

-- The picked types as badges along the top of the map. Tapping one hides
-- its markers until it's tapped again. With more than one there's a
-- "hide all" badge first.
local BAR_BADGE = 40
local BAR_GAP = 6
local BAR_PAD = 7

local BAR_MARGIN = 20
-- whether the "+N" badge has the rest of the bar open below it
local barOpen = false
-- where the bar went with no panel open, it stays there while one is
local barGap

-- The widest stretch along the top that the game's UI leaves free, at the
-- bar's height. An open panel is one of the boxes, so it's left of that too.
local function topGap(view, y0, y1)
    local spans = { { view.left, view.right } }
    for _, box in ipairs(view.ui) do
        if box.top < y1 and box.bottom > y0 then
            local cut = {}
            for _, span in ipairs(spans) do
                if box.right <= span[1] or box.left >= span[2] then
                    cut[#cut + 1] = span
                else
                    if box.left > span[1] then cut[#cut + 1] = { span[1], box.left } end
                    if box.right < span[2] then cut[#cut + 1] = { box.right, span[2] } end
                end
            end
            spans = cut
        end
    end
    local best = { view.left, view.right }
    if #spans > 0 then
        best = spans[1]
        for _, span in ipairs(spans) do
            if span[2] - span[1] > best[2] - best[1] then best = span end
        end
    end
    return best[1] + BAR_MARGIN, best[2] - BAR_MARGIN
end

local function drawBar(view)
    if not GetSetting("mapBar") or #barTypes == 0 then return end
    local allButton = #barTypes > 1 and 1 or 0
    local height = BAR_BADGE + BAR_PAD * 2
    local step = BAR_BADGE + BAR_GAP
    local y0 = view.top + 18

    -- as many as fit in the gap, the rest open from a "+N" badge at the end
    local left, right = topGap(view, y0, y0 + height)
    local size = view.right - view.left
    if not view.panelLeft then
        barGap = { left = left, right = right, size = size }
    elseif barGap and barGap.size == size then
        left, right = barGap.left, barGap.right
    end
    local slots = math.max(2, math.floor((right - left - BAR_PAD * 2 + BAR_GAP) / step))
    local count, more = #barTypes, 0
    if count + allButton > slots then
        count = math.max(1, slots - allButton - 1)
        more = #barTypes - count
    end
    if more == 0 then barOpen = false end
    local used = count + allButton + (more > 0 and 1 or 0)

    local width = used * BAR_BADGE + (used - 1) * BAR_GAP + BAR_PAD * 2
    local x0 = math.max(view.left, (left + right) / 2 - width / 2)
    local bottom = y0 + height
    -- the whole bar takes the mouse, a tap between two badges shouldn't move the map
    ClickArea(x0, y0, x0 + width, bottom)

    DrawRect(x0, y0, x0 + width, bottom, 0x1C2030D0, height / 2)
    DrawRect(x0, y0, x0 + width, bottom, 0xD3BC8E60, height / 2, 1.5)

    local label, labelX
    local y = y0 + height / 2
    local half = BAR_BADGE / 2

    local function typeBadge(id, x, y)
        local clicked, hovered = ClickArea(x - half, y - half, x + half, y + half)
        if clicked then
            hidden[id] = not hidden[id] or nil
            -- picking by hand after "hide all" leaves it
            hiddenBefore = nil
            saveSelection()
        end
        -- hidden ones look like done markers, a bit bigger under the mouse
        drawMarker(x, y, hovered and half * 1.08 or half, iconOf(id), hidden[id])
        if hovered then
            label, labelX = typeNames[id] .. (hidden[id] and "  (hidden)" or ""), x - half
        end
    end

    local function textBadge(x, text, dim)
        local clicked, hovered = ClickArea(x - half, y - half, x + half, y + half)
        drawMarker(x, y, hovered and half * 1.08 or half, 0, dim)
        local w, h = TextSize(text, 15)
        DrawText(text, x - w / 2, y - h / 2, 15, dim and faded(0xECE5D8FF, DONE_OPACITY) or 0xECE5D8FF)
        return clicked, hovered
    end

    if allButton == 1 then
        local x = x0 + BAR_PAD + half
        local clicked, hovered = textBadge(x, "All", hiddenBefore ~= nil)
        if clicked then toggleHideAll() end
        if hovered then
            label, labelX = hiddenBefore and "Show the ones from before" or "Hide all", x - half
        end
    end

    for i = 1, count do
        typeBadge(barTypes[i], x0 + BAR_PAD + (i - 1 + allButton) * step + half, y)
    end

    if more > 0 then
        local x = x0 + BAR_PAD + (used - 1) * step + half
        local clicked, hovered = textBadge(x, barOpen and "-" or "+" .. more, false)
        if clicked then barOpen = not barOpen end
        if hovered then label, labelX = barOpen and "Show fewer" or more .. " more", x - half end

        -- the rest in rows under the bar, as wide as it
        if barOpen then
            local perRow = used
            local rows = math.ceil(more / perRow)
            local top = bottom + 6
            bottom = top + rows * step - BAR_GAP + BAR_PAD * 2
            ClickArea(x0, top, x0 + width, bottom)
            DrawRect(x0, top, x0 + width, bottom, 0x1C2030D0, half + BAR_PAD)
            DrawRect(x0, top, x0 + width, bottom, 0xD3BC8E60, half + BAR_PAD, 1.5)
            for i = 1, more do
                local column, row = (i - 1) % perRow, math.floor((i - 1) / perRow)
                typeBadge(barTypes[count + i], x0 + BAR_PAD + column * step + half, top + BAR_PAD + row * step + half)
            end
        end
    end

    -- the name under the badge the mouse is on, below everything that's open
    if label then
        local w, h = TextSize(label, 16)
        local ty = bottom + 8
        DrawRect(labelX - 8, ty - 5, labelX + w + 8, ty + h + 5, 0x1C2030E0, 6)
        DrawText(label, labelX, ty, 16, 0xECE5D8FF)
    end
end

local function writeDone()
    WriteFile(DONE_FILE, JSON_Encode(keys(done)))
end

-- when the last change by hand was, sync sends them a few seconds later together
local changedAt

local function saveDone()
    writeDone()
    changedAt = GetOSTimeMS()
end

local LIGHT = 0xECE5D8FF
local DIM = 0xA8A091FF

-- a round button with a label, gives back whether it was tapped
local function button(x0, y0, x1, y1, label, size)
    local clicked, hovered = ClickArea(x0, y0, x1, y1)
    DrawRect(x0, y0, x1, y1, hovered and 0x4A5268FF or 0x2E3446FF, (y1 - y0) / 2)
    local w, h = TextSize(label, size)
    DrawText(label, (x0 + x1 - w) / 2, (y0 + y1 - h) / 2, size, LIGHT)
    return clicked
end

-- a picture fit into width and at most maxHeight tall, nothing until it's loaded
local function pictureSize(url, width, maxHeight)
    if url == "" or maxHeight <= 0 then return nil end
    local texture, w, h = GetWebImage(url)
    if texture == 0 or w == 0 then return nil end
    local scale = math.min(width / w, maxHeight / h)
    return texture, w * scale, h * scale
end

-- Text cut into lines that fit width. ImGui only breaks at spaces, so long
-- words and links went past the edge, here they get cut too. Kept per text.
local wrapped, wrappedCount = {}, 0

local function wrapLines(text, size, width)
    local key = size .. ":" .. width .. ":" .. text
    local lines = wrapped[key]
    if lines then return lines end

    lines = {}
    for paragraph in (text:gsub("%s+$", "") .. "\n"):gmatch("(.-)\r?\n") do
        local line = ""
        for word in paragraph:gmatch("%S+") do
            local joined = line == "" and word or line .. " " .. word
            if TextSize(joined, size) <= width then
                line = joined
            elseif TextSize(word, size) <= width then
                lines[#lines + 1] = line
                line = word
            else
                -- longer than a whole line, cut where it stops fitting
                if line ~= "" then line = line .. " " end
                for char in word:gmatch("[%z\1-\127\194-\244][\128-\191]*") do
                    if line ~= "" and TextSize(line .. char, size) > width then
                        lines[#lines + 1] = line
                        line = ""
                    end
                    line = line .. char
                end
            end
        end
        lines[#lines + 1] = line
    end

    if wrappedCount > 200 then wrapped, wrappedCount = {}, 0 end
    wrapped[key] = lines
    wrappedCount = wrappedCount + 1
    return lines
end

-- draws text cut into lines, gives back how tall it is
local function textBlock(text, x, y, size, color, width, draw)
    local lines = wrapLines(text, size, width)
    local _, lineHeight = TextSize("Ag", size)
    if draw then
        for i, line in ipairs(lines) do
            DrawText(line, x, y + (i - 1) * lineHeight, size, color)
        end
    end
    return #lines * lineHeight
end

-- the guide's sizes are for 100%, k makes them fit the size picked
local k = 1
local function px(n) return n * k end

-- shows the at'th marker of the open guide's stack
local function showInGuide(at)
    local count = #openGuide.stack
    at = (at - 1) % count + 1
    local marker = openGuide.stack[at]
    openGuide.at = at
    openGuide.id, openGuide.type, openGuide.map, openGuide.x, openGuide.y = marker.id, marker.type, marker.map, marker.x, marker.y
    commentAt = 1
end

-- The guide from top to bottom, gives back how tall it is. With draw off it
-- only measures, so the pictures can be made smaller when it doesn't fit.
local function layoutGuide(guide, x0, y0, width, maxPicture, draw)
    local text, small, big = px(16), px(14), px(20)
    local inner = width - px(32)
    local x, y = x0 + px(16), y0 + px(16)

    -- the name, with room on the right for the close button
    local name = typeNames[openGuide.type] or "Marker"
    local nameHeight = textBlock(name, x, y, big, LIGHT, inner - px(40), draw)
    if draw then
        if button(x + inner - px(30), y - px(2), x + inner, y + px(28), "X", text) then
            openGuide = nil
            return 0
        end
    end
    y = y + math.max(nameHeight, px(28)) + px(10)

    -- more than one marker on this spot, step through them
    local stackCount = #openGuide.stack
    if stackCount > 1 then
        if draw then
            textBlock(openGuide.at .. " of " .. stackCount .. " here", x, y + px(4), text, DIM, inner - px(80), true)
            if button(x + inner - px(70), y, x + inner - px(38), y + px(26), "<", text) then
                showInGuide(openGuide.at - 1)
            end
            if button(x + inner - px(32), y, x + inner, y + px(26), ">", text) then
                showInGuide(openGuide.at + 1)
            end
        end
        y = y + px(36)
    end

    local isDone = done[openGuide.id] == true
    if draw and button(x, y, x + inner, y + px(32), isDone and "Collected, tap to undo" or "Mark as collected", text) then
        done[openGuide.id] = not isDone or nil
        saveDone()
    end
    y = y + px(46)

    if not guide or guide.failed then
        local message = guide and "Couldn't load the guide, trying again in a bit." or "Loading the guide..."
        return y + textBlock(message, x, y, text, DIM, inner, draw) + px(16) - y0
    end

    local texture, w, h = pictureSize(guide.picture, inner, maxPicture)
    if texture then
        if draw then DrawImage(texture, x + (inner - w) / 2, y, x + (inner + w) / 2, y + h) end
        y = y + h + px(10)
    end
    if guide.text ~= "" then
        y = y + textBlock(guide.text, x, y, text, LIGHT, inner, draw) + px(12)
    end

    local count = #guide.comments
    if count == 0 then
        if guide.text == "" and guide.picture == "" then
            y = y + textBlock("No guide for this one yet.", x, y, text, DIM, inner, draw) + px(6)
        end
        return y + px(16) - y0
    end

    -- appsample's comments one at a time, most liked first. The wheel doesn't
    -- reach the overlay, so it's buttons instead of scrolling.
    commentAt = math.max(1, math.min(commentAt, count))
    if draw then
        DrawRect(x, y, x + inner, y + 1, 0xD3BC8E60)
        textBlock("From appsample  " .. commentAt .. " / " .. count, x, y + px(12), text, DIM, inner - px(80), true)
        if count > 1 then
            if button(x + inner - px(70), y + px(8), x + inner - px(38), y + px(34), "<", text) then
                commentAt = commentAt == 1 and count or commentAt - 1
            end
            if button(x + inner - px(32), y + px(8), x + inner, y + px(34), ">", text) then
                commentAt = commentAt == count and 1 or commentAt + 1
            end
        end
    end
    y = y + px(44)

    local comment = guide.comments[commentAt]
    texture, w, h = pictureSize(comment.picture, inner, maxPicture)
    if texture then
        if draw then DrawImage(texture, x + (inner - w) / 2, y, x + (inner + w) / 2, y + h) end
        y = y + h + px(8)
    end
    if comment.text ~= "" then
        y = y + textBlock(comment.text, x, y, text, LIGHT, inner, draw) + px(6)
    end
    y = y + textBlock(comment.author .. "  " .. comment.date .. "  +" .. comment.likes, x, y, small, DIM, inner, draw)
    return y + px(16) - y0
end

local GUIDE_GAP = 10

-- The open guide under its marker, or above it when there's more room there.
-- Made to fit: the pictures get smaller first, past that the bottom is cut off.
local function drawGuide(view, half)
    guideRect = nil
    if not openGuide then return end
    local guide = GetPointGuide(openGuide.map, openGuide.id, openGuide.type)
    local markerX = view.centerX + (openGuide.x - view.lng) / view.unitsPerPixel
    local markerY = view.centerY + (openGuide.y - view.lat) / view.unitsPerPixel

    k = GetSetting("guideScale") / 100
    local right = view.panelLeft or view.right
    local width = math.min(px(400), right - view.left - GUIDE_GAP * 2)
    local x0 = math.max(view.left + GUIDE_GAP, math.min(markerX - width / 2, right - width - GUIDE_GAP))
    local below = view.bottom - GUIDE_GAP - (markerY + half + GUIDE_GAP)
    local above = (markerY - half - GUIDE_GAP) - (view.top + GUIDE_GAP)
    local room = math.max(below, above)

    local maxPicture = px(240)
    while maxPicture > px(120) and layoutGuide(guide, x0, 0, width, maxPicture, false) > room do
        maxPicture = maxPicture - px(20)
    end
    local height = math.min(layoutGuide(guide, x0, 0, width, maxPicture, false), room)
    local y0 = below >= height and markerY + half + GUIDE_GAP or markerY - half - GUIDE_GAP - height
    y0 = math.max(view.top + GUIDE_GAP, math.min(y0, view.bottom - GUIDE_GAP - height))
    local x1, y1 = x0 + width, y0 + height

    ClickArea(x0, y0, x1, y1)
    guideRect = { x0, y0, x1, y1 }
    DrawRect(x0, y0, x1, y1, 0x1C2030F0, px(12))
    DrawRect(x0, y0, x1, y1, 0xD3BC8E60, px(12), 1.5)
    PushClipRect(x0, y0, x1, y1)
    layoutGuide(guide, x0, y0, width, maxPicture, true)
    PopClipRect()
end

-- Tapping a marker opens its guide, the same one again closes it. stack is
-- every marker on that spot, the guide steps through them.
local function tapMarker(stack)
    if openGuide and openGuide.stack[1].id == stack[1].id then
        openGuide = nil
        return
    end
    openGuide = { stack = stack }
    showInGuide(1)
end

-- the markers on screen this frame, kept between frames so nothing is made each frame
local onScreen = { n = 0, x = {}, y = {}, point = {}, type = {} }

-- Markers closer than this many badge widths count as one spot: merged into one
-- badge, or all in the guide when one of them is tapped.
local SAME_SPOT = 1.0

-- The markers that overlap, each group as a list of indexes into onScreen.
-- Each one joins the first group it's close to, groups are looked up by grid cell.
local function mergeOnScreen(reach)
    local groups, cells = {}, {}
    for i = 1, onScreen.n do
        local x, y = onScreen.x[i], onScreen.y[i]
        local cx, cy = math.floor(x / reach), math.floor(y / reach)
        local joined
        for gx = cx - 1, cx + 1 do
            for gy = cy - 1, cy + 1 do
                for _, group in ipairs(cells[gx * 100000 + gy] or {}) do
                    local first = group[1]
                    local dx, dy = onScreen.x[first] - x, onScreen.y[first] - y
                    if not joined and dx * dx + dy * dy < reach * reach then joined = group end
                end
            end
        end
        if joined then
            joined[#joined + 1] = i
        else
            local group = { i }
            groups[#groups + 1] = group
            local key = cx * 100000 + cy
            cells[key] = cells[key] or {}
            table.insert(cells[key], group)
        end
    end
    return groups
end

local function stackOf(indexes, mapId)
    local stack = {}
    for _, i in ipairs(indexes) do
        local point = onScreen.point[i]
        stack[#stack + 1] = { id = point.id, type = onScreen.type[i], map = mapId, x = point.x, y = point.y }
    end
    return stack
end

-- how many are on a merged badge, in a bubble at its top right
local function drawCount(x, y, half, count, allDone)
    local r = half * 0.42
    local bx, by = x + half * 0.72, y - half * 0.72
    DrawCircle(bx, by, r, faded(0x3B4255FF, allDone and DONE_OPACITY or 1))
    DrawCircle(bx, by, r * 0.86, faded(0xD3BC8EFF, allDone and DONE_OPACITY or 1))
    local label = tostring(count)
    local size = r * 1.3
    local w, h = TextSize(label, size)
    DrawText(label, bx - w / 2, by - h / 2, size, 0x1C2030FF)
end

-- the game's own UI showing now, a marker under it isn't drawn
local function underUi(view, x, y, half)
    for _, box in ipairs(view.ui) do
        if x + half > box.left and x - half < box.right and y + half > box.top and y - half < box.bottom then return true end
    end
    return false
end

local function drawOnMap(view)
    local half = MAP_MARKER / 2 * GetSetting("mapScale") / 100
    local filter = regionFilter(view.mapId, view.lng, view.lat)
    -- Only the middle of a badge takes taps, so the map can still be dragged
    -- from right next to one. Only the one under the mouse is claimed.
    local tapHalf = GetSetting("tapGuides") and half * 0.6
    local mouseX, mouseY = GetMousePosition()
    if guideRect and mouseX >= guideRect[1] and mouseX <= guideRect[3] and mouseY >= guideRect[2] and mouseY <= guideRect[4] then
        tapHalf = nil
    end

    -- the one whose guide is open stays, so it can still be unmarked
    local hideDone = GetSetting("hideDoneMap")
    local n = 0
    for _, id in ipairs(barTypes) do
        local list = shown(id, view.mapId)
        if list then
            for _, point in ipairs(list) do
                if onFloor(point, view.groupId, view.floorId) and (not filter or filter[point.area])
                    and not (hideDone and done[point.id] and not (openGuide and openGuide.id == point.id)) then
                    -- x_pos / y_pos are the same map units as the view's lng / lat
                    local x = view.centerX + (point.x - view.lng) / view.unitsPerPixel
                    local y = view.centerY + (point.y - view.lat) / view.unitsPerPixel
                    if x + half > view.left and x - half < view.right and y + half > view.top and y - half < view.bottom
                        and not underUi(view, x, y, half) then
                        n = n + 1
                        onScreen.x[n], onScreen.y[n], onScreen.point[n], onScreen.type[n] = x, y, point, id
                    end
                end
            end
        end
    end
    onScreen.n = n

    local reach = half * 2 * SAME_SPOT
    PushClipRect(view.left, view.top, view.right, view.bottom)
    if GetSetting("mergeClose") then
        for _, group in ipairs(mergeOnScreen(reach)) do
            local first = group[1]
            local x, y = onScreen.x[first], onScreen.y[first]
            local allDone, isOpen = true, false
            for _, i in ipairs(group) do
                local id = onScreen.point[i].id
                if not done[id] then allDone = false end
                if openGuide and openGuide.id == id then isOpen = true end
            end
            drawMarker(x, y, half, iconOf(onScreen.type[first]), allDone, isOpen)
            if #group > 1 then drawCount(x, y, half, #group, allDone) end
            if tapHalf and math.abs(mouseX - x) <= tapHalf and math.abs(mouseY - y) <= tapHalf
                and ClickArea(x - tapHalf, y - tapHalf, x + tapHalf, y + tapHalf) then
                tapMarker(stackOf(group, view.mapId))
            end
        end
    else
        -- each on its own spot, a tap takes every one that overlaps the tapped one
        local tapped, open
        for i = 1, n do
            local x, y = onScreen.x[i], onScreen.y[i]
            local point = onScreen.point[i]
            if openGuide and openGuide.id == point.id then open = i end
            drawMarker(x, y, half, iconOf(onScreen.type[i]), done[point.id])
            if tapHalf and math.abs(mouseX - x) <= tapHalf and math.abs(mouseY - y) <= tapHalf
                and ClickArea(x - tapHalf, y - tapHalf, x + tapHalf, y + tapHalf) then
                tapped = i
            end
        end
        -- the open one again on top, so its blue ring isn't under the others
        if open then
            drawMarker(onScreen.x[open], onScreen.y[open], half, iconOf(onScreen.type[open]), done[onScreen.point[open].id], true)
        end
        if tapped then
            local group = { tapped }
            for i = 1, n do
                local dx, dy = onScreen.x[i] - onScreen.x[tapped], onScreen.y[i] - onScreen.y[tapped]
                if i ~= tapped and dx * dx + dy * dy < reach * reach then group[#group + 1] = i end
            end
            tapMarker(stackOf(group, view.mapId))
        end
    end
    PopClipRect()

    -- after the markers, so they sit on top of them
    drawBar(view)
    drawGuide(view, half)
end

-- The minimap only shows, it can't be tapped: in game the cursor is locked
-- and a claimed area would eat clicks meant for the game.
local function drawOnMinimap(player)
    -- the minimap changes size with the screen, so the icons are a part of it
    local half = player.radius * MINIMAP_MARKER * GetSetting("minimapScale") / 100
    -- the minimap is a circle, only what's fully inside it is drawn
    local inside = player.radius - half
    local hideDone = GetSetting("hideDoneMinimap")
    local filter = regionFilter(player.mapId, player.lng, player.lat)
    for _, id in ipairs(barTypes) do
        local list = shown(id, player.mapId)
        if list then
            local icon = iconOf(id)
            for _, point in ipairs(list) do
                if onFloor(point, player.groupId, player.floorId) and not (hideDone and done[point.id])
                    and (not filter or filter[point.area]) then
                    local dx = (point.x - player.lng) / player.unitsPerPixel
                    local dy = (point.y - player.lat) / player.unitsPerPixel
                    if dx * dx + dy * dy <= inside * inside then
                        drawMarker(player.centerX + dx, player.centerY + dy, half, icon, done[point.id])
                    end
                end
            end
        end
    end
end

-- Sync with files and HoYoLAB. The login and anything that unmarks on
-- HoYoLAB stays in the overlay: it asks first and backs the marks up.

local HOYOLAB = "HoYoLAB account"
-- taps are sent together once nothing changed for this long
local SEND_AFTER = 3000

-- the HoYoLAB job running and the file dialog open, with what to do after
local hoyoJob, fileJob
-- what the sync page asks before changing anything: { lines, actions = { { label, fn } } }
local question
-- the last thing that happened: { text, error, captcha }
local status
local syncedThisRun = false
local lastAccountCheck = 0
local backupList
-- the account's Genshin characters, one per server: { account, list, error }
local characters, charactersJob

-- what's in a and not in b, as a list
local function minus(a, b)
    local list = {}
    for id in pairs(a) do
        if not b[id] then list[#list + 1] = id end
    end
    return list
end

local function copy(set)
    local out = {}
    for id in pairs(set) do out[id] = true end
    return out
end

local function plural(n, one, many)
    return n .. " " .. (n == 1 and one or many)
end

local function startHoyo(id, after)
    hoyoJob = { id = id, after = after }
end

-- what to say about the points HoYoLAB couldn't take and the unmarks said no to
local function notes(r)
    local text = ""
    if #r.kept > 0 then text = text .. ", kept " .. #r.kept .. " marked" end
    if #r.unknown > 0 then text = text .. ", skipped " .. #r.unknown .. " not in the item files" end
    return text
end

-- Sets collected to what sync decided and makes HoYoLAB the same. base
-- ends up as what both have, unmarks said no to stay collected on both.
local function settle(final, remote)
    local h = sync.hoyolab
    done = copy(final)
    writeDone()
    local add, remove = minus(final, remote), minus(remote, final)
    local function finish(r)
        local base = copy(final)
        for _, id in ipairs(r and r.kept or {}) do
            base[id], done[id] = true, true
        end
        -- not sent, so HoYoLAB still has what it had
        for _, id in ipairs(r and r.unknown or {}) do base[id] = remote[id] end
        if r and #r.kept > 0 then writeDone() end
        h.base, h.last = base, GetOSTimeMS()
        saveSync()
        local sent = r and (", marked " .. r.added .. " and unmarked " .. r.removed .. " on HoYoLAB" .. notes(r)) or ""
        status = { text = "In sync, " .. #keys(done) .. " collected" .. sent }
    end
    if #add == 0 and #remove == 0 then
        finish(nil)
    else
        startHoyo(HoYoLAB_ChangeMarks(add, remove), finish)
    end
end

-- Per point: when both sides agree that's it, otherwise the side that
-- changed since the last sync wins. The first sync only adds, both ways.
local function fullSync()
    local h = sync.hoyolab
    syncedThisRun = true
    status = { text = "Syncing with HoYoLAB..." }
    startHoyo(HoYoLAB_FetchMarks(), function(r)
        local remote = setOf(r.marks)
        if not h.base then
            local toSend, toGet = minus(done, remote), minus(remote, done)
            status = nil
            question = {
                lines = {
                    "First sync, nothing gets unmarked anywhere.",
                    plural(#toSend, "collected one here goes", "collected ones here go") .. " to HoYoLAB.",
                    plural(#toGet, "mark on HoYoLAB comes", "marks on HoYoLAB come") .. " here.",
                },
                actions = {
                    { "Start syncing", function()
                        local union = copy(done)
                        for id in pairs(remote) do union[id] = true end
                        settle(union, remote)
                    end },
                    { "Cancel", function()
                        h.on = false
                        saveSync()
                    end },
                },
            }
            return
        end
        local final = {}
        local function decide(id)
            local here, there, before = done[id] == true, remote[id] == true, h.base[id] == true
            if here == there then
                final[id] = here or nil
            elseif here ~= before then
                final[id] = here or nil
            else
                final[id] = there or nil
            end
        end
        for id in pairs(done) do decide(id) end
        for id in pairs(remote) do decide(id) end
        for id in pairs(h.base) do decide(id) end
        settle(final, remote)
    end)
end

-- sends what changed here since the last sync
local function sendChanges()
    local h = sync.hoyolab
    changedAt = nil
    local add, remove = minus(done, h.base), minus(h.base, done)
    if #add == 0 and #remove == 0 then return end
    startHoyo(HoYoLAB_ChangeMarks(add, remove), function(r)
        local skipped = setOf(r.unknown)
        for _, id in ipairs(add) do
            if not skipped[id] then h.base[id] = true end
        end
        for _, id in ipairs(remove) do h.base[id] = nil end
        for _, id in ipairs(r.kept) do
            h.base[id], done[id] = true, true
        end
        if #r.kept > 0 then writeDone() end
        h.last = GetOSTimeMS()
        saveSync()
        status = { text = "Sent to HoYoLAB: marked " .. r.added .. ", unmarked " .. r.removed .. notes(r) }
    end)
end

-- Get and Send back up both sides before showing anything: the overlay
-- saves HoYoLAB's marks, this saves collected under the same name.
local function backUpCollected(r)
    local file = "collected_" .. r.backup .. ".json"
    local ok, wrote = pcall(WriteFile, file, JSON_Encode(keys(done)))
    if not ok or not wrote then
        status = { text = "Couldn't back up collected to " .. file .. ", nothing was changed.", error = true }
        return nil
    end
    return "Backed up both: hoyolab-backups\\" .. r.backup .. ".json and " .. file .. " in the Markers folder."
end

-- one way, with sync off: HoYoLAB's marks into collected
local function getFromHoyolab()
    status = { text = "Getting your HoYoLAB marks..." }
    startHoyo(HoYoLAB_FetchMarks(true), function(r)
        local saved = backUpCollected(r)
        if not saved then return end
        local remote = setOf(r.marks)
        local new, extra = minus(remote, done), minus(done, remote)
        status = { text = saved }
        local actions = {}
        if #new > 0 then
            actions[#actions + 1] = { "Add the " .. #new, function()
                for _, id in ipairs(new) do done[id] = true end
                saveDone()
                status = { text = "Added " .. #new .. " from HoYoLAB" }
            end }
        end
        if #extra > 0 then
            actions[#actions + 1] = { "Make it exactly HoYoLAB's", function()
                for _, id in ipairs(new) do done[id] = true end
                for _, id in ipairs(extra) do done[id] = nil end
                saveDone()
                status = { text = "Collected is now the same as HoYoLAB" }
            end }
        end
        actions[#actions + 1] = { "Cancel", function() end }
        question = {
            lines = {
                "HoYoLAB has " .. plural(#r.marks, "mark", "marks") .. ".",
                plural(#new, "isn't", "aren't") .. " collected here.",
                plural(#extra, "collected one here isn't", "collected ones here aren't") .. " marked there.",
            },
            actions = actions,
        }
    end)
end

-- one way, with sync off: collected onto HoYoLAB. Unmarking there asks again first.
local function sendToHoyolab()
    status = { text = "Getting your HoYoLAB marks to compare..." }
    startHoyo(HoYoLAB_FetchMarks(true), function(r)
        local saved = backUpCollected(r)
        if not saved then return end
        local remote = setOf(r.marks)
        local new, extra = minus(done, remote), minus(remote, done)
        status = { text = saved }
        local function sent(result)
            status = { text = "Marked " .. result.added .. " and unmarked " .. result.removed .. " on HoYoLAB" .. notes(result) }
        end
        local actions = {}
        if #new > 0 then
            actions[#actions + 1] = { "Mark the " .. #new .. " on HoYoLAB", function()
                startHoyo(HoYoLAB_ChangeMarks(new, {}), sent)
            end }
        end
        if #extra > 0 then
            actions[#actions + 1] = { "Make HoYoLAB exactly this", function()
                startHoyo(HoYoLAB_ChangeMarks(new, extra), sent)
            end }
        end
        actions[#actions + 1] = { "Cancel", function() end }
        question = {
            lines = {
                plural(#new, "collected one here isn't", "collected ones here aren't") .. " marked on HoYoLAB.",
                plural(#extra, "mark on HoYoLAB isn't", "marks on HoYoLAB aren't") .. " collected here.",
            },
            actions = actions,
        }
    end)
end

local function exportFile()
    local id = SaveFileDialog("markers-collected.json", JSON_Encode(keys(done)))
    if not id then return end
    fileJob = { id = id, after = function(r)
        status = r.ok and { text = "Saved " .. plural(#keys(done), "collected one", "collected ones") .. " to " .. r.name }
            or { text = "Couldn't save " .. r.name, error = true }
    end }
end

-- a list of ids like the export, or one of the overlay's HoYoLAB backups
local function idsInFile(data)
    local decoded = JSON_Decode(data)
    if type(decoded) ~= "table" then return nil end
    if type(decoded.marks) == "table" then
        local set = {}
        for _, list in pairs(decoded.marks) do
            for _, id in ipairs(ids(list)) do set[id] = true end
        end
        return set
    end
    return setOf(decoded)
end

local function importFile()
    local id = OpenFileDialog()
    if not id then return end
    fileJob = { id = id, after = function(r)
        local file = r.ok and idsInFile(r.data)
        if not file then
            status = { text = "Couldn't read " .. r.name .. " as a list of collected ones", error = true }
            return
        end
        local new, extra = minus(file, done), minus(done, file)
        local actions = {}
        if #new > 0 then
            actions[#actions + 1] = { "Add the " .. #new, function()
                for _, point in ipairs(new) do done[point] = true end
                saveDone()
                status = { text = "Added " .. #new .. " from " .. r.name }
            end }
        end
        if #extra > 0 then
            actions[#actions + 1] = { "Make it exactly the file", function()
                for _, point in ipairs(new) do done[point] = true end
                for _, point in ipairs(extra) do done[point] = nil end
                saveDone()
                status = { text = "Collected is now the same as " .. r.name }
            end }
        end
        actions[#actions + 1] = { "Cancel", function() end }
        local lines = {
            r.name .. " has " .. plural(#keys(file), "collected one", "collected ones") .. ".",
            plural(#new, "isn't", "aren't") .. " collected here.",
            plural(#extra, "collected one here isn't", "collected ones here aren't") .. " in the file.",
        }
        if sync.hoyolab.on then lines[#lines + 1] = "Sync is on, so this goes to HoYoLAB too." end
        question = { lines = lines, actions = actions }
    end }
end

local function putBack(name)
    status = { text = "Putting back " .. name .. "..." }
    startHoyo(HoYoLAB_RestoreBackup(name), function(r)
        status = { text = "Put back " .. plural(r.added, "mark", "marks") .. " on HoYoLAB" .. notes(r) }
        -- sync brings them here too, they're new on HoYoLAB's side
        if sync.hoyolab.on and sync.hoyolab.base then fullSync() end
    end)
end

-- picks up finished jobs, syncs on start and sends taps
local function syncTick()
    if fileJob then
        local r = FileDialogResult(fileJob.id)
        if r then
            local job = fileJob
            fileJob = nil
            if r.picked then job.after(r) end
        end
    end

    if not HasPermission(HOYOLAB) then return end
    if charactersJob then
        local r = HoYoLAB_Result(charactersJob.id)
        if r then
            characters = { account = charactersJob.account, list = r.characters, error = not r.ok and r.error or nil }
            charactersJob = nil
        end
    end
    if hoyoJob then
        local r = HoYoLAB_Result(hoyoJob.id)
        if not r then return end
        local job = hoyoJob
        hoyoJob, backupList = nil, nil
        if r.ok then
            job.after(r)
        else
            status = { text = "HoYoLAB: " .. r.error, error = true, captcha = r.captcha }
        end
        return
    end

    local h = sync.hoyolab
    if not h.on then return end
    local now = GetOSTimeMS()
    -- logged in or out happens in another window, no need to ask every frame
    if now - lastAccountCheck < 1000 then return end
    lastAccountCheck = now
    if not HoYoLAB_Account().loggedIn then
        syncedThisRun = false
        return
    end
    if not syncedThisRun then
        fullSync()
    elseif h.base and changedAt and now - changedAt >= SEND_AFTER and not question then
        sendChanges()
    end
end

-- how often to look for the item files while they're missing
local RETRY_FRAMES = 120
local framesWithout = 0

function Update()
    iconsThisFrame = 0

    if #groups == 0 then
        framesWithout = framesWithout + 1
        if framesWithout >= RETRY_FRAMES then
            framesWithout = 0
            loadItemFiles()
        end
    end

    -- chests and oculi the game says were opened or picked up, when the player reads the game's traffic.
    -- read even when off, so turning it on doesn't mark the old ones
    local opened = false
    for _, id in ipairs(GetOpenedPoints()) do
        if sync.game.on and not done[id] then
            done[id] = true
            opened = true
        end
    end
    if opened then saveDone() end

    syncTick()

    -- without this there's no view while the map moves
    FollowMapWhileMoving()

    local view = GetMapView()
    if view then
        -- a guide from another map closes when the map switches
        if openGuide and openGuide.map ~= view.mapId then openGuide, guideRect = nil, nil end
        drawOnMap(view)
        return
    end
    -- the guide closes with the map
    openGuide, guideRect = nil, nil

    local player = GetPlayerPosition()
    if player.found then drawOnMinimap(player) end
end

-- the menu page

local function regionChips()
    ImGUI_TextDisabled("Regions")
    local everywhere = not onlyHere and next(regionOn) == nil
    if ImGUI_Chip("Everywhere", everywhere) ~= everywhere and not everywhere then
        regionOn, onlyHere = {}, false
        saveSelection()
    end
    local hereLabel = lastRegion and ("Where I am: " .. lastRegion.name) or "Where I am"
    if ImGUI_Chip(hereLabel, onlyHere) ~= onlyHere then
        onlyHere = not onlyHere
        regionOn = {}
        saveSelection()
    end
    for _, area in ipairs(regions) do
        local on = regionOn[area.id] == true
        if ImGUI_Chip(area.name, on) ~= on then
            regionOn[area.id] = not on or nil
            onlyHere = false
            saveSelection()
        end
    end
end

-- An "All" chip, lit while every type in the list is on. Tapping it turns
-- them all on, or all off when they already are.
local function allChip(types, id)
    local ids, all = {}, true
    for _, kind in ipairs(types) do
        ids[#ids + 1] = kind.id
        if not selected[kind.id] then all = false end
    end
    if ImGUI_Chip("All " .. #types .. "##" .. id, all) ~= all then
        setTypes(ids, not all)
    end
end

local function typeTiles(types, id)
    allChip(types, id)
    for _, kind in ipairs(types) do
        local on = selected[kind.id] == true
        if ImGUI_IconToggle(tostring(kind.id), iconOf(kind.id), kind.name, kind.count, on) ~= on then
            setTypes({ kind.id }, not on)
        end
    end
end

-- the sync page

local backupsOpen = false

local function count(set)
    local n = 0
    for _ in pairs(set) do n = n + 1 end
    return n
end

local function ago(ms)
    local seconds = (GetOSTimeMS() - ms) / 1000
    if seconds < 60 then return "just now" end
    if seconds < 3600 then return math.floor(seconds / 60) .. " min ago" end
    if seconds < 86400 then return math.floor(seconds / 3600) .. " h ago" end
    return math.floor(seconds / 86400) .. " days ago"
end

local function backups(busy)
    if not backupList then backupList = HoYoLAB_Backups() end
    backupsOpen = ImGUI_CollapsingHeader("Backups of your HoYoLAB marks (" .. #backupList .. ")###hoyolabBackups", backupsOpen)
    if not backupsOpen then return end
    ImGUI_TextDisabled("Taken before the overlay changes your marks there.")
    ImGUI_TextDisabled("Putting one back only adds marks, it never unmarks.")
    if #backupList == 0 then ImGUI_TextDisabled("None yet.") end
    for _, backup in ipairs(backupList) do
        ImGUI_Text(backup.name .. "   " .. plural(backup.count, "mark", "marks"))
        if not busy then
            ImGUI_SameLine()
            ImGUI_Button("Put back##" .. backup.name, function() putBack(backup.name) end)
        end
    end
end

-- A switch that's on but can't work yet says what it waits for, with the
-- button that fixes it. It never turns itself off, it starts once all is there.
local function drawWait(what, label, fix)
    if not what then return end
    ImGUI_Text("Waiting for " .. what)
    if label then
        ImGUI_SameLine()
        ImGUI_Button(label, fix)
    end
end

-- what keeps syncing with HoYoLAB from working, nil when nothing does
local function hoyolabWait()
    if not HasPermission(HOYOLAB) then
        return "HoYoLAB access", "Allow##hoyolabWait", function() AskForPermission(HOYOLAB) end
    end
    local account = HoYoLAB_Account()
    if not account.loggedIn then
        if account.windowOpen then return "you to log in, in the HoYoLAB window" end
        return "you to log in to HoYoLAB", "Log in##hoyolabWait", function() HoYoLAB_LogIn() end
    end
end

-- what keeps marking opened chests and picked up oculi from working, nil when nothing does
local function gameWait()
    local traffic = GetGameTraffic()
    if not traffic.on then
        return "\"Read the game's traffic\" in the overlay's menu", "Turn it on##gameWait", AskForGameTraffic
    end
    if traffic.state == "off" then return "the game's traffic to start" end
    if traffic.state == "waiting" then return "the game to log in. If you're logged in already, log out to the door and back in once" end
    if traffic.state == "searching" then return "the session key, this can take a minute" end
    if traffic.state ~= "reading" then return "the game's traffic: " .. traffic.text end
end

local function hoyolabSection(busy)
    ImGUI_Separator()
    ImGUI_Text("HoYoLAB")
    local h = sync.hoyolab
    if hoyoJob then
        ImGUI_TextDisabled("Talking to HoYoLAB...")
    elseif not busy then
        local on = ImGUI_Checkbox("Keep in sync", h.on)
        if on ~= h.on then
            h.on, h.base = on, nil
            saveSync()
            if not on then
                status = nil
            elseif not HasPermission(HOYOLAB) then
                AskForPermission(HOYOLAB)
            elseif not hoyolabWait() then
                fullSync()
            end
        end
        if h.on then
            local what, label, fix = hoyolabWait()
            drawWait(what, label, fix)
            if not what then
                ImGUI_Button("Sync now", fullSync)
                if h.last > 0 then
                    ImGUI_SameLine()
                    ImGUI_TextDisabled("Last synced " .. ago(h.last))
                end
            end
        end
    end
    -- while syncing is on its waiting line has the buttons already
    if not HasPermission(HOYOLAB) then
        if not h.on then
            ImGUI_TextDisabled("Markers needs your OK to use your HoYoLAB account.")
            ImGUI_Button("Allow HoYoLAB access", function() AskForPermission(HOYOLAB) end)
        end
        return
    end

    local account = HoYoLAB_Account()
    if account.error ~= "" then ImGUI_Text(account.error) end
    if not account.loggedIn then
        if account.windowOpen then
            if not h.on then ImGUI_TextDisabled("Log in in the HoYoLAB window.") end
        elseif not h.on then
            ImGUI_Button("Log in to HoYoLAB", function() HoYoLAB_LogIn() end)
        end
        ImGUI_TextDisabled("Use email and password, the Google button may not work there.")
        return
    end
    ImGUI_Text("Logged in, account " .. account.account)
    ImGUI_SameLine()
    ImGUI_Button("Log out", function() HoYoLAB_LogOut() end)
    if (not characters or characters.account ~= account.account) and not charactersJob then
        charactersJob = { id = HoYoLAB_FetchCharacters(), account = account.account }
    end
    if characters and characters.account == account.account then
        if characters.error then ImGUI_TextDisabled("Couldn't get the characters: " .. characters.error) end
        for _, c in ipairs(characters.list) do
            ImGUI_TextDisabled(c.nickname .. ", " .. c.server .. ", AR " .. c.level .. ", UID " .. c.uid)
        end
        if #characters.list > 1 then
            ImGUI_TextDisabled("HoYoLAB keeps one set of marks for the account, every server shares it.")
        end
    end

    if not h.on and not hoyoJob and not busy then
        ImGUI_Button("Get from HoYoLAB", getFromHoyolab)
        ImGUI_SameLine()
        ImGUI_Button("Send to HoYoLAB", sendToHoyolab)
    end
    backups(busy)
end

local function syncPage()
    if status then
        if status.error then ImGUI_Text(status.text) else ImGUI_TextDisabled(status.text) end
        if status.captcha then
            ImGUI_TextDisabled("Solve the captcha on HoYoLAB's map, then try again.")
            ImGUI_Button("Open HoYoLAB's map", function() HoYoLAB_ShowMap() end)
        end
    end
    if question then
        ImGUI_Separator()
        for _, line in ipairs(question.lines) do ImGUI_Text(line) end
        for i, action in ipairs(question.actions) do
            if i > 1 then ImGUI_SameLine() end
            ImGUI_Button(action[1] .. "##question", function()
                question = nil
                action[2]()
            end)
        end
    end
    local busy = hoyoJob ~= nil or fileJob ~= nil or question ~= nil

    ImGUI_Separator()
    ImGUI_Text("Game")
    local on = ImGUI_Checkbox("Mark chests and oculi when I find them", sync.game.on)
    if on ~= sync.game.on then
        sync.game.on = on
        saveSync()
        if on and not GetGameTraffic().on then AskForGameTraffic() end
    end
    if sync.game.on then
        local what, label, fix = gameWait()
        drawWait(what, label, fix)
        if not what then ImGUI_TextDisabled("Working, on the Teyvat map") end
    end

    ImGUI_Separator()
    ImGUI_Text("File")
    ImGUI_TextDisabled(count(done) .. " collected here")
    if not busy then
        ImGUI_Button("Save to a file", exportFile)
        ImGUI_SameLine()
        ImGUI_Button("Load a file", importFile)
    end

    hoyolabSection(busy)
end

function Menu()
    if ImGUI_Tabs("markersPages", { "Markers", "Sync" }) == 2 then
        syncPage()
        return
    end

    if #groups == 0 then
        ImGUI_TextDisabled("No item files yet, nothing to pick from.")
        return
    end

    ImGUI_Separator()
    regionChips()

    ImGUI_Separator()
    local count = #barTypes
    ImGUI_Text(count == 0 and "Nothing picked" or count == 1 and "1 type on the map" or count .. " types on the map")
    if count > 0 then
        ImGUI_SameLine()
        ImGUI_Button("Clear", function()
            selected, hidden, hiddenBefore = {}, {}, nil
            updateBar()
            saveSelection()
        end)
    end
    -- every type of every group at once
    ImGUI_TextDisabled("Everything (big, takes a moment to load)")
    allChip(allTypes, "everything")

    for _, group in ipairs(groups) do
        local on = 0
        for _, kind in ipairs(group.types) do
            if selected[kind.id] then on = on + 1 end
        end
        local count = on == #group.types and "  (all on)" or on > 0 and ("  (" .. on .. " on)") or ""
        local open = ImGUI_CollapsingHeader(group.name .. count .. "###group" .. group.id, openGroups[group.id] == true)
        if open ~= (openGroups[group.id] == true) then
            openGroups[group.id] = open or nil
            saveSelection()
        end
        if open then typeTiles(group.types, "group" .. group.id) end
    end
end
