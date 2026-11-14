metadata = {
    name = "Example",
    description = "A small extension to show how they're put together.",
    about = "Its menu has a button that counts clicks. It also tries a web request, a file and a WebSocket, and writes what happens to its log.",
    author = "genshin-overlay",
    version = "1.0.0",
}

local clicks = 0
local ws = WebSocket.new()


function RegisterSettings()
    DefineExtensionSetting("show_clicks", "Show click count", "bool", true)
end

function Init()
    print("Example extension loaded from " .. WORKING_DIR)
    
    Net_Start("https://example.com", function(response)
        print("GET response status:", response.status or response.error)
    end)

    Net_Start("https://httpbin.org/post", { method = "POST", body = '{"foo":"bar"}' }, function(response)
        print("POST response status:", response.status or response.error)
    end)

    print(ReadFile("example.txt"))
    WriteFile("example.txt", "hi?")
    print(ReadFile("example.txt"))
    AppendFile("example.txt", "\nhello")
    print(ReadFile("example.txt"))
    ws:connect("wss://echo.websocket.org", {
        onOpen = function()
            print("WebSocket connected")
            ws:send("Hello from Genshin Overlay!")
        end,
        onMessage = function(message)
            print("WebSocket received: " .. message)
        end,
        onClose = function(reason)
            print("WebSocket closed: " .. reason)
        end,
    })

    SetInterval(function()
        if ws:isConnected() then
            ws:send(string.format("Click count: %d", clicks))
        end
    end, 5000)
end

function Menu()
    ImGUI_Button("Click me", function()
        clicks = clicks + 1
    end)

    if GetSetting("show_clicks") then
        ImGUI_Text("Clicked " .. clicks .. " times")
    end
end

function Shutdown()
    ws:close()
end

