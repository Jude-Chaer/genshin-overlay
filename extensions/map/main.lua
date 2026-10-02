metadata = {
    name = "Map",
    description = "Draws HoYoLAB's map over the in-game map, lined up with it.",
    author = "genshin-overlay",
    version = "1.0.0",
}

function RegisterSettings()
    DefineExtensionSetting("showMap", "Show the map overlay", "bool", true)
    DefineExtensionSetting("opacity", "Opacity", "float", 0.5, 0.0, 1.0)
end

-- tiles are shrunk to about the size they're drawn at, so zooming out stays cheap
local function tileSizeFor(pixelsPerTile)
    if pixelsPerTile >= 192 then return 256 end
    if pixelsPerTile >= 96 then return 128 end
    if pixelsPerTile >= 48 then return 64 end
    return 32
end

function Update()
    if not GetSetting("showMap") then return end

    local view = GetMapView()
    if not view then return end
    local grid = GetMapGrid(view.mapId)
    if not grid then return end

    local unitsPerPixel = view.unitsPerPixel
    local tile = grid.tileSize

    -- the map area the game window shows, in map units
    local minX = view.lng - (view.centerX - view.left) * unitsPerPixel
    local maxX = view.lng + (view.right - view.centerX) * unitsPerPixel
    local minY = view.lat - (view.centerY - view.top) * unitsPerPixel
    local maxY = view.lat + (view.bottom - view.centerY) * unitsPerPixel

    -- which tiles that is
    local firstX = math.max(0, math.floor((minX + grid.originX) / tile))
    local lastX = math.min(grid.cols - 1, math.floor((maxX + grid.originX) / tile))
    local firstY = math.max(0, math.floor((minY + grid.originY) / tile))
    local lastY = math.min(grid.rows - 1, math.floor((maxY + grid.originY) / tile))

    local size = tileSizeFor(tile / unitsPerPixel)
    local opacity = GetSetting("opacity")

    -- underground the game dims the surface, so we do too
    local underground = view.groupId ~= 0
    local surfaceOpacity = opacity
    if underground then
        surfaceOpacity = opacity * 0.35
    end

    PushClipRect(view.left, view.top, view.right, view.bottom)
    for y = firstY, lastY do
        for x = firstX, lastX do
            local texture = GetMapTile(view.mapId, x, y, size)
            if texture ~= 0 then
                -- tile corners in map units, then on screen
                local unitX = x * tile - grid.originX
                local unitY = y * tile - grid.originY
                local x0 = view.centerX + (unitX - view.lng) / unitsPerPixel
                local y0 = view.centerY + (unitY - view.lat) / unitsPerPixel
                local x1 = view.centerX + (unitX + tile - view.lng) / unitsPerPixel
                local y1 = view.centerY + (unitY + tile - view.lat) / unitsPerPixel
                DrawImage(texture, x0, y0, x1, y1, surfaceOpacity)
            end
        end
    end

    -- the floor you're on, drawn over the box of the map it covers
    if underground then
        local floor = GetMapFloor(view.groupId, view.floorId)
        if floor then
            local x0 = view.centerX + (floor.left - view.lng) / unitsPerPixel
            local y0 = view.centerY + (floor.top - view.lat) / unitsPerPixel
            local x1 = view.centerX + (floor.right - view.lng) / unitsPerPixel
            local y1 = view.centerY + (floor.bottom - view.lat) / unitsPerPixel
            DrawImage(floor.texture, x0, y0, x1, y1, opacity)
        end
    end
    PopClipRect()
end
