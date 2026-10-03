metadata = {
    name = "Example",
    description = "A small extension to show how they're put together.",
    author = "genshin-overlay",
    version = "1.0.0",
}

local clicks = 0

local response = Net_Get("https://example.com")

if response then
    print("GET response status:", response.status)
end

local response = Net_Post(
    "https://httpbin.org/post",
    '{"foo":"bar"}'
)
if response then
    print("POST response status:", response.status)
end


function RegisterSettings()
    DefineExtensionSetting("show_clicks", "Show click count", "bool", true)
end

function Init()
    print("Example extension loaded from " .. WORKING_DIR)
end

function Menu()
    ImGUI_Button("Click me", function()
        clicks = clicks + 1
    end)

    if GetSetting("show_clicks") then
        ImGUI_Text("Clicked " .. clicks .. " times")
    end
end
