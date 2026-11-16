#include "credentials.hpp"
#include <windows.h>
#include <wincred.h>

namespace Credentials {
    std::wstring prefix = L"PocketEsoptron/";

    static std::wstring widen(const std::string& text) {
        if (text.empty()) return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
        std::wstring out(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size);
        return out;
    }

    static std::string narrow(const wchar_t* text) {
        if (!text || !*text) return "";
        int length = (int)wcslen(text);
        int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
        std::string out(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, length, &out[0], size, nullptr, nullptr);
        return out;
    }

    static std::wstring target(const std::string& site) {
        return prefix + widen(site);
    }

    void Wipe(std::string& secret) {
        if (!secret.empty()) SecureZeroMemory(&secret[0], secret.size());
        secret.clear();
    }

    bool Write(const std::string& site, const Saved& saved, const std::wstring& comment) {
        if (site.empty() || saved.secret.empty() || saved.secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) return false;
        std::wstring name = target(site), user = widen(saved.account);
        std::string blob = saved.secret;
        CREDENTIALW credential = {};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = name.data();
        credential.Comment = (LPWSTR)comment.c_str();
        credential.UserName = user.data();
        credential.CredentialBlob = (LPBYTE)blob.data();
        credential.CredentialBlobSize = (DWORD)blob.size();
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        bool ok = CredWriteW(&credential, 0) != FALSE;
        Wipe(blob);
        return ok;
    }

    std::optional<Saved> Read(const std::string& site) {
        PCREDENTIALW credential = nullptr;
        if (site.empty() || !CredReadW(target(site).c_str(), CRED_TYPE_GENERIC, 0, &credential)) return std::nullopt;
        Saved saved;
        saved.account = narrow(credential->UserName);
        saved.secret.assign((const char*)credential->CredentialBlob, credential->CredentialBlobSize);
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
        CredFree(credential);
        if (saved.secret.empty()) return std::nullopt;
        return saved;
    }

    bool MoveFrom(const std::wstring& oldPrefix, const std::string& site, const std::wstring& comment) {
        if (site.empty() || oldPrefix == prefix) return false;
        std::wstring old = oldPrefix + widen(site);
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(old.c_str(), CRED_TYPE_GENERIC, 0, &credential)) return false;
        Saved saved;
        saved.account = narrow(credential->UserName);
        saved.secret.assign((const char*)credential->CredentialBlob, credential->CredentialBlobSize);
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
        CredFree(credential);
        std::optional<Saved> current = Read(site);
        bool moved = !current && Write(site, saved, comment);
        if (current) Wipe(current->secret);
        Wipe(saved.secret);
        if (moved || current) CredDeleteW(old.c_str(), CRED_TYPE_GENERIC, 0);
        return moved;
    }

    bool Delete(const std::string& site) {
        if (site.empty()) return false;
        return CredDeleteW(target(site).c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
    }

    int DeleteAll(const std::wstring& under) {
        DWORD count = 0;
        PCREDENTIALW* found = nullptr;
        std::wstring filter = under + L"*";
        if (!CredEnumerateW(filter.c_str(), 0, &count, &found)) return 0;
        int deleted = 0;
        for (DWORD i = 0; i < count; i++) {
            if (found[i]->Type == CRED_TYPE_GENERIC && CredDeleteW(found[i]->TargetName, CRED_TYPE_GENERIC, 0)) deleted++;
        }
        CredFree(found);
        return deleted;
    }
}
