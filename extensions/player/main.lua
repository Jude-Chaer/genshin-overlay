metadata = {
    name = "Player position",
    description = "Shows where you are on the map, read from the minimap.",
    author = "genshin-overlay",
    version = "1.3.0",
}

function RegisterSettings()
    DefineExtensionSetting("show", "Show the position on screen", "bool", true)
    DefineExtensionSetting("showMap", "Draw the map where it thinks you are", "bool", true)
    DefineExtensionSetting("onTop", "Draw it over the minimap instead of next to it", "bool", false)
    DefineExtensionSetting("opacity", "Opacity", "float", 1.0, 0.0, 1.0)
end

-- HoYoLAB's map around the player in the square left to bottom, at the
-- minimap's size. If the spot is right it looks the same as the minimap.
local function drawMap(player, left, top, right, bottom)
    local grid = GetMapGrid(player.mapId)
    if not grid then return end

    local unitsPerPixel = player.unitsPerPixel
    local tile = grid.tileSize
    local centerX = (left + right) / 2
    local centerY = (top + bottom) / 2
    local half = (right - left) / 2 * unitsPerPixel

    local firstX = math.max(0, math.floor((player.lng - half + grid.originX) / tile))
    local lastX = math.min(grid.cols - 1, math.floor((player.lng + half + grid.originX) / tile))
    local firstY = math.max(0, math.floor((player.lat - half + grid.originY) / tile))
    local lastY = math.min(grid.rows - 1, math.floor((player.lat + half + grid.originY) / tile))

    local size = 128
    if tile / unitsPerPixel >= 192 then size = 256 end
    local opacity = GetSetting("opacity")

    PushClipRect(left, top, right, bottom)
    for y = firstY, lastY do
        for x = firstX, lastX do
            local texture = GetMapTile(player.mapId, x, y, size)
            if texture ~= 0 then
                local unitX = x * tile - grid.originX
                local unitY = y * tile - grid.originY
                local x0 = centerX + (unitX - player.lng) / unitsPerPixel
                local y0 = centerY + (unitY - player.lat) / unitsPerPixel
                local x1 = centerX + (unitX + tile - player.lng) / unitsPerPixel
                local y1 = centerY + (unitY + tile - player.lat) / unitsPerPixel
                DrawImage(texture, x0, y0, x1, y1, opacity)
            end
        end
    end
    -- under ground or water the minimap shows the floor, so that goes on top
    if player.floorId ~= 0 then
        local floor = GetMapFloor(player.groupId, player.floorId)
        if floor then
            local x0 = centerX + (floor.left - player.lng) / unitsPerPixel
            local y0 = centerY + (floor.top - player.lat) / unitsPerPixel
            local x1 = centerX + (floor.right - player.lng) / unitsPerPixel
            local y1 = centerY + (floor.bottom - player.lat) / unitsPerPixel
            DrawImage(floor.texture, x0, y0, x1, y1, opacity)
        end
    end
    -- the player
    DrawText("+", centerX - 6, centerY - 13, 24, 0xFF0000FF)
    PopClipRect()
end

function Update()
    local player = GetPlayerPosition()

    if GetSetting("show") then
        local text = "no minimap"
        if player.found then
            text = string.format("map %d   lat %.1f   lng %.1f", player.mapId, player.lat, player.lng)
            if player.floorId ~= 0 then text = text .. "   floor " .. player.floorId end
        elseif player.state == "near" then
            text = "lost it for a moment"
        elseif player.state == "searching" then
            text = "searching the map"
        elseif player.state == "nowhere" then
            text = "not on the map, in a domain?"
        end
        if not player.found and player.mapId ~= 0 then
            text = text .. string.format("   (last seen %.0f s ago)", player.age)
        end
        DrawText(text, 20, 300, 24, 0xFF00FFFF)
    end

    if player.found and GetSetting("showMap") then
        local side = player.right - player.left
        if GetSetting("onTop") then
            drawMap(player, player.left, player.top, player.right, player.bottom)
        else
            drawMap(player, player.right + side * 0.1, player.top, player.right + side * 1.1, player.bottom)
        end
    end
end
