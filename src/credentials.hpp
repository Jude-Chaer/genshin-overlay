#pragma once
#include <optional>
#include <string>

// Logins kept in Windows' Credential Manager, one entry per site, for this
// Windows user on this PC. Windows encrypts them with the user's login, and
// they show there under prefix + site, where they can be deleted.

namespace Credentials {
    struct Saved {
        // shown in Credential Manager, e.g. the HoYoLAB account id
        std::string account;
        std::string secret;
    };

    // "PocketEsoptron/"; the tests use their own
    extern std::wstring prefix;
    // what prefix was in earlier versions
    inline const std::wstring OLD_PREFIX = L"genshin-overlay/";

    // a secret of 2560 bytes at most, Windows' limit
    bool Write(const std::string& site, const Saved& saved, const std::wstring& comment);
    std::optional<Saved> Read(const std::string& site);
    // true when it's gone, also when there was none
    bool Delete(const std::string& site);
    // every login whose name starts with under, the number deleted
    int DeleteAll(const std::wstring& under);
    // A login saved under oldPrefix moves to prefix with comment, unless prefix
    // has one already. True when one moved.
    bool MoveFrom(const std::wstring& oldPrefix, const std::string& site, const std::wstring& comment);
    // overwritten before it's freed
    void Wipe(std::string& secret);
}
