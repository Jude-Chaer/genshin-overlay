-- The picked types as badges along the top of the map. Tapping one hides its
-- markers until it's tapped again; with more than one an "All" badge comes
-- first, and what doesn't fit opens from a "+N" badge at the end.

local catalog = require("catalog")
local selection = require("selection")

local bar = {}

local BADGE = 40
local GAP = 6
local PAD = 7
local MARGIN = 20

-- whether the "+N" badge has the rest open below it
local moreOpen = false

function bar.draw(view)
    local list = selection.bar
    if not GetSetting("mapBar") or #list == 0 then return end
    local theme = GetTheme()
    local allButton = #list > 1 and 1 or 0
    local height = BADGE + PAD * 2
    local step = BADGE + GAP
    local y0 = view.top + 18

    local left, right = FreeSpace(y0, y0 + height)
    if not left then return end
    left, right = left + MARGIN, right - MARGIN
    local slots = math.max(2, math.floor((right - left - PAD * 2 + GAP) / step))
    local count, more = #list, 0
    if count + allButton > slots then
        -- too little room for a badge, All and +N: All goes first
        if slots - allButton - 1 < 1 then allButton = 0 end
        count = #list <= slots - allButton and #list or math.max(1, slots - allButton - 1)
        more = #list - count
    end
    if more == 0 then moreOpen = false end
    local used = count + allButton + (more > 0 and 1 or 0)

    local width = used * BADGE + (used - 1) * GAP + PAD * 2
    local x0 = math.max(left, (left + right) / 2 - width / 2)
    local bottom = y0 + height
    -- the whole bar takes the mouse, a tap between two badges shouldn't move the map
    ClickArea(x0, y0, x0 + width, bottom)
    DrawRect(x0, y0, x0 + width, bottom, theme.panel, { rounding = height / 2 })
    DrawRect(x0, y0, x0 + width, bottom, theme.cardLine, { rounding = height / 2, outline = 1.5 })

    local label, labelX
    local y = y0 + height / 2
    local half = BADGE / 2

    local function typeBadge(id, x, by)
        local clicked, hovered = ClickArea(x - half, by - half, x + half, by + half)
        if clicked then selection.toggleHidden(id) end
        local hidden = selection.hidden[id] == true
        DrawBadge(x, by, hovered and half * 1.08 or half, catalog.icon(id), { faded = hidden })
        if hovered then
            local kind = catalog.typeOf[id]
            label, labelX = (kind and kind.name or "?") .. (hidden and "  (hidden)" or ""), x - half
        end
    end

    local function textBadge(x, text, dim)
        local clicked, hovered = ClickArea(x - half, y - half, x + half, y + half)
        DrawBadge(x, y, hovered and half * 1.08 or half, nil, { faded = dim })
        local w, h = TextSize(text, 15)
        DrawText(text, x - w / 2, y - h / 2, 15, dim and theme.muted or theme.text)
        return clicked, hovered
    end

    if allButton == 1 then
        local x = x0 + PAD + half
        local clicked, hovered = textBadge(x, "All", selection.hiddenBefore ~= nil)
        if clicked then selection.toggleHideAll() end
        if hovered then
            label, labelX = selection.hiddenBefore and "Show the ones from before" or "Hide all", x - half
        end
    end

    for i = 1, count do
        typeBadge(list[i], x0 + PAD + (i - 1 + allButton) * step + half, y)
    end

    if more > 0 then
        local x = x0 + PAD + (used - 1) * step + half
        local clicked, hovered = textBadge(x, moreOpen and "-" or "+" .. more, false)
        if clicked then moreOpen = not moreOpen end
        if hovered then label, labelX = moreOpen and "Show fewer" or more .. " more", x - half end

        -- the rest in rows under the bar, as wide as it
        if moreOpen then
            local rows = math.ceil(more / used)
            local top = bottom + 6
            bottom = top + rows * step - GAP + PAD * 2
            ClickArea(x0, top, x0 + width, bottom)
            DrawRect(x0, top, x0 + width, bottom, theme.panel, { rounding = half + PAD })
            DrawRect(x0, top, x0 + width, bottom, theme.cardLine, { rounding = half + PAD, outline = 1.5 })
            for i = 1, more do
                local column, row = (i - 1) % used, math.floor((i - 1) / used)
                typeBadge(list[count + i], x0 + PAD + column * step + half, top + PAD + row * step + half)
            end
        end
    end

    -- the name under the badge the mouse is on, below everything that's open
    if label then
        local w, h = TextSize(label, 16)
        local ty = bottom + 8
        DrawRect(labelX - 8, ty - 5, labelX + w + 8, ty + h + 5, theme.panel, { rounding = 6 })
        DrawText(label, labelX, ty, 16, theme.text)
    end
end

return bar
