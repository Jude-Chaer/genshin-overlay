metadata = {
    name = "Markers",
    description = "Draws HoYoLAB's markers on the in-game map and minimap.",
    about = "Pick the types and regions on its page. Tap a marker on the map for its comments and to mark it as collected.",
    author = "genshin-overlay",
    version = "1.0.0",
    permissions = {
        take_clicks = { reason = "So you can tap a marker for its comments, and the bar on the map." },
        network = { reason = "For the comments: the marker's picture, text and appsample's comments." },
    },
}

local catalog = require("catalog")
local selection = require("selection")
local collected = require("collected")
local map = require("map")
local bar = require("bar")
local guide = require("guide")
local minimap = require("minimap")
local menu = require("menu")

function RegisterSettings()
    DefineExtensionSetting("mapScale", "Marker size on the map (%)", "int", 100, 20, 200)
    DefineExtensionSetting("minimapScale", "Marker size on the minimap (%)", "int", 100, 20, 200)
    DefineExtensionSetting("hideCollectedMap", "Hide collected ones on the map", "bool", false)
    DefineExtensionSetting("hideCollectedMinimap", "Hide collected ones on the minimap", "bool", true)
    DefineExtensionSetting("hideOtherLayers", "Hide markers on other floors", "bool", false)
    DefineExtensionSetting("mapBar", "Show the picked types as a bar on the map", "bool", true)
    DefineExtensionSetting("mergeClose", "Merge markers that overlap into one with a count", "bool", false)
    DefineExtensionSetting("tapGuides", "Tap a marker on the map to see its comments", "bool", true)
    DefineExtensionSetting("guideScale", "Comments size (%)", "int", 100, 60, 200)
end

function Init()
    selection.load()
    collected.load()
end

local asked = false

function Update()
    if catalog.update() then
        guide.close()
        selection.ready()
    end
    if #catalog.groups == 0 then return end

    -- without this there's no view while the map moves
    FollowMapWhileMoving()
    local view = GetMapView()
    if view then
        if view.noMapFloor then return end
        if not asked and GetSetting("tapGuides") then
            asked = true
            if PermissionState("take_clicks") == "not_asked" then AskForPermission("take_clicks") end
        end
        -- a guide from another map closes when the map switches
        if guide.open and guide.open.map ~= view.mapId then guide.close() end
        local half = map.draw(view)
        bar.draw(view)
        guide.draw(view, half)
        return
    end
    -- the guide closes with the map
    guide.close()

    if #selection.bar == 0 then return end
    local player = GetPlayer()
    if player.found then minimap.draw(player) end
end

function Menu()
    menu.draw()
end
