-- The points the player collected, by HoYoLAB point id, kept in collected.json.
-- Everything that marks or unmarks goes through set, whatever it comes from.

local collected = {}

local FILE = "collected.json"
local ids = {}

-- goes up with every change, for what's worked out from the set
collected.version = 0

-- older saves have ids as 17.0
local function whole(list)
    local out = {}
    for _, id in ipairs(type(list) == "table" and list or {}) do
        local value = math.tointeger(id)
        if value then out[#out + 1] = value end
    end
    return out
end

function collected.load()
    ids = {}
    local saved = FileExists(FILE) and JSON_Decode(ReadFile(FILE))
    for _, id in ipairs(whole(saved)) do ids[id] = true end
    collected.version = collected.version + 1
end

local function save()
    local list = {}
    for id in pairs(ids) do list[#list + 1] = id end
    table.sort(list)
    WriteFile(FILE, JSON_Encode(list))
end

function collected.has(id)
    return ids[id] == true
end

function collected.set(id, on)
    if (ids[id] == true) == on then return end
    ids[id] = on or nil
    collected.version = collected.version + 1
    save()
end

return collected
