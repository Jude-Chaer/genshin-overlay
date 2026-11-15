#include "accounts.hpp"
#include "credentials.hpp"
#include "extensions.hpp"
#include "files.hpp"
#include "settings.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>

namespace Accounts {
    using Clock = std::chrono::steady_clock;

    // appsample's Firebase key, public in its site's code; used when the saved user has none
    static const char* AppsampleKey = "AIzaSyBVU3WOAtgWA35VLqL4EIXmxDcNYFHSwss";
    static const char* RefreshUrl = "https://securetoken.googleapis.com/v1/token?key=";

    struct Login {
        Status status;
        // what it goes back to when the window closes without a login
        State before = State::LoggedOut;
        // HoYoLAB's ltoken_v2, appsample's refresh token
        std::string secret;
        std::string uid, apiKey;
        std::string idToken;
        Clock::time_point idTokenEnds;
        // logged in, for Frame to compare with the account seen last
        std::string newAccount;
    };

    static std::mutex mutex;
    static Login logins[2];
    static std::atomic<int> changes = 0;
    // one appsample refresh at a time
    static std::mutex refreshing;

    static Login& of(Site site) { return logins[(int)site]; }

    static const Site Sites[] = { Site::HoyolabMap, Site::Appsample };

    const char* SiteName(Site site) {
        return site == Site::HoyolabMap ? "hoyolab_map" : "appsample";
    }

    std::optional<Site> SiteNamed(const std::string& name) {
        for (Site site : Sites) {
            if (name == SiteName(site)) return site;
        }
        return std::nullopt;
    }

    const char* SiteLabel(Site site) {
        return site == Site::HoyolabMap ? "HoYoLAB Map" : "appsample";
    }

    const char* StateName(State state) {
        switch (state) {
        case State::WindowOpen: return "window_open";
        case State::LoggedIn: return "logged_in";
        case State::NeedsLogin: return "needs_login";
        case State::Captcha: return "captcha";
        default: return "logged_out";
        }
    }

    static Extensions::ExtensionPermissionTypes permissionFor(Site site) {
        return site == Site::HoyolabMap ? Extensions::ExtensionPermissionTypes::HoyolabMapAccount
                                     : Extensions::ExtensionPermissionTypes::AppsampleAccount;
    }

    // a cookie value goes into a request header, nothing that could end it or start another one
    static bool cleanValue(const std::string& value) {
        if (value.empty() || value.size() > 512) return false;
        for (char c : value) {
            if (c <= ' ' || c > '~' || c == ';' || c == ',') return false;
        }
        return true;
    }

    // forgets the secrets, keeps the status
    static void forget(Login& login) {
        Credentials::Wipe(login.secret);
        Credentials::Wipe(login.idToken);
        login.uid.clear();
        login.apiKey.clear();
    }

    // appsample's secret in the store
    static std::string appsampleSecret(const Login& login) {
        return nlohmann::json{ { "uid", login.uid }, { "refresh_token", login.secret }, { "api_key", login.apiKey } }.dump();
    }

    // the store's entry into login, false when it doesn't read
    static bool take(Site site, Login& login, const std::string& account, const std::string& secret) {
        if (site == Site::HoyolabMap) {
            if (!cleanValue(account) || !cleanValue(secret)) return false;
            login.secret = secret;
            return true;
        }
        nlohmann::json saved = nlohmann::json::parse(secret, nullptr, false);
        if (!saved.is_object()) return false;
        login.uid = saved.value("uid", "");
        login.secret = saved.value("refresh_token", "");
        login.apiKey = saved.value("api_key", "");
        if (login.apiKey.empty()) login.apiKey = AppsampleKey;
        return !login.uid.empty() && cleanValue(login.secret) && cleanValue(login.apiKey);
    }

    void Init() {
        for (Site site : Sites) {
            std::optional<Credentials::Saved> saved = Credentials::Read(SiteName(site));
            if (!saved) continue;
            std::lock_guard<std::mutex> lock(mutex);
            Login& login = of(site);
            forget(login);
            if (take(site, login, saved->account, saved->secret)) {
                login.status = { State::LoggedIn, saved->account, "" };
                // compared with the account seen last like a new login
                login.newAccount = saved->account;
            }
            else {
                forget(login);
                std::cerr << "[accounts] the saved " << SiteLabel(site) << " login doesn't read, it's deleted" << std::endl;
                Credentials::Delete(SiteName(site));
            }
            Credentials::Wipe(saved->secret);
        }
        changes++;
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(mutex);
        for (Site site : Sites) forget(of(site));
    }

    // a login as another account is shown once Frame has switched the
    // permission off: until then it's still the window
    static bool settled(const Login& login) {
        return login.newAccount.empty();
    }

    Status Get(Site site) {
        std::lock_guard<std::mutex> lock(mutex);
        const Login& login = of(site);
        if (!settled(login)) return { State::WindowOpen, "", "" };
        return login.status;
    }

    int Changes() {
        return changes;
    }

    void Frame() {
        for (Site site : Sites) {
            std::string seen;
            {
                std::lock_guard<std::mutex> lock(mutex);
                Login& login = of(site);
                if (login.newAccount.empty()) continue;
                seen = Files::sha256(std::string(SiteName(site)) + ":" + login.newAccount);
            }
            std::string before = Settings::LoadAccountSeen(SiteName(site));
            if (before != seen) {
                Settings::SaveAccountSeen(SiteName(site), seen);
                if (!before.empty()) {
                    Extensions::switchOffEverywhere(permissionFor(site));
                    std::cout << "[accounts] logged in to another " << SiteLabel(site) << " account, its permission is off for every extension" << std::endl;
                }
            }
            std::lock_guard<std::mutex> lock(mutex);
            of(site).newAccount.clear();
            changes++;
        }
    }

    // a login from the window into the store and memory
    static bool found(Site site, Login& parsed, const std::string& account) {
        std::string secret = site == Site::HoyolabMap ? parsed.secret : appsampleSecret(parsed);
        std::wstring comment = site == Site::HoyolabMap ? L"HoYoLAB Map login of Genshin Overlay" : L"appsample login of Genshin Overlay";
        bool kept = Credentials::Write(SiteName(site), { account, secret }, comment);
        Credentials::Wipe(secret);
        std::lock_guard<std::mutex> lock(mutex);
        Login& login = of(site);
        if (!kept) {
            forget(parsed);
            login.status.state = login.before;
            login.status.problem = "Windows' Credential Manager didn't keep the login.";
            changes++;
            return false;
        }
        forget(login);
        login.secret = parsed.secret;
        login.uid = parsed.uid;
        login.apiKey = parsed.apiKey;
        login.status = { State::LoggedIn, account, "" };
        login.newAccount = account;
        forget(parsed);
        changes++;
        return true;
    }

    static bool notKept(Site site) {
        std::lock_guard<std::mutex> lock(mutex);
        Login& login = of(site);
        login.status.state = login.before;
        login.status.problem = std::string(SiteLabel(site)) + "'s login looked different than expected, nothing was kept.";
        changes++;
        return false;
    }

    bool FoundHoyolabMap(const std::string& ltuid, const std::string& ltoken) {
        Login parsed;
        if (!take(Site::HoyolabMap, parsed, ltuid, ltoken)) return notKept(Site::HoyolabMap);
        return found(Site::HoyolabMap, parsed, ltuid);
    }

    bool FoundAppsample(const FirebaseUser& user) {
        Login parsed;
        parsed.uid = user.uid;
        parsed.secret = user.refreshToken;
        parsed.apiKey = user.apiKey.empty() ? AppsampleKey : user.apiKey;
        if (parsed.uid.empty() || !cleanValue(parsed.secret) || !cleanValue(parsed.apiKey)) {
            forget(parsed);
            return notKept(Site::Appsample);
        }
        return found(Site::Appsample, parsed, user.email.empty() ? user.uid : user.email);
    }

    void WindowOpened(Site site) {
        std::lock_guard<std::mutex> lock(mutex);
        Login& login = of(site);
        if (login.status.state == State::WindowOpen) return;
        login.before = login.status.state;
        login.status.state = State::WindowOpen;
        login.status.problem.clear();
        changes++;
    }

    void WindowClosed(Site site, const std::string& problem) {
        std::lock_guard<std::mutex> lock(mutex);
        Login& login = of(site);
        if (login.status.state != State::WindowOpen) return;
        login.status.state = login.before;
        login.status.problem = problem;
        changes++;
    }

    void LogOut(Site site) {
        Credentials::Delete(SiteName(site));
        std::lock_guard<std::mutex> lock(mutex);
        Login& login = of(site);
        forget(login);
        login.status = {};
        login.before = State::LoggedOut;
        changes++;
    }

    void Refused(Site site, const std::string& secret) {
        std::lock_guard<std::mutex> lock(mutex);
        Login& login = of(site);
        if (login.status.state != State::LoggedIn && login.status.state != State::Captcha) return;
        // a newer login came meanwhile
        if (!secret.empty() && secret != login.secret) return;
        Credentials::Delete(SiteName(site));
        forget(login);
        login.status.state = State::NeedsLogin;
        changes++;
    }

    void CaptchaAsked(Site site) {
        if (site != Site::HoyolabMap) return;
        std::lock_guard<std::mutex> lock(mutex);
        Login& login = of(site);
        if (login.status.state != State::LoggedIn) return;
        login.status.state = State::Captcha;
        changes++;
    }

    void Working(Site site) {
        std::lock_guard<std::mutex> lock(mutex);
        Login& login = of(site);
        if (login.status.state != State::Captcha) return;
        login.status.state = State::LoggedIn;
        changes++;
    }

    std::string HoyolabMapCookie() {
        std::lock_guard<std::mutex> lock(mutex);
        const Login& login = of(Site::HoyolabMap);
        if (login.secret.empty() || !settled(login) || (login.status.state != State::LoggedIn && login.status.state != State::Captcha)) return "";
        return "ltuid_v2=" + login.status.account + "; ltoken_v2=" + login.secret;
    }

    std::optional<FirebaseUser> ReadFirebaseUser(const std::string& json) {
        nlohmann::json value = nlohmann::json::parse(json, nullptr, false);
        if (!value.is_object()) return std::nullopt;
        FirebaseUser user;
        if (value.contains("uid") && value["uid"].is_string()) user.uid = value["uid"].get<std::string>();
        if (value.contains("email") && value["email"].is_string()) user.email = value["email"].get<std::string>();
        if (value.contains("apiKey") && value["apiKey"].is_string()) user.apiKey = value["apiKey"].get<std::string>();
        auto tokens = value.find("stsTokenManager");
        if (tokens != value.end() && tokens->is_object() && tokens->contains("refreshToken") && (*tokens)["refreshToken"].is_string())
            user.refreshToken = (*tokens)["refreshToken"].get<std::string>();
        if (user.uid.empty() || user.refreshToken.empty()) {
            Credentials::Wipe(user.refreshToken);
            return std::nullopt;
        }
        return user;
    }

    // a string field, or a number written as one; "" for anything else
    static std::string textField(const nlohmann::json& object, const char* key) {
        auto it = object.find(key);
        if (it == object.end()) return "";
        if (it->is_string()) return it->get<std::string>();
        if (it->is_number_integer()) return std::to_string(it->get<long long>());
        return "";
    }

    Refresh ReadRefresh(const Net::Response& response) {
        Refresh refresh;
        nlohmann::json answer = nlohmann::json::parse(response.body, nullptr, false);
        if (response.status == 200 && answer.is_object()) {
            refresh.idToken = textField(answer, "id_token");
            refresh.refreshToken = textField(answer, "refresh_token");
            std::string expires = textField(answer, "expires_in");
            refresh.expiresIn = std::atoi(expires.c_str());
            refresh.ok = !refresh.idToken.empty() && !refresh.refreshToken.empty() && refresh.expiresIn > 0;
            if (!refresh.ok) refresh.why = "securetoken's answer has no token in it";
            return refresh;
        }
        // 429 is too many at once, not the token
        if (response.status >= 400 && response.status < 500 && response.status != 429 && answer.is_object() && answer.contains("error")) {
            refresh.refused = true;
            const nlohmann::json& error = answer["error"];
            refresh.why = error.is_object() && error.contains("message") && error["message"].is_string() ? error["message"].get<std::string>() : "refused";
            return refresh;
        }
        refresh.why = response.status == 0 ? "securetoken.googleapis.com couldn't be reached"
                                            : "securetoken.googleapis.com answered " + std::to_string(response.status);
        return refresh;
    }

    std::optional<std::string> AppsampleIdToken(Net::Stopper* stopper) {
        std::lock_guard<std::mutex> one(refreshing);
        std::string body, apiKey, uid, sent;
        {
            std::lock_guard<std::mutex> lock(mutex);
            Login& login = of(Site::Appsample);
            if (login.status.state != State::LoggedIn || login.secret.empty() || !settled(login)) return std::nullopt;
            if (!login.idToken.empty() && Clock::now() + std::chrono::minutes(5) < login.idTokenEnds) return login.idToken;
            body = nlohmann::json{ { "grant_type", "refresh_token" }, { "refresh_token", login.secret } }.dump();
            apiKey = login.apiKey;
            uid = login.uid;
            sent = login.secret;
        }
        Net::Response response;
        Net::post(RefreshUrl + apiKey, body, response, false, {}, stopper);
        Credentials::Wipe(body);
        Refresh refresh = ReadRefresh(response);
        Credentials::Wipe(response.body);
        if (!refresh.refused) Credentials::Wipe(sent);
        if (refresh.refused) {
            std::cerr << "[accounts] appsample's login was refused: " << refresh.why << std::endl;
            Refused(Site::Appsample, sent);
            Credentials::Wipe(sent);
            return std::nullopt;
        }
        if (!refresh.ok) {
            std::cerr << "[accounts] appsample's login couldn't be refreshed: " << refresh.why << std::endl;
            return std::nullopt;
        }

        std::lock_guard<std::mutex> lock(mutex);
        Login& login = of(Site::Appsample);
        // logged out or in as someone else meanwhile
        if (login.status.state != State::LoggedIn || login.uid != uid) {
            Credentials::Wipe(refresh.idToken);
            Credentials::Wipe(refresh.refreshToken);
            return std::nullopt;
        }
        if (refresh.refreshToken != login.secret) {
            Credentials::Wipe(login.secret);
            login.secret = refresh.refreshToken;
            std::string secret = appsampleSecret(login);
            if (!Credentials::Write(SiteName(Site::Appsample), { login.status.account, secret }, L"appsample login of Genshin Overlay"))
                std::cerr << "[accounts] appsample's new refresh token couldn't be kept, the old one is still saved" << std::endl;
            Credentials::Wipe(secret);
        }
        Credentials::Wipe(refresh.refreshToken);
        Credentials::Wipe(login.idToken);
        login.idToken = refresh.idToken;
        login.idTokenEnds = Clock::now() + std::chrono::seconds(refresh.expiresIn);
        return login.idToken;
    }
}
