#include "traffic_keys.hpp"
#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include "../files.hpp"
#include "../net.hpp"

namespace TrafficKeys {
    // the same key saved with \r\n or \n is one key
    static std::string plain(std::string pem) {
        pem.erase(std::remove(pem.begin(), pem.end(), '\r'), pem.end());
        return pem;
    }

    bool Read(const std::string& body, SessionKey::Keys& out, std::string& problem) {
        nlohmann::json keys = nlohmann::json::parse(body, nullptr, false);
        if (!keys.is_object() || !keys.contains("format") || !keys["format"].is_number_integer()) {
            problem = "it isn't a keys file";
            return false;
        }
        if (keys["format"].get<int>() != FORMAT) {
            problem = "it's format " + std::to_string(keys["format"].get<int>()) + ", this overlay reads " + std::to_string(FORMAT);
            return false;
        }
        const nlohmann::json& dispatch = keys.contains("dispatch") ? keys["dispatch"] : nlohmann::json();
        const nlohmann::json& rsa = keys.contains("rsa") ? keys["rsa"] : nlohmann::json();
        if (!dispatch.is_object() || dispatch.empty() || !rsa.is_object() || rsa.empty()) {
            problem = "it has no dispatch or no login keys";
            return false;
        }
        SessionKey::Keys got;
        if (size_t left = SessionKey::AddDispatch(dispatch, got)) {
            problem = std::to_string(left) + " of its dispatch keys aren't good";
            return false;
        }
        for (auto& [id, pem] : rsa.items()) {
            if (!pem.is_string()) {
                problem = "login key " + id + " isn't text";
                return false;
            }
            got.rsaPem.push_back(plain(pem.get<std::string>()));
        }
        if (!SessionKey::Rsa(got.rsaPem).AllLoaded()) {
            problem = "a login key doesn't load";
            return false;
        }
        out = std::move(got);
        return true;
    }

    void Merge(SessionKey::Keys& into, const SessionKey::Keys& from) {
        for (auto& [id, key] : from.dispatch)
            into.dispatch[id] = key;
        for (const std::string& pem : from.rsaPem) {
            bool have = false;
            for (const std::string& mine : into.rsaPem)
                have = have || plain(mine) == plain(pem);
            if (!have)
                into.rsaPem.push_back(pem);
        }
    }

    bool Load(const std::filesystem::path& folder, SessionKey::Keys& out) {
        bool shipped = SessionKey::LoadKeys(folder, out);
        std::string saved = Files::readText(folder / FILE);
        if (saved.empty())
            return shipped;
        SessionKey::Keys got;
        std::string problem;
        if (!Read(saved, got, problem)) {
            std::cerr << "Game traffic: the saved " << FILE << " is left out, " << problem << std::endl;
            return shipped;
        }
        Merge(out, got);
        return !out.dispatch.empty() && !out.rsaPem.empty();
    }

    const char* GotCode(Got got) {
        switch (got) {
        case Got::Same: return "same";
        case Got::New: return "new";
        case Got::Missing: return "missing";
        case Got::Unreached: return "unreached";
        case Got::Broken: return "broken";
        }
        return "";
    }

    Got Take(const std::filesystem::path& folder, int status, const std::string& body, SessionKey::Keys& out) {
        if (status == 404)
            return Got::Missing;
        if (status != 200)
            return Got::Unreached;
        SessionKey::Keys got;
        std::string problem;
        if (!Read(body, got, problem)) {
            std::cerr << "Game traffic: the release's " << FILE << " is left out, " << problem << std::endl;
            return Got::Broken;
        }
        if (Files::readText(folder / FILE) == body)
            return Got::Same;
        // still used when it can't be saved, it's downloaded again next time
        if (!Files::writeFile(folder / FILE, body))
            std::cerr << "Game traffic: couldn't save " << (folder / FILE).string() << std::endl;
        SessionKey::Keys keys;
        SessionKey::LoadKeys(folder, keys);
        Merge(keys, got);
        out = std::move(keys);
        return Got::New;
    }

    Got Download(const std::filesystem::path& folder, SessionKey::Keys& out, Net::Stopper* stopper) {
        Net::Response response;
        if (!Net::get(Files::RELEASE + FILE, response, false, nullptr, stopper))
            return Got::Unreached;
        return Take(folder, response.status, response.body, out);
    }
}
