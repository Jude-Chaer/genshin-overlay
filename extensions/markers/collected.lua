-- The points the player collected, by HoYoLAB point id, kept in collected.json.
-- Everything that marks or unmarks goes through set or change, whatever it
-- comes from.

local collected = {}

local FILE = "collected.json"
local ids = {}

-- goes up with every change, for what's worked out from the set
collected.version = 0

-- called with (id, on, from) for each point that changed; from is who
-- changed it: "tap", "file", "get" or a site name
collected.onChange = nil

-- older saves have ids as 17.0
function collected.whole(list)
    local out = {}
    for _, id in ipairs(type(list) == "table" and list or {}) do
        local value = math.tointeger(id)
        if value and value > 0 then out[#out + 1] = value end
    end
    return out
end

function collected.load()
    ids = {}
    local saved = FileExists(FILE) and JSON_Decode(ReadFile(FILE))
    for _, id in ipairs(collected.whole(saved)) do ids[id] = true end
    collected.version = collected.version + 1
end

function collected.list()
    local list = {}
    for id in pairs(ids) do list[#list + 1] = id end
    table.sort(list)
    return list
end

local function save()
    WriteFile(FILE, JSON_Encode(collected.list()))
end

function collected.has(id)
    return ids[id] == true
end

function collected.count()
    local n = 0
    for _ in pairs(ids) do n = n + 1 end
    return n
end

-- a copy of the set, id = true
function collected.copy()
    local out = {}
    for id in pairs(ids) do out[id] = true end
    return out
end

local function changed(id, on, from)
    if collected.onChange then collected.onChange(id, on, from) end
end

function collected.set(id, on, from)
    if (ids[id] == true) == on then return end
    ids[id] = on or nil
    collected.version = collected.version + 1
    save()
    changed(id, on, from or "tap")
end

-- marks the ids in add and unmarks the ones in remove, both lists, with one save
function collected.change(add, remove, from)
    local done = {}
    for _, id in ipairs(add) do
        if not ids[id] then
            ids[id] = true
            done[#done + 1] = { id, true }
        end
    end
    for _, id in ipairs(remove) do
        if ids[id] then
            ids[id] = nil
            done[#done + 1] = { id, false }
        end
    end
    if #done == 0 then return end
    collected.version = collected.version + 1
    save()
    for _, each in ipairs(done) do changed(each[1], each[2], from) end
end

-- Before a change that unmarks several here, the set as it is goes to a file
-- in the extension's folder; the newest few are kept, the oldest one is
-- written over.
local BACKUPS = 5
local function backupFile(slot) return "collected-backup-" .. slot .. ".json" end

-- goes up with every backup, for the list on the page
collected.backupVersion = 0

-- newest first: { slot, at (GetOSTimeMS when made), ids }
function collected.backups()
    local out = {}
    for slot = 1, BACKUPS do
        local name = backupFile(slot)
        local data = FileExists(name) and JSON_Decode(ReadFile(name))
        if type(data) == "table" and type(data.at) == "number" then
            out[#out + 1] = { slot = slot, at = data.at, ids = collected.whole(data.ids) }
        end
    end
    table.sort(out, function(a, b) return a.at > b.at end)
    return out
end

function collected.backUp()
    local made, slot = {}, nil
    for _, each in ipairs(collected.backups()) do made[each.slot] = each end
    for s = 1, BACKUPS do
        if not made[s] then slot = s break end
        if not slot or made[s].at < made[slot].at then slot = s end
    end
    WriteFile(backupFile(slot), JSON_Encode({ at = GetOSTimeMS(), ids = collected.list() }))
    collected.backupVersion = collected.backupVersion + 1
end

return collected
