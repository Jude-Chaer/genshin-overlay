#pragma once
#include <GL/glew.h>
#include <sol/sol.hpp>
#include <memory>
#include <string>
#include <tuple>
#include "keybindings.hpp"
#include "net.hpp"
#include "extensions.hpp"
#include "controls.hpp"

namespace ItemCatalog { class Catalog; }
namespace ItemFiles { struct Status; }

namespace LuaFunctions {
    // Every function an extension can call, each with the permission it needs.
    // A call without it does nothing and gives nil (false for yes/no ones),
    // with one log line per function per extension.
    void registerLuaFunctions(sol::state& luaState);
    // the name and permission of each function registered above, as
    // "Net_Get" or "WebSocket.connect", for the tests
    struct Bound {
        std::string name;
        Extensions::ExtensionPermissionTypes permission;
    };
    const std::vector<Bound>& boundFunctions();
    // the registry table where each bound function sits under its name, so a test can
    // tell a global that came through bind from one that didn't
    inline const char* BOUND_TABLE = "overlayBoundFunctions";

    struct LuaInterval
    {
        int id;
        int milliseconds;
        double nextExecution;
        Extensions::Extension* owner;
        sol::protected_function callback;
    };
    extern std::vector<LuaInterval> intervals;
    // removes the extension's intervals, see Extensions::unload
    void forgetIntervals(Extensions::Extension* owner);

    void ImGUI_Text(const std::string& str);
    void ImGUI_Button(const std::string& label, sol::protected_function callback, sol::optional<sol::table> options);
    void ImGUI_Image(GLuint textureID, float width, float height);
    bool ImGUI_Checkbox(const std::string& label, bool currentValue);
    // a muted line with a spinner, for something under way
    void ImGUI_Waiting(const std::string& text);
    // a muted line for nothing there yet
    void ImGUI_Empty(const std::string& text);
    // the text and a bar, fraction 0 to 1, nil when it isn't known
    void ImGUI_Progress(const std::string& text, sol::optional<float> fraction);
    float ImGUI_SliderFloat(const std::string& label, float currentValue, float minValue, float maxValue);
    int ImGUI_SliderInt(const std::string& label, int currentValue, int minValue, int maxValue);
    double ImGUI_Number(const std::string& label, double currentValue, sol::optional<sol::table> options);
    bool ImGUI_Toggle(const std::string& label, bool currentValue, sol::optional<sol::table> options);
    std::string ImGUI_InputText(const std::string& label, const std::string& currentText, sol::optional<sol::table> options);
    void ImGUI_SameLine();
    void ImGUI_Tooltip(const std::string& text);
    void ImGUI_Separator();
    bool IsProgramHidden();
    std::string RegisterKeybind(int glfwKey, sol::protected_function callbackFunction, Keybindings::KeybindFlags flags, sol::optional<int> modifiers);
    bool DeleteKeybind(const std::string& uuid);
    void DefineExtensionSetting(const std::string& id, const std::string& label, const std::string& type, sol::object defaultValue,
        sol::object minOrOptions, sol::optional<float> max);
    sol::object GetExtensionSetting(const std::string& id, sol::this_state state);
    sol::variadic_results LoadTextureFromFileLua(const std::string& path, sol::this_state state);
    sol::object GetMapView(sol::this_state state);
    void FollowMapWhileMoving();
    sol::object GetPlayerPosition(sol::this_state state);
    sol::object GetMapGrid(int mapId, sol::this_state state);
    GLuint GetMapTile(int mapId, int x, int y, int size);
    uint64_t GetItemsVersion();
    sol::object GetItemsState(sol::this_state state);
    sol::object GetItemTypes(sol::this_state state);
    sol::object GetItemPositions(int type, sol::this_state state);
    sol::object GetItemRegions(sol::this_state state);
    sol::object GetItemIcon(int type, sol::this_state state);
    sol::object GetMapFloor(int groupId, int floorId, sol::this_state state);
    void DrawImage(GLuint textureID, float x0, float y0, float x1, float y1, sol::optional<float> alpha);
    void GDrawText(const std::string& text, float x, float y, sol::optional<float> size, sol::optional<uint32_t> color); // The "G" is only there because DrawText is already used by the Windows API
    // the overlay's colours (0xRRGGBBAA) and sizes (pixels, from the font
    // size now) in the player's look, see Controls::Theme
    sol::table GetTheme(sol::this_state state);
    sol::table themeTable(const Controls::Theme& theme, sol::state_view lua);
    void PushClipRect(float x0, float y0, float x1, float y1);
    void PopClipRect();
    // Net_Get and Net_Post wait for the answer, at most 5 s. Net_Start doesn't:
    // Net_Start(url, options, onDone) gives an id, the request runs on its own
    // thread. options: method ("GET" or "POST") and body. The answer is
    // { status, body }, or { error } with "timeout", "too_big", "refused" or
    // "failed". onDone gets it once, in the frame after it came; without
    // onDone Net_Result(id) gives nil while it waits, then the answer once.
    // Each body is 16 MB at most, Net_Start's whole request 30 s.
    sol::object NetGet(const std::string& url, sol::this_state state);
    sol::object NetPost(const std::string& url, const std::string& data, sol::this_state state);
    sol::object NetStart(const std::string& url, sol::object options, sol::object onDone, sol::this_state state);
    sol::object NetResult(int id, sol::this_state state);
    inline const size_t NetMaxBody = 16 * 1024 * 1024;
    inline const int NetWaitMs = 5000;
    inline const int NetStartMs = 30000;
    // requests of one extension waiting or not collected yet
    inline const int NetMaxWaiting = 16;
    // each turn of the overlay, drawn or not: web requests and
    // sockets give what came to their handlers
    void UpdateAnswers();
    // gives the requests that are done to their onDone, see UpdateAnswers
    void UpdateNetRequests();
    // stops the extension's requests and drops their answers, see Extensions::unload
    void forgetNetRequests(Extensions::Extension* owner);
    // its requests waiting or not collected, for the tests
    int netRequestsOf(const Extensions::Extension* owner);
    sol::object JSONEncode(sol::object value, sol::this_state state);
    sol::object JSONDecode(const std::string& json, sol::this_state state);
	sol::optional<std::string> ReadFile(const std::string& path);
	bool WriteFile(const std::string& path, const std::string& contents);
	bool AppendFile(const std::string& path, const std::string& contents);
    bool FileExists(const std::string& path);
    sol::object ListDirectory(const std::string& path, sol::this_state state);
    // An extension's web socket, WebSocket.new() in Lua. It runs on its own
    // thread: connect(url, handlers) gives true once it's trying and returns at
    // once. handlers is optional: onOpen(), onMessage(message) and
    // onClose(reason), each called in a frame, reason "closed", "refused",
    // "timeout", "too_many_messages" or "failed". Without onMessage, poll and
    // pollAll take the messages. send queues, also while connecting; close
    // asks it to end. Each way holds 1000 messages at most.
    struct SocketLink;
    struct Socket {
        std::shared_ptr<SocketLink> link;
        Socket() = default;
        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;
        ~Socket();
    };
    bool WebSocketConnect(Socket& socket, const std::string& url, sol::optional<sol::table> handlers);
    bool WebSocketSend(Socket& socket, const std::string& message);
    sol::object WebSocketPoll(Socket& socket, sol::this_state state);
    sol::object WebSocketPollAll(Socket& socket, sol::this_state state);
    bool WebSocketIsConnected(Socket& socket);
    void WebSocketClose(Socket& socket);
    inline const size_t SocketMaxQueued = 1000;
    // sockets of one extension open or connecting
    inline const int SocketMax = 16;
    // calls the handlers and lets go of ended sockets, see UpdateAnswers
    void UpdateSockets();
    // ends the extension's sockets without calling it again, see Extensions::unload
    void forgetSockets(Extensions::Extension* owner);
    // its sockets not let go of yet, for the tests
    int socketsOf(const Extensions::Extension* owner);
	double CompareImages(GLuint textureID1, GLuint textureID2, int accuracy);
    double GetOSTimeMS(); // Yeah I think we need to expose this because Lua doesn't have a way to get time, since with don't include sol::lib:os
    int SetInterval(sol::protected_function callback, int milliseconds);
    void ClearInterval(int id);
    void UpdateIntervals();
    // The cursor in overlay pixels. Without Mouse anywhere only over the game
    // window, nil anywhere else.
    sol::variadic_results GetMousePosition(sol::this_state state);
    // GetMousePosition's answer for a cursor on the screen: game is the game
    // window's client area on screen (nullopt without the game), overlay the
    // overlay window's corner
    std::optional<std::pair<int, int>> mouseFor(POINT cursor, const std::optional<RECT>& game, POINT overlay, bool anywhere);

    // what the GetItem functions give, from the catalog passed in (nullptr for none)
    namespace Items {
        // takes the catalog the GetItem functions read until the next frame, and frees
        // the icons of an older one; call before the extensions update
        void beginFrame();
        sol::table state(const ItemFiles::Status& status, sol::state_view lua);
        sol::table types(const ItemCatalog::Catalog* catalog, sol::state_view lua);
        sol::object positions(const ItemCatalog::Catalog* catalog, int type, sol::state_view lua);
        sol::table regions(const ItemCatalog::Catalog* catalog, sol::state_view lua);
    }

}
