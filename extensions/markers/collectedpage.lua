-- The Collected page beside the list: a card per site to log in and keep it
-- in sync, or with sync off to get from it or send to it once; a file to
-- save to or load from; and the backups, the sites' and here, to restore.

local collected = require("collected")
local sync = require("sync")

local page = {}

local LABEL, plural = sync.LABEL, sync.plural

-- what's being asked about: { kind = "get" | "send" | "load", site, name, compare },
-- or { site, busy = true } while a site is read for it
local preview = nil

-- the backups per site, read again after a sync, a job from here, another
-- account or when the page opens (another extension may have made one)
local backups = {}
local localBackups = nil
local lastDrawn = 0
local function backupsOf(site)
    local key = tostring(sync.account(site)) .. ":" .. tostring(sync.last(site))
    local kept = backups[site]
    if not kept or kept.key ~= key then
        kept = { key = key, list = sync.account(site) and Marks_Backups(site) or {} }
        backups[site] = kept
    end
    return kept.list
end
function page.forgetBackups(site)
    backups[site] = nil
end

local function allowed(site)
    return PermissionState(sync.PERMISSION[site]) == "allowed"
end

local function ago(ms)
    local minutes = math.floor((GetOSTimeMS() - ms) / 60000)
    if minutes < 1 then return "just now" end
    if minutes < 60 then return minutes .. " min ago" end
    local hours = math.floor(minutes / 60)
    if hours < 24 then return plural(hours, "hour", "hours") .. " ago" end
    return plural(math.floor(hours / 24), "day", "days") .. " ago"
end

-- a site's line, in its card's header and in the list
local function siteLine(site)
    if not allowed(site) then return "Not allowed" end
    if not sync.account(site) then return "Not logged in" end
    local state = sync.now[site]
    if not sync.isOn(site) then return "Sync off" end
    if state.status then return state.status.text end
    local last = sync.last(site)
    return last and ("Synced " .. ago(last)) or "Sync on"
end

-- the Collected entry's line in the list
function page.summary()
    local last
    for _, site in ipairs(sync.SITES) do
        if sync.isOn(site) then
            local line = siteLine(site)
            if not line:find("^Synced") then return line end
            local at = sync.last(site)
            if not last or at < last then last = at end
        end
    end
    if last then return "Synced " .. ago(last) end
    return collected.count() .. " collected, sync off"
end

local function failed(code)
    Notice(sync.errorText(code), { kind = "error" })
end

local function fileName(path)
    return (path or ""):match("[^\\/]+$") or path
end

-- Get, Send and Load: what it would change and the buttons to do it
local function previewPart()
    local p = preview
    local id = "##preview" .. (p.site or "file")
    local c = p.compare
    local missing, extra = #c.missing, #c.extra
    -- the ones the site can't take, never counted as different
    local function cannotLine()
        local n = c.cannot and #c.cannot or 0
        if n == 0 then return end
        ImGUI_Text(n .. " of yours can't be marked on " .. LABEL[p.site] .. ": it doesn't have them, or doesn't let them be marked found.")
    end
    if missing == 0 and extra == 0 then
        local name = p.kind == "load" and fileName(p.name) or LABEL[p.site]
        ImGUI_Empty(name .. " has the same as here, nothing to change.")
        if p.site then cannotLine() end
        ImGUI_Button("OK" .. id, function() preview = nil end)
        return
    end
    -- an answer closes only its own question
    local function close() if preview == p then preview = nil end end
    if p.kind == "get" then
        local there = LABEL[p.site]
        ImGUI_Text(there .. " has " .. missing .. " marked that you haven't collected here. You have "
            .. extra .. " collected here that aren't marked there.")
        cannotLine()
        if missing > 0 then
            ImGUI_Button("Collect the " .. missing .. id, function()
                sync.take(c, false, "get")
                preview = nil
            end, { look = "main" })
            ImGUI_SameLine()
        end
        if extra > 0 then
            ImGUI_Button("Match " .. there .. " (unmarks " .. extra .. " here)" .. id, function()
                sync.take(c, true, "get")
                preview = nil
            end, { look = "risky" })
            ImGUI_SameLine()
        end
    elseif p.kind == "send" then
        local there = LABEL[p.site]
        ImGUI_Text("You have " .. extra .. " collected here that aren't marked on " .. there .. " yet. " .. there
            .. " has " .. missing .. " marked that you haven't collected here. A copy of " .. there
            .. "'s marks is saved first.")
        cannotLine()
        local function send(exactly)
            local site = p.site
            p.busy = true
            if not sync.send(site, c, exactly, function(r)
                close()
                page.forgetBackups(site)
                if not r.ok then failed(r.error) return end
                Notice(there .. ": " .. plural(r.added, "marked", "marked") .. ", " .. plural(r.removed, "unmarked", "unmarked") .. ".",
                    { kind = "done" })
            end) then
                preview = nil
                failed("failed")
            end
        end
        if p.busy then
            ImGUI_Waiting("Sending to " .. there .. "...")
            return
        end
        if extra > 0 then
            ImGUI_Button("Mark the " .. extra .. " there" .. id, function() send(false) end, { look = "main" })
            ImGUI_SameLine()
        end
        if missing > 0 then
            ImGUI_Button("Match my list (unmarks " .. missing .. " there)" .. id, function() send(true) end, { look = "risky" })
            ImGUI_SameLine()
        end
    else
        local name = fileName(p.name)
        ImGUI_Text(name .. " has " .. missing .. " you haven't collected here. You have " .. extra
            .. " collected here that aren't in it.")
        if missing > 0 then
            ImGUI_Button("Collect the " .. missing .. id, function()
                sync.take(c, false, "file")
                preview = nil
            end, { look = "main" })
            ImGUI_SameLine()
        end
        if extra > 0 then
            ImGUI_Button("Match the file (unmarks " .. extra .. " here)" .. id, function()
                sync.take(c, true, "file")
                preview = nil
            end, { look = "risky" })
            ImGUI_SameLine()
        end
    end
    ImGUI_Button("Cancel" .. id, function() preview = nil end)
end

-- Get and Send read the site first, then say what would change
local function readFor(kind, site)
    local asking = { site = site, busy = true }
    preview = asking
    if not sync.fetch(site, function(theirs, cannotOrError)
        if preview ~= asking then return end
        if not theirs then
            preview = nil
            failed(cannotOrError)
            return
        end
        preview = { kind = kind, site = site, compare = sync.compare(theirs, cannotOrError) }
    end, collected.list()) then
        preview = nil
        failed("failed")
    end
end

local function siteCard(site)
    local label = LABEL[site]
    if not ImGUI_Header(label, { open = true, summary = siteLine(site) }) then return end
    local id = "##" .. site
    if not allowed(site) then
        ImGUI_Empty("To sync, let Markers use your " .. label .. " account on its Permissions tab.")
        ImGUI_Button("Open Permissions" .. id, function() OpenPermissions(sync.PERMISSION[site]) end, { look = "main" })
        return
    end
    local a = GetAccount(site)
    if a.state == "window_open" then
        ImGUI_Waiting("Log in in the window that opened.")
        return
    end
    if a.state ~= "logged_in" then
        if a.problem and a.problem ~= "" then ImGUI_Empty(a.problem) end
        ImGUI_Button((a.state == "logged_out" and "Log in" or "Log in again") .. id, function() LogIn(site) end, { look = "main" })
        return
    end
    ImGUI_Text("Logged in as " .. ((a.name and a.name ~= "") and a.name or a.account))

    local on = sync.isOn(site)
    local now = ImGUI_Toggle("Keep in sync" .. id, on)
    if now ~= on then
        sync.setOn(site, now)
        if preview and preview.site == site then preview = nil end
    end
    if now then
        local state = sync.now[site]
        if state.status then ImGUI_Empty(state.status.text)
        elseif sync.last(site) then ImGUI_Empty("Synced " .. ago(sync.last(site)))
        end
        ImGUI_Button("Sync now" .. id, function() sync.full(site) end, { busy = sync.busy(site) and "Syncing..." or nil })
        return
    end

    if preview and preview.site == site then
        if preview.busy and not preview.compare then
            ImGUI_Waiting("Reading " .. label .. "...")
        else
            previewPart()
        end
        return
    end
    local busy = sync.busy(site)
    ImGUI_Button("Get from " .. label .. id, function() readFor("get", site) end, { busy = busy })
    ImGUI_SameLine()
    ImGUI_Button("Send to " .. label .. id, function() readFor("send", site) end, { busy = busy })
end

local FILTER = { "JSON", "*.json" }

local function fileCard()
    if not ImGUI_Header("File", { summary = "Save your collected list, or load one" }) then return end
    if preview and preview.kind == "load" then
        previewPart()
        return
    end
    ImGUI_Button("Save to a file##file", function()
        local n = collected.count()
        SaveFileDialog("collected.json", JSON_Encode(collected.list()), FILTER, function(r)
            if r.error then
                if r.error ~= "cancelled" then Notice("Couldn't save the file.", { kind = "error" }) end
                return
            end
            Notice("Saved " .. plural(n, "collected point", "collected points") .. " to " .. fileName(r.name) .. ".", { kind = "done" })
        end)
    end)
    ImGUI_SameLine()
    ImGUI_Button("Load a file##file", function()
        OpenFileDialog(FILTER, function(r)
            if r.error then
                if r.error == "too_big" then Notice("That file is too big.", { kind = "error" })
                elseif r.error ~= "cancelled" then Notice("Couldn't open the file.", { kind = "error" }) end
                return
            end
            local ok, list = pcall(JSON_Decode, r.data)
            local ids = ok and collected.whole(list) or {}
            if #ids == 0 then
                Notice(fileName(r.name) .. " has no collected list in it.", { kind = "error" })
                return
            end
            preview = { kind = "load", name = r.name, compare = sync.compare(sync.setOf(ids)) }
        end)
    end)
end

local function backupsCard()
    if not ImGUI_Header("Backups") then return end
    ImGUI_Empty("A copy of a site's marks is saved before every change. Restoring brings back marks that were lost, it never unmarks anything.")
    local any = false
    for _, site in ipairs(sync.SITES) do
        if allowed(site) and sync.account(site) then
            for _, b in ipairs(backupsOf(site)) do
                any = true
                ImGUI_Button("Restore##" .. site .. b.name, function()
                    if not sync.restore(site, b.name, function(r)
                        page.forgetBackups(site)
                        if not r.ok then failed(r.error) return end
                        local what = r.added == 0 and "nothing was missing" or (plural(r.added, "mark", "marks") .. " back")
                        local skipped = #(r.skipped or {})
                        if skipped > 0 then
                            what = what .. ", " .. skipped .. " couldn't be put back, the map items don't have them"
                        end
                        Notice("Restored " .. LABEL[site] .. " from " .. b.date .. ": " .. what .. ".", { kind = "done" })
                    end) then
                        failed("failed")
                    end
                end, { busy = sync.busy(site) })
                ImGUI_SameLine()
                ImGUI_Text(LABEL[site] .. ", " .. b.date .. ", " .. plural(b.count, "mark", "marks"))
            end
        end
    end
    -- here's own, made before something unmarked several here
    if not localBackups or localBackups.version ~= collected.backupVersion then
        localBackups = { version = collected.backupVersion, list = collected.backups() }
    end
    for _, b in ipairs(localBackups.list) do
        any = true
        ImGUI_Button("Restore##here" .. b.slot, function()
            local before = collected.count()
            collected.change(b.ids, {}, "file")
            local back = collected.count() - before
            local what = back == 0 and "nothing was missing" or (plural(back, "point", "points") .. " collected again")
            Notice("Restored your collected list from " .. ago(b.at) .. ": " .. what .. ".", { kind = "done" })
        end)
        ImGUI_SameLine()
        ImGUI_Text("Collected here, " .. ago(b.at) .. ", " .. plural(#b.ids, "point", "points"))
    end
    if not any then ImGUI_Empty("No backups yet.") end
end

function page.draw()
    -- opened again: the backups are read again
    if GetOSTimeMS() - lastDrawn > 1000 then backups, localBackups = {}, nil end
    lastDrawn = GetOSTimeMS()
    ImGUI_Text("Collected", { look = "title" })
    for _, site in ipairs(sync.SITES) do siteCard(site) end
    fileCard()
    backupsCard()
end

-- a log in, out or another account
function page.accountChanged(site)
    page.forgetBackups(site)
    if preview and preview.site == site then preview = nil end
end

return page
