-- Collected kept the same as HoYoLAB Map and appsample, each site by itself,
-- for the account logged in there. Every read and write goes through the
-- overlay's Marks_Start, which reads the site again first and makes the
-- backup.
--
-- The rule per point: when here and there agree that's it, otherwise the
-- side that changed since the last sync wins, both changed: here wins. The
-- base is what both had after the last sync, one per account; a site's
-- first sync for an account has none and only adds, both ways.

local collected = require("collected")

local sync = {}

sync.SITES = { "hoyolab_map", "appsample" }
sync.LABEL = { hoyolab_map = "HoYoLAB Map", appsample = "appsample" }
sync.PERMISSION = { hoyolab_map = "hoyolab_map_account", appsample = "appsample_account" }

local FILE = "sync.json"
-- taps go out this long after the last one
sync.SEND_AFTER = 3000

-- ---- the rule, on sets of ids (id = true) ----

local function union(a, b)
    local out = {}
    for id in pairs(a) do out[id] = true end
    for id in pairs(b) do out[id] = true end
    return out
end

-- ids in a and not in b, sorted
function sync.minus(a, b)
    local out = {}
    for id in pairs(a) do
        if not b[id] then out[#out + 1] = id end
    end
    table.sort(out)
    return out
end

function sync.setOf(list)
    local out = {}
    for _, id in ipairs(list or {}) do out[id] = true end
    return out
end

-- what both should have: here and there are now, base after the last sync,
-- nil for a first sync
function sync.decide(here, there, base)
    if not base then return union(here, there) end
    local final = {}
    local function one(id)
        local h, t, b = here[id] == true, there[id] == true, base[id] == true
        local keep
        if h == t then keep = h
        elseif h ~= b then keep = h
        else keep = t end
        if keep then final[id] = true end
    end
    for id in pairs(here) do one(id) end
    for id in pairs(there) do one(id) end
    for id in pairs(base) do one(id) end
    return final
end

-- ---- saved: per site on, last and a base per account ----

local saved = {}

local function blank()
    return { on = false, last = nil, bases = {} }
end

function sync.load()
    saved = {}
    local data = FileExists(FILE) and JSON_Decode(ReadFile(FILE))
    for _, site in ipairs(sync.SITES) do
        local s = type(data) == "table" and type(data[site]) == "table" and data[site] or {}
        local one = blank()
        one.on = s.on == true
        one.last = type(s.last) == "number" and s.last or nil
        for account, list in pairs(type(s.bases) == "table" and s.bases or {}) do
            if type(account) == "string" then one.bases[account] = sync.setOf(collected.whole(list)) end
        end
        saved[site] = one
    end
end

local function store()
    local data = {}
    for _, site in ipairs(sync.SITES) do
        local one = saved[site]
        local bases = {}
        for account, set in pairs(one.bases) do
            local list = sync.minus(set, {})
            bases[account] = list
        end
        data[site] = { on = one.on, last = one.last, bases = bases }
    end
    WriteFile(FILE, JSON_Encode(data))
end

function sync.isOn(site) return saved[site] and saved[site].on end
function sync.last(site) return saved[site] and saved[site].last end
function sync.base(site, account) return saved[site] and saved[site].bases[account] end

-- ---- what's going on now, not saved ----

-- per site: job (waiting for an answer), dirty (points changed here since
-- they were last sent), changedAt, syncedFor (the account synced since the
-- start), held and heldFor (a failed sync waits, for that account), round
-- (goes up with the switch, answers from an earlier round are dropped),
-- status { text, error }
local now = {}
for _, site in ipairs(sync.SITES) do now[site] = { dirty = {}, syncedFor = nil, round = 0 } end
sync.now = now

-- a point changed here: every other site with sync on sends it. dirty says
-- where it came from: "tap" for the player's own, "bulk" for a file, a Get
-- or the other site's sync
collected.onChange = function(id, on, from)
    for _, site in ipairs(sync.SITES) do
        if from ~= site and saved[site] and saved[site].on then
            now[site].dirty[id] = from == "tap" and "tap" or "bulk"
            now[site].changedAt = GetOSTimeMS()
        end
    end
end

local function account(site)
    local a = GetAccount(site)
    if a and a.state == "logged_in" and a.account then return a.account end
    return nil
end
sync.account = account

local ERRORS = {
    logged_out = "Not logged in.",
    captcha = "HoYoLAB Map wants a check first: log in again.",
    account_changed = "The account changed meanwhile, nothing was changed.",
    no_items = "The map items aren't there yet.",
    failed = "Couldn't reach it. Nothing was changed, try again later.",
    backup_failed = "The backup couldn't be saved, so nothing was changed.",
    cancelled = "Cancelled, nothing was changed.",
    no_backup = "That backup is gone or is another account's.",
}
function sync.errorText(code) return ERRORS[code] or "Something went wrong, nothing was changed." end

local function plural(n, one, many) return n .. " " .. (n == 1 and one or many) end
sync.plural = plural

-- starts a job for one site, its answer to done(result for that site); one
-- at a time per site, false while one runs or when the overlay refused it
local function start(site, action, add, remove, done)
    local state = now[site]
    if state.job then return false end
    local id = Marks_Start(site, action, add, remove, function(answer)
        state.job = nil
        done(answer[site] or { ok = false, error = "failed" })
    end)
    if id then state.job = id end
    return id ~= nil
end

local function finishBase(site, who, final, skipped)
    local base = {}
    for id in pairs(final) do
        if not skipped[id] then base[id] = true end
    end
    saved[site].bases[who] = base
    saved[site].last = GetOSTimeMS()
    store()
end

-- after a failed sync it's tried again after this long, or with Sync now
local RETRY_AFTER = 60000

-- A failed sync's points stay off the taps' list, the next full sync works
-- them out again from the base. Cancelled waits for Sync now or the next start.
local function failedFull(state, code)
    state.status = { text = sync.errorText(code), error = true }
    if code ~= "cancelled" then
        state.heldFor = state.syncedFor
        state.syncedFor = nil
        state.held = GetOSTimeMS()
    end
end

-- Reads the site, decides every point, then changes the site and here.
-- Here changes only once the site took it, and not for points changed here
-- meanwhile: those are sent next.
function sync.full(site)
    local state = now[site]
    local who = account(site)
    if not who or state.job then return false end
    local round = state.round
    -- the switch went off or on again, or another account came meanwhile
    local function gone() return state.round ~= round or account(site) ~= who end
    state.syncedFor = who
    state.status = { text = "Syncing..." }
    -- what was waiting is part of this sync now
    state.dirty = {}
    local started = start(site, "fetch", nil, nil, function(r)
        if gone() then return end
        if not r.ok then
            failedFull(state, r.error)
            return
        end
        local here = collected.copy()
        local there = sync.setOf(r.marks)
        local base = saved[site].bases[who]
        local first = base == nil
        local final = sync.decide(here, there, base)
        local add, remove = sync.minus(final, there), sync.minus(there, final)
        local function settle(result)
            local skipped = sync.setOf(result and result.skipped)
            -- unmarks the player said no to stay on both sides
            for _, id in ipairs(result and result.kept or {}) do final[id] = true end
            local get, drop = {}, {}
            for _, id in ipairs(sync.minus(final, here)) do
                if not state.dirty[id] then get[#get + 1] = id end
            end
            for _, id in ipairs(sync.minus(here, final)) do
                if not state.dirty[id] then drop[#drop + 1] = id end
            end
            if #drop > 1 then collected.backUp() end
            collected.change(get, drop, site)
            finishBase(site, who, final, skipped)
            local sent = result and result.added or 0
            if first then
                Notice("Synced with " .. sync.LABEL[site] .. " for the first time: " .. #get .. " collected here, "
                    .. sent .. " marked there. Nothing was unmarked.", { kind = "done" })
            end
            state.status = nil
        end
        if #add == 0 and #remove == 0 then
            settle(nil)
            return
        end
        if not start(site, "change", add, remove, function(result)
            if gone() then return end
            if not result.ok then
                failedFull(state, result.error)
                return
            end
            settle(result)
        end) then
            failedFull(state, "failed")
        end
    end)
    if not started then failedFull(state, "failed") end
    return started
end

-- Sends what changed here since the last sync: the marks in one job, the
-- player's own unmarks one per job, the unmarks from a file, a Get or the
-- other site in one job.
local function sendChanges(site, who)
    local state = now[site]
    local base = saved[site].bases[who]
    local add, bulk, tap = {}, {}, nil
    for id, from in pairs(state.dirty) do
        local here = collected.has(id)
        if here and not base[id] then add[#add + 1] = id
        elseif not here and base[id] then
            if from == "bulk" then bulk[#bulk + 1] = id
            elseif not tap then tap = id end
        else state.dirty[id] = nil end
    end
    local removing = #bulk > 0 and bulk or { tap }
    if #add == 0 and #removing == 0 then return end
    table.sort(add)
    table.sort(removing)
    local round = state.round
    local started = start(site, "change", add, removing, function(r)
        if state.round ~= round or account(site) ~= who then return end
        -- a full sync since gave the account a new base
        base = saved[site].bases[who]
        if not base then return end
        if not r.ok then
            state.status = { text = sync.errorText(r.error), error = true }
            -- cancelled waits for Sync now or the next start, which ask again
            if r.error == "cancelled" then return end
            -- sent again once a full sync went through
            state.heldFor = who
            state.syncedFor = nil
            state.held = GetOSTimeMS()
            return
        end
        local skipped = sync.setOf(r.skipped)
        for _, id in ipairs(add) do
            if not skipped[id] then base[id] = true end
        end
        for _, id in ipairs(removing) do base[id] = nil end
        local kept = {}
        for _, id in ipairs(r.kept or {}) do
            base[id] = true
            kept[#kept + 1] = id
        end
        collected.change(kept, {}, site)
        saved[site].last = GetOSTimeMS()
        store()
        state.status = nil
    end)
    -- a job that wasn't taken leaves them waiting, tried again SEND_AFTER later
    if not started then
        state.changedAt = GetOSTimeMS()
        return
    end
    for _, id in ipairs(add) do state.dirty[id] = nil end
    for _, id in ipairs(removing) do state.dirty[id] = nil end
end

-- every frame: a full sync once per account and start, taps after SEND_AFTER
function sync.update()
    local t = GetOSTimeMS()
    for _, site in ipairs(sync.SITES) do
        local state = now[site]
        if saved[site] and saved[site].on and not state.job then
            local who = account(site)
            if who then
                if state.syncedFor ~= who then
                    if not state.held or t - state.held >= RETRY_AFTER then
                        state.held = nil
                        sync.full(site)
                    end
                elseif saved[site].bases[who] and next(state.dirty) and state.changedAt and t - state.changedAt >= sync.SEND_AFTER then
                    sendChanges(site, who)
                end
            end
        end
    end
end

-- the player's switch; off drops the site's bases, so turning it on again
-- is a first sync, which only adds
function sync.setOn(site, on)
    local one = saved[site]
    if one.on == on then return end
    one.on = on
    one.bases = {}
    local state = now[site]
    state.round = state.round + 1
    state.dirty = {}
    state.syncedFor = nil
    state.held = nil
    state.status = nil
    store()
end

-- A log in, out or another account: synced again. A failed sync still
-- waits for the same account, the overlay tells a captcha or a new problem
-- this way too.
function sync.accountChanged(site)
    local state = now[site]
    if not state then return end
    state.syncedFor = nil
    if state.held and account(site) == state.heldFor then return end
    state.held = nil
    state.status = nil
end

function sync.busy(site) return now[site].job ~= nil end

-- ---- one way, with sync off ----

-- reads the site: done(set of its marks, set of asked ones it can't take)
-- or done(nil, error)
function sync.fetch(site, done, asked)
    return start(site, "fetch", asked, nil, function(r)
        if not r.ok then done(nil, r.error) return end
        done(sync.setOf(r.marks), sync.setOf(r.skipped or {}))
    end)
end

-- what a list from somewhere (a site, a file) would change here
-- cannot: the ones here the site can't take, left out of extra
function sync.compare(theirs, cannot)
    local here = collected.copy()
    local extra, left = {}, {}
    for _, id in ipairs(sync.minus(here, theirs)) do
        if cannot and cannot[id] then left[#left + 1] = id else extra[#extra + 1] = id end
    end
    return { theirs = theirs, missing = sync.minus(theirs, here), extra = extra, cannot = left }
end

-- here gets what they have; exactly also unmarks what they don't
function sync.take(compare, exactly, from)
    local drop = exactly and compare.extra or {}
    if #drop > 1 then collected.backUp() end
    collected.change(compare.missing, drop, from)
end

-- the site gets what's here; exactly also unmarks there what isn't (the
-- overlay asks first when that's several)
function sync.send(site, compare, exactly, done)
    return start(site, "change", compare.extra, exactly and compare.missing or {}, done)
end

function sync.restore(site, name, done)
    return start(site, "restore", name, nil, function(r)
        done(r)
        -- with sync on, what came back comes here too
        if r.ok and r.added > 0 and saved[site].on then now[site].syncedFor = nil end
    end)
end

return sync
