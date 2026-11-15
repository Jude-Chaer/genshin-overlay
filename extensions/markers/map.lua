-- The picked markers on the big map. Up to LIMIT on screen each has its own
-- badge; past that, or with "merge" on, the ones close together share a
-- badge with a count.

local catalog = require("catalog")
local selection = require("selection")
local collected = require("collected")
local guide = require("guide")

local map = {}

-- badge width at 100%, in pixels
map.MARKER = 50
local LIMIT = 2000
-- while merged, one badge each again only below this, so it doesn't flip at the limit
local UNMERGE = 1800
-- map units down the map's height where "merge" starts: the game's closest
-- zoom shows about 460, merging starts a few zoom steps out from it
local MERGE_FROM = 700

local merging = false
-- what the markers on screen were collected for
local collectedKey

-- the game's own UI showing now, a marker under it isn't drawn
local function underUi(view, x, y, half)
    for _, box in ipairs(view.ui) do
        if x + half > box.left and x - half < box.right and y + half > box.top and y - half < box.bottom then return true end
    end
    return false
end

-- the cells inside the box, walked one by one or, when the box has more
-- cells than the grid, through the ones it has
local function eachCell(grid, x0, y0, x1, y1, fn)
    local cell = grid.cell or catalog.CELL
    local cx0, cy0 = math.floor(x0 / cell), math.floor(y0 / cell)
    local cx1, cy1 = math.floor(x1 / cell), math.floor(y1 / cell)
    if (cx1 - cx0 + 1) * (cy1 - cy0 + 1) <= grid.cellCount then
        for cx = cx0, cx1 do
            for cy = cy0, cy1 do
                local list = grid.cells[catalog.cellKey(cx, cy)]
                if list and fn(list) then return end
            end
        end
    else
        for _, list in pairs(grid.cells) do
            if fn(list) then return end
        end
    end
end

-- the box of map units on screen, a badge more at each side
local function unitsOnScreen(view, half)
    local upp = view.unitsPerPixel
    return view.lng - (view.centerX - view.left + half) * upp, view.lat - (view.centerY - view.top + half) * upp,
        view.lng + (view.right - view.centerX + half) * upp, view.lat + (view.bottom - view.centerY + half) * upp
end

-- kept between frames, so nothing is made each frame
local onScreen = { n = 0, x = {}, y = {}, point = {} }
-- the visible points around the screen, a screen's half more at each side,
-- used again while the map is dragged inside that box
local around = { n = 0, point = {}, key = nil, x0 = 0, y0 = 0, x1 = 0, y1 = 0 }

local function gather(view, rules, x0, y0, x1, y1, limit)
    local n, visible = 0, selection.visible
    eachCell(selection.grid(view.mapId), x0, y0, x1, y1, function(list)
        for _, point in ipairs(list) do
            if point.x >= x0 and point.x <= x1 and point.y >= y0 and point.y <= y1 and visible(point, rules) then
                n = n + 1
                if n > limit then return true end
                around.point[n] = point
            end
        end
    end)
    return n
end

-- every visible marker on screen, false when there are more than limit
local function collect(view, rules, half, aroundKey, limit)
    local x0, y0, x1, y1 = unitsOnScreen(view, half)
    local inside = around.key == aroundKey and x0 >= around.x0 and y0 >= around.y0 and x1 <= around.x1 and y1 <= around.y1
    if not inside then
        local w, h = (x1 - x0) / 2, (y1 - y0) / 2
        around.key, around.x0, around.y0, around.x1, around.y1 = aroundKey, x0 - w, y0 - h, x1 + w, y1 + h
        around.n = gather(view, rules, around.x0, around.y0, around.x1, around.y1, LIMIT * 4)
        if around.n > LIMIT * 4 then
            -- too many around it, only the screen itself
            around.key, around.x0, around.y0, around.x1, around.y1 = nil, x0, y0, x1, y1
            around.n = gather(view, rules, x0, y0, x1, y1, limit)
            if around.n > limit then
                around.n, onScreen.n = 0, 0
                return false
            end
        end
    end
    local upp, n = view.unitsPerPixel, 0
    for i = 1, around.n do
        local point = around.point[i]
        if point.x >= x0 and point.x <= x1 and point.y >= y0 and point.y <= y1 then
            local x = view.centerX + (point.x - view.lng) / upp
            local y = view.centerY + (point.y - view.lat) / upp
            if not underUi(view, x, y, half) then
                n = n + 1
                if n > limit then
                    onScreen.n = 0
                    return false
                end
                onScreen.x[n], onScreen.y[n], onScreen.point[n] = x, y, point
            end
        end
    end
    onScreen.n = n
    return true
end

-- Merged spots: the visible points of the shown types joined with the ones
-- less than a badge away. Each zoom step's buckets are kept until
-- the selection, the layer or what's hidden changes. The open guide's point
-- and the collected look aren't part of it, they're looked up when drawn.
local merged = { key = nil, bySize = {} }

local function bucketsFor(view, rules, size)
    local key = table.concat({ view.mapId, selection.version, selection.whereVersion, rules.groupId, rules.floorId, tostring(rules.hideOther),
        tostring(rules.hideCollected), rules.hideCollected and collected.version or 0 }, ":")
    if merged.key ~= key then merged.key, merged.bySize = key, {} end
    local kept = merged.bySize[size]
    if kept then return kept end
    -- what's hidden here is only its rule, the open guide's point is drawn on its own
    local plain = { filter = rules.filter, groupId = rules.groupId, floorId = rules.floorId,
        hideCollected = rules.hideCollected, hideOther = rules.hideOther }
    -- every visible point with its type, in a grid a badge wide to find neighbours
    local points, typeOf, near = {}, {}, {}
    for _, typeId in ipairs(selection.shownTypes()) do
        local grid = catalog.on(typeId, view.mapId)
        if grid then
            for _, point in ipairs(grid.points) do
                if selection.visible(point, plain) then
                    points[#points + 1] = point
                    typeOf[point] = typeId
                    local k = catalog.cellKey(math.floor(point.x / size), math.floor(point.y / size))
                    local list = near[k]
                    if not list then
                        list = {}
                        near[k] = list
                    end
                    list[#list + 1] = point
                end
            end
        end
    end
    -- each spot starts at a point not taken yet and takes the free ones less
    -- than a badge away from it, so it sits where its markers are
    local taken, buckets, reach = {}, {}, size * size
    for _, seed in ipairs(points) do
        if not taken[seed] then
            local bucket = { x = 0, y = 0, stack = {}, other = true }
            local types, stack = {}, bucket.stack
            local cx, cy = math.floor(seed.x / size), math.floor(seed.y / size)
            for ix = cx - 1, cx + 1 do
                for iy = cy - 1, cy + 1 do
                    local list = near[catalog.cellKey(ix, iy)]
                    if list then
                        for _, point in ipairs(list) do
                            local dx, dy = point.x - seed.x, point.y - seed.y
                            if not taken[point] and dx * dx + dy * dy < reach then
                                taken[point] = true
                                stack[#stack + 1] = point
                                bucket.x, bucket.y = bucket.x + point.x, bucket.y + point.y
                                local typeId = typeOf[point]
                                types[typeId] = (types[typeId] or 0) + 1
                                if not selection.otherLayer(point, rules.groupId, rules.floorId) then bucket.other = false end
                            end
                        end
                    end
                end
            end
            local count = #stack
            bucket.x, bucket.y = bucket.x / count, bucket.y / count
            -- the badge shows the type it has most of
            local best, bestN = nil, 0
            for typeId, n in pairs(types) do
                if n > bestN then best, bestN = typeId, n end
            end
            bucket.type = best
            buckets[#buckets + 1] = bucket
        end
    end
    -- by cells a few buckets wide, so drawing only looks at the ones on screen
    local cell = size * 8
    local cells, cellCount = {}, 0
    for _, bucket in ipairs(buckets) do
        local k = catalog.cellKey(math.floor(bucket.x / cell), math.floor(bucket.y / cell))
        local list = cells[k]
        if not list then
            list = {}
            cells[k] = list
            cellCount = cellCount + 1
        end
        list[#list + 1] = bucket
    end
    local kept = { cell = cell, cells = cells, cellCount = cellCount }
    merged.bySize[size] = kept
    return kept
end

-- faded when every one in it is collected, worked out again after a change
local function allCollected(bucket)
    if bucket.seen ~= collected.version then
        bucket.seen, bucket.collected = collected.version, true
        for _, point in ipairs(bucket.stack) do
            if not collected.has(point.id) then
                bucket.collected = false
                break
            end
        end
    end
    return bucket.collected
end

-- a badge's width in map units, rounded up to a power of two
local function bucketSize(view, half)
    local units = half * 2 * view.unitsPerPixel
    local size = 8
    while size < units do size = size * 2 end
    return size
end

-- the options of the looks, made once; faded only says collected, another
-- layer gets the layer picture and a smaller badge
local plainLook, fadedLook, openLook = {}, { faded = true }, {}
local layerLook = {
    [false] = { up = { layer = "up" }, down = { layer = "down" } },
    [true] = { up = { faded = true, layer = "up" }, down = { faded = true, layer = "down" } },
}
-- a badge on another layer, part of the size of one on the layer showing
map.OTHER_LAYER = 0.75

-- arrow: "up" or "down" for a point on another layer
local function drawBadge(x, y, half, point, typeId, isCollected, arrow, open)
    local look = open and openLook or arrow and layerLook[isCollected][arrow] or isCollected and fadedLook or plainLook
    if open then openLook.layer = arrow end
    if arrow then half = half * map.OTHER_LAYER end
    DrawBadge(x, y, half, catalog.icon(typeId), look)
end

local function drawCount(x, y, half, count)
    DrawBubble(x + half * 0.72, y - half * 0.72, half * 0.42, count > 999 and "999+" or tostring(count))
end

-- only the middle of a badge takes taps, so the map can still be dragged from
-- right next to one; only the one under the mouse is claimed
local function tapped(x, y, tapHalf, mouseX, mouseY)
    return tapHalf and mouseX and math.abs(mouseX - x) <= tapHalf and math.abs(mouseY - y) <= tapHalf
        and ClickArea(x - tapHalf, y - tapHalf, x + tapHalf, y + tapHalf)
end

local shownBuckets = { n = 0, bucket = {}, x = {}, y = {} }

local function drawMerged(view, rules, half, tapHalf, mouseX, mouseY)
    local grid = bucketsFor(view, rules, bucketSize(view, half))
    local upp, n = view.unitsPerPixel, 0
    local x0, y0, x1, y1 = unitsOnScreen(view, half)
    eachCell(grid, x0, y0, x1, y1, function(list)
        for _, bucket in ipairs(list) do
            local x = view.centerX + (bucket.x - view.lng) / upp
            local y = view.centerY + (bucket.y - view.lat) / upp
            if x + half > view.left and x - half < view.right and y + half > view.top and y - half < view.bottom then
                if not underUi(view, x, y, half) then
                    n = n + 1
                    shownBuckets.bucket[n], shownBuckets.x[n], shownBuckets.y[n] = bucket, x, y
                end
            end
        end
    end)
    local tappedBucket
    -- the other layers first, so the layer showing is on top
    for _, faint in ipairs({ true, false }) do
        for i = 1, n do
            local bucket = shownBuckets.bucket[i]
            if bucket.other == faint then
                local x, y = shownBuckets.x[i], shownBuckets.y[i]
                local arrow = bucket.other and selection.layerArrow(bucket.stack[1], rules.groupId, rules.floorId)
                local shrink = arrow and map.OTHER_LAYER or 1
                drawBadge(x, y, half, nil, bucket.type, allCollected(bucket), arrow, false)
                if #bucket.stack > 1 then drawCount(x, y, half * shrink, #bucket.stack) end
                if tapped(x, y, tapHalf and tapHalf * shrink, mouseX, mouseY) then tappedBucket = bucket end
            end
        end
    end
    -- the open guide's marker on top, where it is
    local open = guide.open
    if open then
        local point = open.point
        local x = view.centerX + (point.x - view.lng) / upp
        local y = view.centerY + (point.y - view.lat) / upp
        drawBadge(x, y, half, point, point.type, collected.has(point.id), selection.layerArrow(point, rules.groupId, rules.floorId), true)
    end
    if tappedBucket then guide.tap(tappedBucket.stack, view.mapId) end
end

local function drawEach(view, rules, half, tapHalf, mouseX, mouseY)
    local n = onScreen.n
    local tappedAt, openAt
    for _, faint in ipairs({ true, false }) do
        for i = 1, n do
            local point = onScreen.point[i]
            local other = selection.otherLayer(point, rules.groupId, rules.floorId)
            if other == faint then
                local x, y = onScreen.x[i], onScreen.y[i]
                if point.id == rules.keep then openAt = i end
                local arrow = other and selection.layerArrow(point, rules.groupId, rules.floorId)
                drawBadge(x, y, half, point, point.type, collected.has(point.id), arrow, false)
                local tapAt = tapHalf and (arrow and tapHalf * map.OTHER_LAYER or tapHalf)
                if tapped(x, y, tapAt, mouseX, mouseY) then tappedAt = i end
            end
        end
    end
    -- the open one again on top, so its ring isn't under the others
    if openAt then
        local point = onScreen.point[openAt]
        drawBadge(onScreen.x[openAt], onScreen.y[openAt], half, point, point.type, collected.has(point.id),
            selection.layerArrow(point, rules.groupId, rules.floorId), true)
    end
    -- a tap takes every one that overlaps the tapped one, at the size each is drawn
    if tappedAt then
        local function drawnHalf(point)
            return selection.otherLayer(point, rules.groupId, rules.floorId) and half * map.OTHER_LAYER or half
        end
        local stack = { onScreen.point[tappedAt] }
        local own = drawnHalf(onScreen.point[tappedAt])
        for i = 1, n do
            local dx, dy = onScreen.x[i] - onScreen.x[tappedAt], onScreen.y[i] - onScreen.y[tappedAt]
            local reach = own + drawnHalf(onScreen.point[i])
            if i ~= tappedAt and dx * dx + dy * dy < reach * reach then stack[#stack + 1] = onScreen.point[i] end
        end
        guide.tap(stack, view.mapId)
    end
end

function map.draw(view)
    openLook.ring = GetTheme().accent
    local half = map.MARKER / 2 * GetSetting("mapScale") / 100
    local open = guide.open
    local rules = selection.rules(view.mapId, view.lng, view.lat, view.groupId, view.floorId,
        GetSetting("hideCollectedMap"), open and open.point.id)

    local tapHalf = GetSetting("tapGuides") and half * 0.6 or nil
    local mouseX, mouseY = GetMousePosition()
    if guide.covers(mouseX, mouseY) then tapHalf = nil end

    PushClipRect(view.left, view.top, view.panelLeft or view.right, view.bottom)
    -- the setting merges only once zoomed out from the game's closest zoom
    local merge = GetSetting("mergeClose") and view.unitsPerPixel * (view.bottom - view.top) >= MERGE_FROM
    if not merge then
        -- a still map shows the same ones as last frame
        local ui = {}
        for i, box in ipairs(view.ui) do
            ui[i] = box.left .. "," .. box.top .. "," .. box.right .. "," .. box.bottom
        end
        local key = table.concat({ view.mapId, view.lat, view.lng, view.unitsPerPixel, view.groupId, view.floorId,
            view.left, view.top, view.right, view.bottom, table.concat(ui, ";"), half, selection.version, selection.whereVersion,
            rules.hideCollected and collected.version or 0, tostring(rules.keep), tostring(rules.hideOther) }, ":")
        if key ~= collectedKey then
            collectedKey = key
            -- what the points around were picked by, all but where the map is
            local aroundKey = table.concat({ view.mapId, view.groupId, view.floorId,
                selection.version, selection.whereVersion, rules.hideCollected and collected.version or 0, tostring(rules.keep),
                tostring(rules.hideOther) }, ":")
            merging = not collect(view, rules, half, aroundKey, merging and UNMERGE or LIMIT)
        end
        merge = merging
    else
        collectedKey = nil
    end
    if merge then
        drawMerged(view, rules, half, tapHalf, mouseX, mouseY)
    else
        drawEach(view, rules, half, tapHalf, mouseX, mouseY)
    end
    PopClipRect()
    return half
end

return map
