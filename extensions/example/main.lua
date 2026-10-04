metadata = {
    name = "Example",
    description = "A small extension to show how they're put together.",
    author = "genshin-overlay",
    version = "1.0.0",
}

local clicks = 0
local ws = WebSocket.new()
local connected = false


function RegisterSettings()
    DefineExtensionSetting("show_clicks", "Show click count", "bool", true)
end

function Init()
    print("Example extension loaded from " .. WORKING_DIR)
    
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

    print(ReadFile("example.txt"))
    WriteFile("example.txt", "hi?")
    print(ReadFile("example.txt"))
    AppendFile("example.txt", "\nhello")
    print(ReadFile("example.txt"))
    connected = ws:connect("wss://echo.websocket.org")

    if connected then
        print("WebSocket connected")
        ws:send("Hello from Genshin Overlay!")
    else
        print("WebSocket connection failed")
    end
end

function Menu()
    ImGUI_Button("Click me", function()
        clicks = clicks + 1
    end)

    if GetSetting("show_clicks") then
        ImGUI_Text("Clicked " .. clicks .. " times")
    end
end

function Update()
    if not connected then
        return
    end
    ws:send(string.format("Click count: %d", clicks))
    for _, message in ipairs(ws:pollAll()) do
        print("WebSocket received: " .. message)
    end
end

function Shutdown()
    ws:close()
    print("WebSocket closed")
end

