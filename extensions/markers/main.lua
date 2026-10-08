metadata = {
    name = "Markers",
    description = "Proof of concept: draws HoYoLAB's common chests on the in-game map.",
    author = "genshin-overlay",
    version = "0.1.0",
}

-- only Teyvat and only common chests
local MAP_ID = 2
local COMMON_CHEST = 17
local POINTS_URL = "https://sg-public-api.hoyolab.com/common/map_user/ys_obc/v1/map/point/list?app_sn=ys_obc&lang=en-us&map_id=" .. MAP_ID
local POINTS_FILE = "chests.json"
local ICON_FILE = "chest.png"
local ICON_SIZE = 32

local points = {}
local icon = 0

-- HoYoLAB only gives every point of the map at once (about 22 MB), so the
-- chests are picked out and saved, and next time only that small file is read
local function download()
    local response = Net_Get(POINTS_URL, true)
    if not response or response.status ~= 200 then return false end
    local body = JSON_Decode(response.body)
    if not body or body.retcode ~= 0 then return false end

    local chests = {}
    for _, point in ipairs(body.data.point_list) do
        if point.label_id == COMMON_CHEST then
            -- points underground say which floor they're on
            local group = point.point_group
            chests[#chests + 1] = {
                x = point.x_pos,
                y = point.y_pos,
                group = group and group.group_id or 0,
                floor = group and group.floor_id or 0,
            }
        end
    end

    for _, label in ipairs(body.data.label_list) do
        if label.id == COMMON_CHEST then
            local image = Net_Get(label.icon)
            if not image or image.status ~= 200 then return false end
            WriteFile(ICON_FILE, image.body)
        end
    end

    WriteFile(POINTS_FILE, JSON_Encode(chests))
    return true
end

function Init()
    if not FileExists(POINTS_FILE) or not FileExists(ICON_FILE) then
        if not download() then
            print("Markers: couldn't get the points from HoYoLAB")
            return
        end
    end

    points = JSON_Decode(ReadFile(POINTS_FILE)) or {}
    icon = LoadTexture(ICON_FILE)
end

function Update()
    -- without this there's no view while the map moves
    FollowMapWhileMoving()

    local view = GetMapView()
    if not view or view.mapId ~= MAP_ID then return end

    local half = ICON_SIZE / 2
    PushClipRect(view.left, view.top, view.right, view.bottom)
    for _, point in ipairs(points) do
        -- underground only the floor you're on, above ground only what's above ground
        if point.group == view.groupId and (point.group == 0 or point.floor == view.floorId) then
            -- x_pos / y_pos are the same map units as the view's lng / lat
            local x = view.centerX + (point.x - view.lng) / view.unitsPerPixel
            local y = view.centerY + (point.y - view.lat) / view.unitsPerPixel
            DrawImage(icon, x - half, y - half, x + half, y + half)
        end
    end
    PopClipRect()
end
