-- A tapped marker's guide next to it: the picture and text, appsample's
-- comments one at a time, < > through the markers on that spot, and
-- "Mark as collected".

local catalog = require("catalog")
local collected = require("collected")

local guide = {}

-- the open one: { stack = { points }, at, point, map }, nil when closed
guide.open = nil
local commentAt = 1
-- where it was drawn last frame, a tap there isn't for the markers under it
local rect

local GAP = 10
-- how far the map can move under the open guide before it closes, past
-- what a zoom moves it, and the zoom change that starts the count again
local MOVE_CLOSES = 8
local ZOOM_RESTARTS = 0.02

function guide.close()
    guide.open, rect = nil, nil
end

function guide.covers(x, y)
    return rect and x and x >= rect[1] and x <= rect[3] and y >= rect[2] and y <= rect[4]
end

-- the one < or > asked for, shown once the frame's guide is drawn
local nextAt = nil

local function show(at)
    local count = #guide.open.stack
    at = (at - 1) % count + 1
    guide.open.at, guide.open.point = at, guide.open.stack[at]
    commentAt = 1
end

-- the same spot tapped again closes it
function guide.tap(stack, mapId)
    if guide.open and guide.open.stack[1].id == stack[1].id then
        guide.close()
        return
    end
    guide.open = { stack = stack, map = mapId }
    show(1)
    if PermissionState("network") == "not_asked" then AskForPermission("network") end
end

-- sizes are for 100%, k is the guide size picked
local k = 1
local function px(n) return n * k end

local theme

local function button(x0, y0, x1, y1, label, size)
    local clicked, hovered = ClickArea(x0, y0, x1, y1)
    DrawRect(x0, y0, x1, y1, hovered and theme.picked or theme.button, { rounding = (y1 - y0) / 2 })
    local w, h = TextSize(label, size)
    DrawText(label, (x0 + x1 - w) / 2, (y0 + y1 - h) / 2, size, theme.text)
    return clicked
end

-- text in lines that fit width, gives back how tall it is
local function textBlock(text, x, y, size, color, width, draw)
    local _, h = TextSize(text, size, { wrap = width })
    if draw then DrawText(text, x, y, size, color, { wrap = width }) end
    return h
end

-- a picture fit into width and at most maxHeight tall, nothing until it's loaded
local function picture(url, width, maxHeight)
    if not url or url == "" or maxHeight <= 0 then return nil end
    local image, w, h = GetWebImage(url)
    if not image or not w or w == 0 then return nil end
    local scale = math.min(width / w, maxHeight / h)
    return image, w * scale, h * scale
end

-- top to bottom, gives back how tall it is; with draw off it only measures,
-- so the pictures can be made smaller when it doesn't fit
local function layout(info, x0, y0, width, maxPicture, draw)
    local text, small, big = px(16), px(14), px(20)
    local inner = width - px(32)
    local x, y = x0 + px(16), y0 + px(16)
    local open = guide.open
    local point = open.point

    local kind = catalog.typeOf[point.type]
    local nameHeight = textBlock(kind and kind.name or "Marker", x, y, big, theme.text, inner - px(40), draw)
    if draw and button(x + inner - px(30), y - px(2), x + inner, y + px(28), "X", text) then
        guide.close()
        return 0
    end
    y = y + math.max(nameHeight, px(28)) + px(10)

    local count = #open.stack
    if count > 1 then
        if draw then
            textBlock(open.at .. " of " .. count .. " here", x, y + px(4), text, theme.muted, inner - px(80), true)
            -- the next one shows from the next frame, this one is drawn whole
            if button(x + inner - px(70), y, x + inner - px(38), y + px(26), "<", text) then nextAt = open.at - 1 end
            if button(x + inner - px(32), y, x + inner, y + px(26), ">", text) then nextAt = open.at + 1 end
        end
        y = y + px(36)
    end

    local isCollected = collected.has(point.id)
    if draw and button(x, y, x + inner, y + px(32), isCollected and "Collected, tap to undo" or "Mark as collected", text) then
        collected.set(point.id, not isCollected)
    end
    y = y + px(46)

    if PermissionState("network") ~= "allowed" then
        return y + textBlock("Comments need Network Access, on this extension's Permissions tab.", x, y, text, theme.muted, inner, draw) + px(16) - y0
    end
    if info == false then
        return y + textBlock("No comments for this one.", x, y, text, theme.muted, inner, draw) + px(16) - y0
    end
    if not info or info.failed then
        local message = info and "Couldn't load the comments, trying again in a bit." or "Loading the comments..."
        return y + textBlock(message, x, y, text, theme.muted, inner, draw) + px(16) - y0
    end

    local image, w, h = picture(info.picture, inner, maxPicture)
    if image then
        if draw then DrawImage(image, x + (inner - w) / 2, y, x + (inner + w) / 2, y + h) end
        y = y + h + px(10)
    end
    if info.text ~= "" then
        y = y + textBlock(info.text, x, y, text, theme.text, inner, draw) + px(12)
    end

    local comments = info.comments
    if #comments == 0 then
        if info.text == "" and not info.picture then
            y = y + textBlock("No comments for this one yet.", x, y, text, theme.muted, inner, draw) + px(6)
        end
        return y + px(16) - y0
    end

    -- one comment at a time, < and > step through them
    commentAt = math.max(1, math.min(commentAt, #comments))
    if draw then
        DrawRect(x, y, x + inner, y + 1, theme.line)
        textBlock("From appsample  " .. commentAt .. " / " .. #comments, x, y + px(12), text, theme.muted, inner - px(80), true)
        if #comments > 1 then
            if button(x + inner - px(70), y + px(8), x + inner - px(38), y + px(34), "<", text) then
                commentAt = commentAt == 1 and #comments or commentAt - 1
            end
            if button(x + inner - px(32), y + px(8), x + inner, y + px(34), ">", text) then
                commentAt = commentAt == #comments and 1 or commentAt + 1
            end
        end
    end
    y = y + px(44)

    local comment = comments[commentAt]
    image, w, h = picture(comment.picture, inner, maxPicture)
    if image then
        if draw then DrawImage(image, x + (inner - w) / 2, y, x + (inner + w) / 2, y + h) end
        y = y + h + px(8)
    end
    if comment.text ~= "" then
        y = y + textBlock(comment.text, x, y, text, theme.text, inner, draw) + px(6)
    end
    y = y + textBlock(comment.author .. "  " .. comment.date .. "  +" .. comment.likes, x, y, small, theme.muted, inner, draw)
    return y + px(16) - y0
end

-- under its marker, or above it when there's more room there; made to fit:
-- the pictures get smaller first, past that the bottom is cut off
function guide.draw(view, half)
    rect = nil
    local open = guide.open
    if not open then return end
    theme = GetTheme()
    local info = GetPointGuide(open.point.id)
    local markerX = view.centerX + (open.point.x - view.lng) / view.unitsPerPixel
    local markerY = view.centerY + (open.point.y - view.lat) / view.unitsPerPixel

    -- a game panel or the game's own list of the markers on a spot opening
    -- or closing, the gear goes while one is open
    for _, key in ipairs({ "panel", "gear" }) do
        if view[key] ~= "unsure" then
            open[key] = open[key] or view[key]
            if view[key] ~= open[key] then
                guide.close()
                return
            end
        end
    end

    -- the map's centre in pixels from where it was at the open; a zoom round
    -- a point off the centre moves it too, at most this far
    local upp = view.unitsPerPixel
    local zoomed = open.upp and math.abs(math.log(upp / open.upp)) or 0
    local reach = math.sqrt((view.right - view.left) ^ 2 + (view.bottom - view.top) ^ 2) / 2 * zoomed
    if not open.upp or zoomed > ZOOM_RESTARTS then
        open.lng, open.lat, open.upp = view.lng, view.lat, upp
    elseif math.abs(view.lng - open.lng) / upp > MOVE_CLOSES + reach or math.abs(view.lat - open.lat) / upp > MOVE_CLOSES + reach then
        guide.close()
        return
    end

    k = GetSetting("guideScale") / 100
    local right = view.panelLeft or view.right
    local width = math.min(px(400), right - view.left - GAP * 2)
    local x0 = math.max(view.left + GAP, math.min(markerX - width / 2, right - width - GAP))
    local below = view.bottom - GAP - (markerY + half + GAP)
    local above = (markerY - half - GAP) - (view.top + GAP)
    local room = math.max(below, above)

    local maxPicture = px(240)
    while maxPicture > px(120) and layout(info, x0, 0, width, maxPicture, false) > room do
        maxPicture = maxPicture - px(20)
    end
    local height = math.min(layout(info, x0, 0, width, maxPicture, false), room)
    local y0 = below >= height and markerY + half + GAP or markerY - half - GAP - height
    y0 = math.max(view.top + GAP, math.min(y0, view.bottom - GAP - height))
    local x1, y1 = x0 + width, y0 + height

    ClickArea(x0, y0, x1, y1)
    rect = { x0, y0, x1, y1 }
    DrawRect(x0, y0, x1, y1, theme.panel, { rounding = px(12) })
    DrawRect(x0, y0, x1, y1, theme.cardLine, { rounding = px(12), outline = 1.5 })
    PushClipRect(x0, y0, x1, y1)
    layout(info, x0, y0, width, maxPicture, true)
    if nextAt and guide.open then show(nextAt) end
    nextAt = nil
    PopClipRect()
end

return guide
