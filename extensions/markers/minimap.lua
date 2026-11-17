-- The same badges inside the minimap's circle. It only shows: in game the
-- cursor is locked, and a claimed area would take clicks meant for the game.

local catalog = require("catalog")
local selection = require("selection")
local collected = require("collected")
local map = require("map")

local minimap = {}

-- badge width at 100%, part of the minimap's radius
local MARKER = 0.16
-- faded only says collected, another layer gets the layer picture and the
-- map's smaller badge
local plainLook, fadedLook = {}, { faded = true }
local layerLook = {
    [false] = { up = { layer = "up" }, down = { layer = "down" } },
    [true] = { up = { faded = true, layer = "up" }, down = { faded = true, layer = "down" } },
}

function minimap.draw(player)
    local half = player.radius * MARKER / 2 * GetSetting("minimapScale") / 100
    -- only what's fully inside the circle
    local inside = player.radius - half
    local rules = selection.rules(player.mapId, player.lng, player.lat, player.groupId, player.floorId,
        GetSetting("hideCollectedMinimap"))
    local upp = player.unitsPerPixel
    local reach = inside * upp
    local cell = catalog.CELL
    local cx0, cy0 = math.floor((player.lng - reach) / cell), math.floor((player.lat - reach) / cell)
    local cx1, cy1 = math.floor((player.lng + reach) / cell), math.floor((player.lat + reach) / cell)
    local grid = selection.grid(player.mapId)
    -- the other layers first, so the layer the player is on is on top
    for _, faint in ipairs({ true, false }) do
        for cx = cx0, cx1 do
            for cy = cy0, cy1 do
                for _, point in ipairs(grid.cells[catalog.cellKey(cx, cy)] or {}) do
                    local other = selection.otherLayer(point, player.groupId, player.floorId)
                    if other == faint and selection.visible(point, rules) then
                        local dx, dy = (point.x - player.lng) / upp, (point.y - player.lat) / upp
                        if dx * dx + dy * dy <= inside * inside then
                            local done = collected.has(point.id)
                            local look = other and layerLook[done][selection.layerArrow(point, player.groupId, player.floorId)]
                                or done and fadedLook or plainLook
                            DrawBadge(player.centerX + dx, player.centerY + dy, other and half * map.OTHER_LAYER or half,
                                catalog.icon(point.type), look)
                        end
                    end
                end
            end
        end
    end
end

return minimap
