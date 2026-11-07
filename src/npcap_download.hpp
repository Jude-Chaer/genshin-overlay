#pragma once
#include <string>

// The menu's Download Npcap button: gets the newest installer from npcap.com
// and opens it, the player clicks through Npcap's own installer. Npcap's free
// license doesn't allow a silent install or shipping it with the overlay.

namespace NpcapDownload {
    enum class State { Idle, Downloading, Opened, Failed };

    // on its own thread, does nothing while one is running
    extern void Start();
    extern State GetState();

    // the installer's file name in npcap.com's page, empty when it isn't there
    extern std::string InstallerName(const std::string& page);
    // the names Npcap's installers are signed with
    extern bool NpcapSigner(const std::wstring& name);
}
