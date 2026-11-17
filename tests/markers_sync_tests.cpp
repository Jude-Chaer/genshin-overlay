#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <set>
#include <string>
#include <sol/sol.hpp>

// The Markers extension's sync (extensions/markers/sync.lua): collected kept
// the same as HoYoLAB Map and appsample. The sites here are stand-ins that
// keep marks like the overlay's Marks_Start gives them, by HoYoLAB id. The
// ids are real HoYoLAB points: 56, 58 and 59 from HoYoLAB's own answer in
// tests/data/marks, 17631 and 33158 from the test item files; 4242 isn't in
// the item files, so no site takes it.

namespace fs = std::filesystem;

namespace {
    const fs::path MARKERS = fs::path(__FILE__).parent_path().parent_path() / "extensions" / "markers";

    struct Sync {
        sol::state lua;
        Sync() {
            lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::package, sol::lib::math);
            lua["package"]["path"] = (MARKERS / "?.lua").string();
            lua.script(R"(
                -- the extension's folder, files kept as tables
                files = {}
                local function copy(value)
                    if type(value) ~= "table" then return value end
                    local out = {}
                    for k, v in pairs(value) do out[k] = copy(v) end
                    return out
                end
                FileExists = function(name) return files[name] ~= nil end
                ReadFile = function(name) return files[name] end
                WriteFile = function(name, data) files[name] = data return true end
                JSON_Encode = copy
                JSON_Decode = copy
                clock = 0
                GetOSTimeMS = function() return clock end
                notices = {}
                Notice = function(text) notices[#notices + 1] = text end

                -- the sites, by HoYoLAB id; cant holds what a site skips
                sites = { hoyolab_map = {}, appsample = {} }
                cant = { hoyolab_map = { [4242] = true }, appsample = { [4242] = true, [56] = true } }
                accounts = { hoyolab_map = "hoyo1", appsample = "uid1" }
                GetAccount = function(site)
                    local a = accounts[site]
                    return { site = site, state = a and "logged_in" or "logged_out", account = a }
                end
                -- what the overlay's question answers when a change unmarks several: "unmark", "keep" or "cancel"
                answer = "unmark"
                asked = 0
                jobs, mostRemoves = {}, 0
                Marks_Start = function(site, action, add, remove, onDone)
                    jobs[#jobs + 1] = { site = site, action = action, add = add, remove = remove, onDone = onDone }
                    return #jobs
                end
                -- full: the overlay refuses new jobs
                full = false
                local start = Marks_Start
                Marks_Start = function(...) if full then return nil end return start(...) end
                -- the worker: runs every waiting job, and the ones they start
                function runJobs()
                    while #jobs > 0 do
                        local job = table.remove(jobs, 1)
                        local marks, r = sites[job.site], { ok = true, added = 0, removed = 0, kept = {}, skipped = {} }
                        if job.action == "fetch" then
                            r.marks = {}
                            for id in pairs(marks) do r.marks[#r.marks + 1] = id end
                            table.sort(r.marks)
                        else
                            local remove = {}
                            for _, id in ipairs(job.remove or {}) do
                                if marks[id] then remove[#remove + 1] = id end
                            end
                            if #remove > mostRemoves then mostRemoves = #remove end
                            if #remove > 1 then
                                asked = asked + 1
                                if answer == "cancel" then r = { ok = false, error = "cancelled" } end
                                if answer == "keep" then r.kept, remove = remove, {} end
                            end
                            if r.ok then
                                for _, id in ipairs(job.add or {}) do
                                    if cant[job.site][id] then r.skipped[#r.skipped + 1] = id
                                    elseif not marks[id] then marks[id] = true r.added = r.added + 1 end
                                end
                                for _, id in ipairs(remove) do marks[id] = nil r.removed = r.removed + 1 end
                            end
                        end
                        job.onDone({ [job.site] = r })
                    end
                end
                function has(site, list)
                    sites[site] = {}
                    for _, id in ipairs(list) do sites[site][id] = true end
                end
                collected = require("collected")
                sync = require("sync")
                collected.load()
                sync.load()
                function here(list)
                    collected.change(list, {}, "file")
                end
                -- an overlay start: what's saved is read again
                function restart()
                    package.loaded.collected, package.loaded.sync = nil, nil
                    collected = require("collected")
                    sync = require("sync")
                    collected.load()
                    sync.load()
                end
                function step(ms)
                    clock = clock + (ms or 16)
                    sync.update()
                    runJobs()
                end
            )");
        }
        void run(const std::string& code) { lua.script(code); }
        std::set<int> set(const std::string& expression) {
            std::set<int> out;
            sol::table table = lua.script("return " + expression);
            for (auto& [key, value] : table) {
                if (value.get_type() == sol::type::boolean) out.insert(key.as<int>());
                else out.insert(value.as<int>());
            }
            return out;
        }
        std::set<int> collected() { return set("collected.list()"); }
        std::set<int> site(const std::string& name) { return set("sites." + name); }
        int number(const std::string& expression) { return lua.script("return " + expression); }
        std::string text(const std::string& expression) { return lua.script("return " + expression); }
    };
}

TEST_CASE("sync's rule: the side that changed since the last sync wins, a first sync only adds") {
    Sync s;
    // first sync: everything from both
    s.run("final = sync.decide(sync.setOf({ 56, 58 }), sync.setOf({ 58, 59 }), nil)");
    CHECK(s.set("final") == std::set<int>{ 56, 58, 59 });
    // since base { 56, 58, 59 }: 56 unmarked here, 59 unmarked there, 17631 marked there
    s.run("final = sync.decide(sync.setOf({ 58, 59 }), sync.setOf({ 56, 58, 17631 }), sync.setOf({ 56, 58, 59 }))");
    CHECK(s.set("final") == std::set<int>{ 58, 17631 });
    // both unmarked 58 and both marked 33158: they agree
    s.run("final = sync.decide(sync.setOf({ 56, 33158 }), sync.setOf({ 56, 33158 }), sync.setOf({ 56, 58 }))");
    CHECK(s.set("final") == std::set<int>{ 56, 33158 });
    // nothing changed anywhere
    s.run("final = sync.decide(sync.setOf({ 56 }), sync.setOf({ 56 }), sync.setOf({ 56 }))");
    CHECK(s.set("final") == std::set<int>{ 56 });
}

TEST_CASE("the first sync for an account adds both ways, unmarks nothing and says so") {
    Sync s;
    s.run("here({ 56, 58 }) has('hoyolab_map', { 58, 59 }) sync.setOn('hoyolab_map', true) step()");
    CHECK(s.collected() == std::set<int>{ 56, 58, 59 });
    CHECK(s.site("hoyolab_map") == std::set<int>{ 56, 58, 59 });
    CHECK(s.text("notices[1]") == "Synced with HoYoLAB Map for the first time: 1 collected here, 1 marked there. Nothing was unmarked.");
    CHECK(s.set("sync.base('hoyolab_map', 'hoyo1')") == std::set<int>{ 56, 58, 59 });
    // appsample is off and isn't touched
    CHECK(s.site("appsample").empty());
    CHECK(s.number("#notices") == 1);
}

TEST_CASE("after a restart each side's changes since the last sync go to the other") {
    Sync s;
    s.run("here({ 56, 58, 59 }) has('hoyolab_map', { 56, 58, 59 }) sync.setOn('hoyolab_map', true) step()");
    // with the overlay closed: 56 unmarked here, and on HoYoLAB 59 unmarked and 17631 marked
    s.run("restart() collected.change({}, { 56 }, 'file') has('hoyolab_map', { 56, 58, 17631 }) step()");
    CHECK(s.collected() == std::set<int>{ 58, 17631 });
    CHECK(s.site("hoyolab_map") == std::set<int>{ 58, 17631 });
    CHECK(s.set("sync.base('hoyolab_map', 'hoyo1')") == std::set<int>{ 58, 17631 });
    // one unmark each way: nobody was asked
    CHECK(s.number("asked") == 0);
}

TEST_CASE("a sync that unmarks several on a site goes through the overlay's question") {
    Sync s;
    s.run("here({ 56, 58, 59 }) has('hoyolab_map', { 56, 58, 59 }) sync.setOn('hoyolab_map', true) step()");
    s.run("restart() collected.change({}, { 56, 58 }, 'file')");

    SECTION("keep them marked: they stay collected here too") {
        s.run("answer = 'keep' step()");
        CHECK(s.number("asked") == 1);
        CHECK(s.site("hoyolab_map") == std::set<int>{ 56, 58, 59 });
        CHECK(s.collected() == std::set<int>{ 56, 58, 59 });
        CHECK(s.set("sync.base('hoyolab_map', 'hoyo1')") == std::set<int>{ 56, 58, 59 });
    }
    SECTION("cancel: nothing changes on either side, and it isn't sent again on its own") {
        s.run("answer = 'cancel' step()");
        CHECK(s.site("hoyolab_map") == std::set<int>{ 56, 58, 59 });
        CHECK(s.collected() == std::set<int>{ 59 });
        CHECK(s.text("sync.now.hoyolab_map.status.text") == "Cancelled, nothing was changed.");
        s.run("step(120000) step(3000) step()");
        CHECK(s.number("asked") == 1);
        CHECK(s.site("hoyolab_map") == std::set<int>{ 56, 58, 59 });
    }
}

TEST_CASE("turned off during a sync: neither side changes and no base is kept") {
    Sync s;
    s.run("here({ 56 }) has('hoyolab_map', { 58 }) sync.setOn('hoyolab_map', true) clock = clock + 16 sync.update()");
    // the read is on its way
    s.run("sync.setOn('hoyolab_map', false) runJobs()");
    CHECK(s.collected() == std::set<int>{ 56 });
    CHECK(s.site("hoyolab_map") == std::set<int>{ 58 });
    CHECK(s.lua.script("return sync.base('hoyolab_map', 'hoyo1') == nil and sync.last('hoyolab_map') == nil").get<bool>());
}

TEST_CASE("a sync the overlay won't start says so and is tried again later") {
    Sync s;
    s.run("here({ 56 }) has('hoyolab_map', { 58 }) full = true sync.setOn('hoyolab_map', true) step()");
    CHECK(s.text("sync.now.hoyolab_map.status.text") == "Couldn't reach it. Nothing was changed, try again later.");
    s.run("full = false step(1000)");
    CHECK(s.site("hoyolab_map") == std::set<int>{ 58 });
    // the overlay telling the same account again doesn't skip the wait
    s.run("sync.accountChanged('hoyolab_map') step(1000)");
    CHECK(s.site("hoyolab_map") == std::set<int>{ 58 });
    s.run("step(60000)");
    CHECK(s.site("hoyolab_map") == std::set<int>{ 56, 58 });
    CHECK(s.collected() == std::set<int>{ 56, 58 });
}

TEST_CASE("here's backups: the newest 5 are kept, each one only adds back") {
    Sync s;
    for (int i = 1; i <= 6; i++) s.run("clock = " + std::to_string(i * 1000) + " here({ " + std::to_string(i) + " }) collected.backUp()");
    s.run("list = collected.backups()");
    CHECK(s.number("#list") == 5);
    // the first, with only 1 in it, was written over
    CHECK(s.number("list[1].at") == 6000);
    CHECK(s.number("list[5].at") == 2000);
    CHECK(s.set("list[5].ids") == std::set<int>{ 1, 2 });
}

TEST_CASE("each account has its own base: another account's first sync only adds") {
    Sync s;
    s.run("here({ 56, 58 }) has('hoyolab_map', { 56, 58 }) sync.setOn('hoyolab_map', true) step()");
    // another account, with other marks
    s.run("accounts.hoyolab_map = 'hoyo2' has('hoyolab_map', { 59 }) sync.accountChanged('hoyolab_map') step()");
    CHECK(s.collected() == std::set<int>{ 56, 58, 59 });
    CHECK(s.site("hoyolab_map") == std::set<int>{ 56, 58, 59 });
    CHECK(s.number("#notices") == 2);
    // the first account's base is kept for when it comes back
    CHECK(s.set("sync.base('hoyolab_map', 'hoyo1')") == std::set<int>{ 56, 58 });
    CHECK(s.set("sync.base('hoyolab_map', 'hoyo2')") == std::set<int>{ 56, 58, 59 });
}

TEST_CASE("a point a site can't hold stays collected here and isn't unmarked by the next sync") {
    Sync s;
    // 56 has no appsample pair, 4242 isn't in the item files
    s.run("here({ 56, 4242, 17631 }) has('appsample', { 33158 }) sync.setOn('appsample', true) step()");
    CHECK(s.site("appsample") == std::set<int>{ 17631, 33158 });
    CHECK(s.collected() == std::set<int>{ 56, 4242, 17631, 33158 });
    CHECK(s.set("sync.base('appsample', 'uid1')") == std::set<int>{ 17631, 33158 });
    s.run("restart() step()");
    CHECK(s.collected() == std::set<int>{ 56, 4242, 17631, 33158 });
    CHECK(s.site("appsample") == std::set<int>{ 17631, 33158 });
}

TEST_CASE("taps go out 3 s after the last one, unmarks one per job") {
    Sync s;
    s.run("here({ 56, 58, 59 }) has('hoyolab_map', { 56, 58, 59 }) sync.setOn('hoyolab_map', true) step()");
    s.run("mostRemoves = 0 collected.set(56, false) step(1000) collected.set(58, false) collected.set(17631, true) step(2000)");
    // 2 s after the last tap: nothing sent yet
    CHECK(s.site("hoyolab_map") == std::set<int>{ 56, 58, 59 });
    s.run("step(1000) step() step()");
    CHECK(s.site("hoyolab_map") == std::set<int>{ 59, 17631 });
    CHECK(s.number("mostRemoves") == 1);
    CHECK(s.number("asked") == 0);
    CHECK(s.set("sync.base('hoyolab_map', 'hoyo1')") == std::set<int>{ 59, 17631 });
}

TEST_CASE("unmarks from a file go to a synced site together, so several are asked about") {
    Sync s;
    s.run("here({ 56, 58, 59 }) has('hoyolab_map', { 56, 58, 59 }) sync.setOn('hoyolab_map', true) step()");
    s.run("sync.take(sync.compare(sync.setOf({ 59 })), true, 'file')");
    CHECK(s.collected() == std::set<int>{ 59 });

    SECTION("unmark") {
        s.run("step(3000) step()");
        CHECK(s.number("asked") == 1);
        CHECK(s.site("hoyolab_map") == std::set<int>{ 59 });
        CHECK(s.set("sync.base('hoyolab_map', 'hoyo1')") == std::set<int>{ 59 });
    }
    SECTION("cancel: the site keeps them and isn't asked again until the next sync") {
        s.run("answer = 'cancel' step(3000) step(120000) step(3000)");
        CHECK(s.number("asked") == 1);
        CHECK(s.site("hoyolab_map") == std::set<int>{ 56, 58, 59 });
        CHECK(s.text("sync.now.hoyolab_map.status.text") == "Cancelled, nothing was changed.");
        // the next start asks again
        s.run("restart() step()");
        CHECK(s.number("asked") == 2);
    }
}

TEST_CASE("a change from one site's sync goes on to the other site") {
    Sync s;
    s.run("here({ 58 }) has('hoyolab_map', { 58 }) has('appsample', { 58 }) sync.setOn('hoyolab_map', true) sync.setOn('appsample', true) step()");
    // marked on HoYoLAB with the overlay closed
    s.run("restart() has('hoyolab_map', { 58, 33158 }) step() step(3000) step()");
    CHECK(s.collected() == std::set<int>{ 58, 33158 });
    CHECK(s.site("appsample") == std::set<int>{ 58, 33158 });
}

TEST_CASE("turning sync off forgets the bases, so turning it on again only adds") {
    Sync s;
    s.run("here({ 56, 58 }) has('hoyolab_map', { 56, 58 }) sync.setOn('hoyolab_map', true) step()");
    s.run("sync.setOn('hoyolab_map', false) collected.change({}, { 56 }, 'file')");
    CHECK(s.lua.script("return sync.base('hoyolab_map', 'hoyo1') == nil").get<bool>());
    s.run("sync.setOn('hoyolab_map', true) step()");
    // 56 isn't unmarked there, it comes back here
    CHECK(s.collected() == std::set<int>{ 56, 58 });
    CHECK(s.site("hoyolab_map") == std::set<int>{ 56, 58 });
}

TEST_CASE("logged out, nothing is sent; after logging in the taps go with the sync") {
    Sync s;
    s.run("here({ 56 }) has('hoyolab_map', { 56 }) sync.setOn('hoyolab_map', true) step()");
    s.run("accounts.hoyolab_map = nil sync.accountChanged('hoyolab_map') collected.set(58, true) step(5000)");
    CHECK(s.site("hoyolab_map") == std::set<int>{ 56 });
    s.run("accounts.hoyolab_map = 'hoyo1' sync.accountChanged('hoyolab_map') step() step(3000)");
    CHECK(s.site("hoyolab_map") == std::set<int>{ 56, 58 });
}

TEST_CASE("one way with sync off: Get, Send and a file say what they'd change, exactly also unmarks") {
    Sync s;
    s.run("here({ 56, 58 }) has('hoyolab_map', { 58, 59, 17631 })");
    s.run("sync.fetch('hoyolab_map', function(theirs) cmp = sync.compare(theirs) end) runJobs()");
    CHECK(s.set("cmp.missing") == std::set<int>{ 59, 17631 });
    CHECK(s.set("cmp.extra") == std::set<int>{ 56 });

    SECTION("Get, collect the missing ones") {
        s.run("sync.take(cmp, false, 'get')");
        CHECK(s.collected() == std::set<int>{ 56, 58, 59, 17631 });
    }
    SECTION("Get, match: 56 unmarked here") {
        s.run("sync.take(cmp, true, 'get')");
        CHECK(s.collected() == std::set<int>{ 58, 59, 17631 });
    }
    SECTION("Send, mark there") {
        s.run("sync.send('hoyolab_map', cmp, false, function(r) sent = r end) runJobs()");
        CHECK(s.site("hoyolab_map") == std::set<int>{ 56, 58, 59, 17631 });
        CHECK(s.number("sent.added") == 1);
    }
    SECTION("Send, match my list: the two unmarks there are asked first") {
        s.run("sync.send('hoyolab_map', cmp, true, function(r) sent = r end) runJobs()");
        CHECK(s.number("asked") == 1);
        CHECK(s.site("hoyolab_map") == std::set<int>{ 56, 58 });
    }
    SECTION("a file, match: several unmarked here, collected is copied first") {
        s.run("here({ 59, 33158 }) cmp = sync.compare(sync.setOf({ 58 })) sync.take(cmp, true, 'file')");
        CHECK(s.collected() == std::set<int>{ 58 });
        CHECK(s.set("files['collected-backup-1.json'].ids") == std::set<int>{ 56, 58, 59, 33158 });
    }
    // with sync off nothing is kept as a base
    CHECK(s.lua.script("return sync.base('hoyolab_map', 'hoyo1') == nil").get<bool>());
}

namespace {
    // the Collected page drawn by stand-ins that write down what it shows; a
    // button named in press is pressed
    struct Page : Sync {
        Page() {
            run(R"(
                perms = { hoyolab_map_account = "allowed", appsample_account = "not_asked" }
                PermissionState = function(name) return perms[name] end
                Marks_Backups = function() return {} end
                shown, press = {}, nil
                local function show(text) shown[#shown + 1] = text end
                ImGUI_Header = function(label, o) show("# " .. label .. (o and o.summary and " | " .. o.summary or "")) return true end
                ImGUI_Text = function(text) show(text) end
                ImGUI_Empty = function(text) show(text) end
                ImGUI_Waiting = function(text) show(text) end
                ImGUI_SameLine = function() end
                ImGUI_Toggle = function(label, on) show((on and "[x] " or "[ ] ") .. label) return on end
                ImGUI_Button = function(label, fn)
                    show("[" .. label .. "]")
                    if press == label then press = nil fn() end
                end
                page = require("collectedpage")
                function draw()
                    shown = {}
                    page.draw()
                    return table.concat(shown, "\n")
                end
            )");
        }
        std::string draw() { return lua.script("return draw()"); }
    };
}

TEST_CASE("the Collected page: Get says what changes, Collect takes only what's missing") {
    Page p;
    p.run("here({ 56, 58 }) has('hoyolab_map', { 58, 59, 17631 })");
    std::string page = p.draw();
    CHECK(page.find("# HoYoLAB Map | Sync off") != std::string::npos);
    CHECK(page.find("Logged in as hoyo1") != std::string::npos);
    CHECK(page.find("# appsample | Not allowed") != std::string::npos);
    CHECK(page.find("[Open Permissions##appsample]") != std::string::npos);
    p.run("press = 'Get from HoYoLAB Map##hoyolab_map' draw() runJobs()");
    page = p.draw();
    CHECK(page.find("HoYoLAB Map has 2 marked that you haven't collected here. You have 1 collected here that aren't marked there.")
        != std::string::npos);
    CHECK(page.find("[Match HoYoLAB Map (unmarks 1 here)##previewhoyolab_map]") != std::string::npos);
    p.run("press = 'Collect the 2##previewhoyolab_map' draw()");
    CHECK(p.collected() == std::set<int>{ 56, 58, 59, 17631 });
    CHECK(p.draw().find("[Get from HoYoLAB Map##hoyolab_map]") != std::string::npos);
    CHECK(p.text("page.summary()") == "4 collected, sync off");
}

TEST_CASE("the Collected page: a backup of here is listed and restoring it only adds") {
    Page p;
    p.run("here({ 56, 58, 59 }) collected.backUp() collected.change({ 17631 }, { 56, 58 }, 'file')");
    std::string page = p.draw();
    CHECK(page.find("Collected here, just now, 3 points") != std::string::npos);
    p.run("press = 'Restore##here1' draw()");
    CHECK(p.collected() == std::set<int>{ 56, 58, 59, 17631 });
    CHECK(p.text("notices[#notices]") == "Restored your collected list from just now: 2 points collected again.");
}

TEST_CASE("the Collected page: with sync on, the switch, Sync now and the line") {
    Page p;
    p.run("here({ 56 }) has('hoyolab_map', { 56 }) sync.setOn('hoyolab_map', true) step()");
    std::string page = p.draw();
    CHECK(page.find("[x] Keep in sync##hoyolab_map") != std::string::npos);
    CHECK(page.find("Synced just now") != std::string::npos);
    CHECK(page.find("[Sync now##hoyolab_map]") != std::string::npos);
    CHECK(page.find("[Get from") == std::string::npos);
    p.run("clock = clock + 5 * 60000");
    CHECK(p.text("page.summary()") == "Synced 5 min ago");
    p.run("accounts.hoyolab_map = nil");
    CHECK(p.text("page.summary()") == "Not logged in");
    CHECK(p.draw().find("[Log in##hoyolab_map]") != std::string::npos);
}
