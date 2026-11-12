#include <catch2/catch_test_macros.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include "net.hpp"
#include "extensions.hpp"
#include "lua_functions.hpp"
#include "test_extension.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

// Net::get against a server on this machine that answers one request the way
// a test asks: the whole body, a body cut short, or one piece at a time.

namespace {
    struct Server {
        SOCKET listener = INVALID_SOCKET;
        int port = 0;
        std::thread thread;
        std::atomic<bool> done{ false };

        // says it sends `told` bytes, sends `sent` of them `piece` at a time with `pause` between, then closes
        Server(size_t told, size_t sent, size_t piece, std::chrono::milliseconds pause) {
            WSADATA data;
            WSAStartup(MAKEWORD(2, 2), &data);
            listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address = {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = 0;
            bind(listener, (sockaddr*)&address, sizeof(address));
            int size = sizeof(address);
            getsockname(listener, (sockaddr*)&address, &size);
            port = ntohs(address.sin_port);
            listen(listener, 1);

            thread = std::thread([this, told, sent, piece, pause]() {
                SOCKET client = accept(listener, nullptr, nullptr);
                if (client == INVALID_SOCKET) return;
                // the request, up to its blank line
                std::string request;
                char buffer[1024];
                while (request.find("\r\n\r\n") == std::string::npos) {
                    int got = recv(client, buffer, sizeof(buffer), 0);
                    if (got <= 0) break;
                    request.append(buffer, got);
                }
                std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(told) + "\r\nConnection: close\r\n\r\n";
                send(client, head.data(), (int)head.size(), 0);
                std::string body(piece, 'x');
                for (size_t at = 0; at < sent && !done; at += piece) {
                    int n = (int)std::min(piece, sent - at);
                    if (send(client, body.data(), n, 0) != n) break;
                    if (pause.count() > 0) std::this_thread::sleep_for(pause);
                }
                closesocket(client);
            });
        }

        ~Server() {
            done = true;
            closesocket(listener);
            if (thread.joinable()) thread.join();
            WSACleanup();
        }

        std::string url() const {
            return "http://127.0.0.1:" + std::to_string(port) + "/items.pack.gz";
        }
    };
}

TEST_CASE("a whole body is a success") {
    Server server(10000, 10000, 1000, std::chrono::milliseconds(0));
    Net::Response response;
    CHECK(Net::get(server.url(), response));
    CHECK(response.status == 200);
    CHECK(response.body.size() == 10000);
}

TEST_CASE("a body cut short is a failure") {
    Server server(10000, 5000, 1000, std::chrono::milliseconds(0));
    Net::Response response;
    CHECK_FALSE(Net::get(server.url(), response));
}

TEST_CASE("stopping a download that's coming in slowly returns at once, from another thread") {
    // 1 MB at 1 KB every 100 ms would take almost two minutes
    Server server(1000000, 1000000, 1000, std::chrono::milliseconds(100));
    Net::Stopper stopper;
    std::atomic<bool> started{ false };
    bool ok = true;
    size_t got = 0;
    std::thread download([&]() {
        Net::Response response;
        ok = Net::get(server.url(), response, false, [&](size_t bytes) {
            started = true;
            return true;
        }, &stopper);
        got = response.body.size();
    });
    auto waited = std::chrono::steady_clock::now();
    while (!started && std::chrono::steady_clock::now() - waited < std::chrono::seconds(10)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(started);
    // stopped while the get waits for the next piece, as when the overlay closes
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    auto stoppedAt = std::chrono::steady_clock::now();
    stopper.stop();
    download.join();
    auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stoppedAt).count();
    CHECK_FALSE(ok);
    CHECK(got < 1000000);
    INFO("returned " << took << " ms after stop");
    CHECK(took < 500);

    // once stopped, a get given it fails right away
    Net::Response response;
    CHECK_FALSE(Net::get(server.url(), response, false, nullptr, &stopper));
}

// Extensions reach the internet only: this PC and the home network are
// refused before anything is sent.

namespace {
    // Answers every request that comes, with answer(request) as the whole
    // reply, and counts them; serves as a site or as a proxy.
    struct Replier {
        SOCKET listener = INVALID_SOCKET;
        int port = 0;
        std::thread thread;
        std::atomic<int> requests{ 0 };
        std::atomic<int> connections{ 0 };
        std::mutex mutex;
        std::vector<std::string> seen;
        // each connection is served on its own thread, closed with the listener
        std::vector<SOCKET> open;
        std::vector<std::thread> served;

        explicit Replier(std::function<std::string(const std::string&)> answer) {
            WSADATA data;
            WSAStartup(MAKEWORD(2, 2), &data);
            listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address = {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            bind(listener, (sockaddr*)&address, sizeof(address));
            int size = sizeof(address);
            getsockname(listener, (sockaddr*)&address, &size);
            port = ntohs(address.sin_port);
            listen(listener, 4);

            thread = std::thread([this, answer]() {
                for (;;) {
                    SOCKET client = accept(listener, nullptr, nullptr);
                    if (client == INVALID_SOCKET) return;
                    connections++;
                    std::lock_guard<std::mutex> lock(mutex);
                    open.push_back(client);
                    served.emplace_back([this, answer, client]() { serve(client, answer); });
                }
            });
        }

        void serve(SOCKET client, const std::function<std::string(const std::string&)>& answer) {
            std::string request;
            char buffer[1024];
            for (;;) {
                size_t end = request.find("\r\n\r\n");
                if (end != std::string::npos) {
                    std::string head = request.substr(0, end);
                    request.erase(0, end + 4);
                    requests++;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        seen.push_back(head);
                    }
                    std::string reply = answer(head);
                    send(client, reply.data(), (int)reply.size(), 0);
                    continue;
                }
                int got = recv(client, buffer, sizeof(buffer), 0);
                if (got <= 0) break;
                request.append(buffer, got);
            }
            std::lock_guard<std::mutex> lock(mutex);
            auto at = std::find(open.begin(), open.end(), client);
            if (at != open.end()) {
                open.erase(at);
                closesocket(client);
            }
        }

        ~Replier() {
            closesocket(listener);
            if (thread.joinable()) thread.join();
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (SOCKET client : open) closesocket(client);
                open.clear();
            }
            for (auto& t : served) t.join();
            WSACleanup();
        }

        std::wstring address() const {
            return L"127.0.0.1:" + std::to_wstring(port);
        }
    };

    std::string ascii(const std::wstring& text) {
        std::string out;
        for (wchar_t c : text) out += static_cast<char>(c);
        return out;
    }

    std::string ok(const std::string& body) {
        return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }

    // the proxy setting goes back when the test ends
    struct Proxy {
        explicit Proxy(const std::wstring& address) { Net::testProxy = address; }
        ~Proxy() { Net::testProxy.clear(); }
    };
}

namespace {
    // a body with no Content-Length
    std::string chunked(const std::string& body) {
        char size[32];
        snprintf(size, sizeof(size), "%zx", body.size());
        return "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" + std::string(size) + "\r\n" + body + "\r\n0\r\n\r\n";
    }

    // an answer that ends its connection
    std::string closing(const std::string& body) {
        return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    }
}

TEST_CASE("a post whose answer is cut short is a failure") {
    Server server(10000, 5000, 1000, std::chrono::milliseconds(0));
    Net::Response response;
    CHECK_FALSE(Net::post(server.url(), "{}", response));
}

TEST_CASE("a request past its deadline stops there") {
    // 1 MB at 1 KB every 100 ms would take almost two minutes
    Server server(1000000, 1000000, 1000, std::chrono::milliseconds(100));
    Net::Limits limits;
    limits.deadlineMs = 300;
    Net::Response response;
    auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(Net::get(server.url(), response, false, nullptr, nullptr, limits));
    CHECK(response.timedOut);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

TEST_CASE("a body past maxBody is a failure, said up front or not") {
    Server told(2000000, 2000000, 100000, std::chrono::milliseconds(0));
    Net::Limits limits;
    limits.maxBody = 1000000;
    Net::Response response;
    CHECK_FALSE(Net::get(told.url(), response, false, nullptr, nullptr, limits));
    CHECK(response.tooBig);

    Replier untold([](const std::string&) { return chunked(std::string(1000000, 'x')); });
    limits.maxBody = 999999;
    Net::Response cut;
    CHECK_FALSE(Net::get("http://" + ascii(untold.address()) + "/", cut, false, nullptr, nullptr, limits));
    CHECK(cut.tooBig);

    Server fits(1000000, 1000000, 100000, std::chrono::milliseconds(0));
    limits.maxBody = 1000000;
    Net::Response whole;
    CHECK(Net::get(fits.url(), whole, false, nullptr, nullptr, limits));
    CHECK(whole.body.size() == 1000000);
}

TEST_CASE("only addresses out on the internet count as public") {
    // this PC, the home network, link-local, Tailscale, multicast, and IPv4 written as IPv6
    for (const wchar_t* home : { L"127.0.0.1", L"127.5.5.5", L"0.0.0.0", L"10.1.2.3", L"172.16.0.1", L"172.31.255.255",
        L"192.168.1.1", L"169.254.10.20", L"100.64.0.1", L"100.127.255.255", L"224.0.0.1", L"255.255.255.255",
        L"::", L"::1", L"fe80::1", L"fec0::1", L"fd12:3456::1", L"fc00::1", L"ff02::1",
        L"::ffff:192.168.1.1", L"::ffff:127.0.0.1", L"64:ff9b::a00:1", L"2002:c0a8:101::1",
        L"router.home", L"", L"192.168.1" }) {
        INFO(ascii(home));
        CHECK_FALSE(Net::publicAddress(home));
    }
    // public DNS servers, a fake-IP proxy's range, the edges of the private ranges
    for (const wchar_t* out : { L"8.8.8.8", L"1.1.1.1", L"172.15.255.255", L"172.32.0.1", L"100.63.255.255", L"100.128.0.1",
        L"198.18.0.5", L"2001:4860:4860::8888", L"2606:4700:4700::1111", L"::ffff:8.8.8.8", L"64:ff9b::808:808" }) {
        INFO(ascii(out));
        CHECK(Net::publicAddress(out));
    }
}

TEST_CASE("every address of this PC is refused") {
    ULONG size = 64 * 1024;
    std::vector<unsigned char> buffer(size);
    REQUIRE(GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, (IP_ADAPTER_ADDRESSES*)buffer.data(), &size) == NO_ERROR);
    int checked = 0;
    for (auto* adapter = (IP_ADAPTER_ADDRESSES*)buffer.data(); adapter; adapter = adapter->Next) {
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            sockaddr* address = unicast->Address.lpSockaddr;
            wchar_t text[64] = {};
            void* bytes = address->sa_family == AF_INET ? (void*)&((sockaddr_in*)address)->sin_addr : (void*)&((sockaddr_in6*)address)->sin6_addr;
            REQUIRE(InetNtopW(address->sa_family, bytes, text, 64));
            // Clash's own adapter sits in its fake range, which is let through on purpose
            if (std::wstring(text).rfind(L"198.18.", 0) == 0 || std::wstring(text).rfind(L"198.19.", 0) == 0) continue;
            INFO(ascii(text));
            CHECK_FALSE(Net::publicAddress(text));
            checked++;
            // the same IPv4 written as IPv6 goes to the same place
            if (address->sa_family == AF_INET) {
                std::wstring v4 = text;
                CHECK_FALSE(Net::publicAddress(L"::ffff:" + v4));
                CHECK_FALSE(Net::publicAddress(L"64:ff9b::" + v4));
                const unsigned char* b = (const unsigned char*)bytes;
                wchar_t sixToFour[64] = {};
                swprintf(sixToFour, 64, L"2002:%02x%02x:%02x%02x::1", b[0], b[1], b[2], b[3]);
                INFO(ascii(sixToFour));
                CHECK_FALSE(Net::publicAddress(sixToFour));
            }
        }
    }
    CHECK(checked > 0);
}

TEST_CASE("an extension's request to this PC sends nothing") {
    Replier site([](const std::string&) { return ok("hi"); });
    std::string port = std::to_string(site.port);

    for (const std::string& host : { std::string("127.0.0.1"), std::string("localhost") }) {
        INFO(host);
        Net::Response response;
        CHECK_FALSE(Net::get("http://" + host + ":" + port + "/", response, false, nullptr, nullptr, Net::Limits{ true }));
        CHECK(response.refused.find(host) == 0);
        Net::Response posted;
        CHECK_FALSE(Net::post("http://" + host + ":" + port + "/", "{\"a\":1}", posted, false, Net::Limits{ true }));
        CHECK(posted.refused.find(host) == 0);
        Net::WebSocket socket;
        CHECK_FALSE(socket.connect("ws://" + host + ":" + port + "/", false, Net::Limits{ true }));
        CHECK(socket.refusedPlace().find(host) == 0);
    }
    CHECK(site.requests == 0);

    // the overlay's own requests aren't extensions'
    Net::Response response;
    CHECK(Net::get("http://127.0.0.1:" + port + "/", response));
    CHECK(response.body == "hi");
    CHECK(response.refused.empty());
    CHECK(site.requests == 1);
}

TEST_CASE("through a proxy on this PC the name asked for is checked") {
    // like Clash on 127.0.0.1:7890: the request names the site, the proxy fetches it
    Replier proxy([](const std::string& head) {
        if (head.find("GET http://example.com/away") == 0)
            return std::string("HTTP/1.1 302 Found\r\nLocation: http://192.168.1.1/admin\r\nContent-Length: 0\r\n\r\n");
        return ok("through");
    });
    Proxy use(proxy.address());

    Net::Response response;
    // a site on the internet goes through
    CHECK(Net::get("http://example.com/", response, false, nullptr, nullptr, Net::Limits{ true }));
    CHECK(response.body == "through");
    CHECK(proxy.requests == 1);

    // a home address or a name for this PC never reaches the proxy
    for (const std::string& url : { std::string("http://192.168.1.1/admin"), std::string("http://localhost:8080/"), std::string("http://[::1]/") }) {
        INFO(url);
        Net::Response refused;
        CHECK_FALSE(Net::get(url, refused, false, nullptr, nullptr, Net::Limits{ true }));
        CHECK_FALSE(refused.refused.empty());
    }
    CHECK(proxy.requests == 1);

    // nor does a redirect to one
    Net::Response redirected;
    CHECK_FALSE(Net::get("http://example.com/away", redirected, false, nullptr, nullptr, Net::Limits{ true }));
    CHECK(redirected.refused == "192.168.1.1");
    CHECK(proxy.requests == 2);
    {
        std::lock_guard<std::mutex> lock(proxy.mutex);
        for (const std::string& head : proxy.seen) CHECK(head.find("192.168") == std::string::npos);
    }
}

namespace {
    // Sec-WebSocket-Accept: base64 of SHA-1 of the key and the protocol's own GUID
    std::string acceptFor(const std::string& key) {
        std::string text = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        unsigned char hash[20] = {};
        BCRYPT_ALG_HANDLE sha1 = nullptr;
        BCryptOpenAlgorithmProvider(&sha1, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
        BCryptHash(sha1, nullptr, 0, (PUCHAR)text.data(), (ULONG)text.size(), hash, sizeof(hash));
        BCryptCloseAlgorithmProvider(sha1, 0);
        char out[64] = {};
        DWORD size = sizeof(out);
        CryptBinaryToStringA(hash, sizeof(hash), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &size);
        return out;
    }

    bool readExactly(SOCKET client, unsigned char* into, size_t count) {
        for (size_t at = 0; at < count;) {
            int got = recv(client, (char*)into + at, (int)(count - at), 0);
            if (got <= 0) return false;
            at += got;
        }
        return true;
    }

    // one frame from the client (always masked): its opcode and text
    bool readFrame(SOCKET client, int& opcode, std::string& text) {
        unsigned char head[2];
        if (!readExactly(client, head, 2)) return false;
        opcode = head[0] & 0x0F;
        uint64_t length = head[1] & 0x7F;
        if (length == 126) {
            unsigned char more[2];
            if (!readExactly(client, more, 2)) return false;
            length = (more[0] << 8) | more[1];
        }
        unsigned char mask[4];
        if (!readExactly(client, mask, 4)) return false;
        text.resize((size_t)length);
        if (length && !readExactly(client, (unsigned char*)text.data(), (size_t)length)) return false;
        for (size_t i = 0; i < text.size(); i++) text[i] ^= mask[i % 4];
        return true;
    }

    void sendFrame(SOCKET client, int opcode, const std::string& text) {
        std::string frame;
        frame += (char)(0x80 | opcode);
        frame += (char)text.size();  // the tests send short ones only
        frame += text;
        send(client, frame.data(), (int)frame.size(), 0);
    }

    // a web socket server on this machine that answers each message with "echo: " and it
    struct EchoServer {
        SOCKET listener = INVALID_SOCKET;
        int port = 0;
        std::thread thread;
        std::atomic<bool> closedByClient{ false };
        // closes the socket itself after its first answer
        bool closeAfterEcho = false;

        explicit EchoServer(bool closeAfterEcho = false) : closeAfterEcho(closeAfterEcho) {
            WSADATA data;
            WSAStartup(MAKEWORD(2, 2), &data);
            listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address = {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            bind(listener, (sockaddr*)&address, sizeof(address));
            int size = sizeof(address);
            getsockname(listener, (sockaddr*)&address, &size);
            port = ntohs(address.sin_port);
            listen(listener, 1);

            thread = std::thread([this]() {
                SOCKET client = accept(listener, nullptr, nullptr);
                if (client == INVALID_SOCKET) return;
                std::string request;
                char buffer[1024];
                while (request.find("\r\n\r\n") == std::string::npos) {
                    int got = recv(client, buffer, sizeof(buffer), 0);
                    if (got <= 0) break;
                    request.append(buffer, got);
                }
                // as a proxy it's asked for a tunnel first
                if (request.rfind("CONNECT ", 0) == 0) {
                    std::string tunnel = "HTTP/1.1 200 Connection established\r\n\r\n";
                    send(client, tunnel.data(), (int)tunnel.size(), 0);
                    request.erase(0, request.find("\r\n\r\n") + 4);
                    while (request.find("\r\n\r\n") == std::string::npos) {
                        int got = recv(client, buffer, sizeof(buffer), 0);
                        if (got <= 0) break;
                        request.append(buffer, got);
                    }
                }
                size_t at = request.find("Sec-WebSocket-Key: ");
                if (at == std::string::npos) {
                    closesocket(client);
                    return;
                }
                at += 19;
                std::string key = request.substr(at, request.find("\r\n", at) - at);
                std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + acceptFor(key) + "\r\n\r\n";
                send(client, reply.data(), (int)reply.size(), 0);

                int opcode = 0;
                std::string text;
                while (readFrame(client, opcode, text)) {
                    if (opcode == 8) {
                        closedByClient = true;
                        sendFrame(client, 8, text.substr(0, 2));
                        break;
                    }
                    sendFrame(client, 1, "echo: " + text);
                    if (this->closeAfterEcho) {
                        sendFrame(client, 8, "");
                        break;
                    }
                }
                closesocket(client);
            });
        }

        ~EchoServer() {
            closesocket(listener);
            if (thread.joinable()) thread.join();
            WSACleanup();
        }
    };
}

TEST_CASE("a web socket sends, gets its answers and closes") {
    EchoServer server;
    Net::WebSocket socket;
    REQUIRE(socket.connect("ws://127.0.0.1:" + std::to_string(server.port) + "/"));
    CHECK(socket.isConnected());
    CHECK(socket.send("hello"));
    CHECK(socket.send("again"));

    std::vector<std::string> got;
    auto waited = std::chrono::steady_clock::now();
    while (got.size() < 2 && std::chrono::steady_clock::now() - waited < std::chrono::seconds(5)) {
        for (std::string& message : socket.pollAll()) got.push_back(message);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(got.size() == 2);
    CHECK(got[0] == "echo: hello");
    CHECK(got[1] == "echo: again");

    auto closing = std::chrono::steady_clock::now();
    socket.close();
    auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - closing).count();
    CHECK_FALSE(socket.isConnected());
    CHECK_FALSE(socket.send("after"));
    INFO("closed in " << took << " ms");
    CHECK(took < 1500);
    // it said goodbye before going
    server.thread.join();
    CHECK(server.closedByClient);
}


TEST_CASE("a web socket with too many messages not polled is closed") {
    EchoServer server;
    Net::WebSocket socket;
    socket.setMaxQueued(2);
    REQUIRE(socket.connect("ws://127.0.0.1:" + std::to_string(server.port) + "/"));
    for (const char* message : { "one", "two", "three" }) CHECK(socket.send(message));
    auto waited = std::chrono::steady_clock::now();
    while (socket.isConnected() && std::chrono::steady_clock::now() - waited < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(socket.overflowed());
    CHECK_FALSE(socket.isConnected());
    // the ones that fit are still there
    CHECK(socket.pollAll().size() == 2);
}

TEST_CASE("a site takes in the names under it and nothing else") {
    Net::Sites sites{ true, { "hoyolab.com" } };
    CHECK(Net::onSites("hoyolab.com", sites));
    CHECK(Net::onSites("bbs-api-os.HoYoLAB.com", sites));
    CHECK_FALSE(Net::onSites("evilhoyolab.com", sites));
    CHECK_FALSE(Net::onSites("hoyolab.com.evil.net", sites));
    CHECK_FALSE(Net::onSites("hoyolab.com.", sites));
    CHECK_FALSE(Net::onSites("example.com", sites));
    CHECK_FALSE(Net::onSites("example.com", Net::Sites{ true, {} }));
    CHECK(Net::onSites("example.com", Net::Sites{}));
}

TEST_CASE("a request held to its sites is refused on a redirect away from them") {
    Replier proxy([](const std::string& head) {
        if (head.find("GET http://example.com/away") == 0)
            return std::string("HTTP/1.1 302 Found\r\nLocation: http://example.org/\r\nContent-Length: 0\r\n\r\n");
        return ok("not this");
    });
    Proxy use(proxy.address());
    Net::Sites sites{ true, { "example.com" } };

    // the request to a site goes out, the redirect away from it doesn't
    Net::Response redirected;
    CHECK_FALSE(Net::get("http://example.com/away", redirected, false, nullptr, nullptr, Net::Limits{ true, sites }));
    CHECK(redirected.refused == "example.org");
    CHECK(redirected.notListed);
    CHECK(proxy.requests == 1);

    // another site is refused before anything is sent
    Net::Response other;
    CHECK_FALSE(Net::get("http://example.org/", other, false, nullptr, nullptr, Net::Limits{ true, sites }));
    CHECK(other.refused == "example.org");
    CHECK(other.notListed);
    CHECK(proxy.requests == 1);

    Net::WebSocket socket;
    CHECK_FALSE(socket.connect("ws://example.org/", false, Net::Limits{ true, sites }));
    CHECK(socket.refusedNotListed());
    CHECK(proxy.requests == 1);
}



// Net_Start: an extension's request on its own thread, the answer given once.

namespace {
    using Permission = Extensions::ExtensionPermissionTypes;

    // loads it, gives it Network Access and runs its Init
    Extensions::Extension* netExtension(const std::string& name, const std::string& lua) {
        Extensions::Extension* ext = TestExtension::load(name, lua);
        if (!ext) return nullptr;
        ext->extensionPermissions.insert(Permission::NetworkAccess);
        Extensions::runInit(*ext);
        return ext;
    }

    // frames until done() or 5 s
    bool framesUntil(const std::function<bool()>& done) {
        auto started = std::chrono::steady_clock::now();
        while (!done()) {
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(5)) return false;
            TestExtension::turn();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
}

TEST_CASE("Net_Start's onDone gets the answer once, Net_Result gives it once") {
    Replier proxy([](const std::string& head) {
        if (head.find("POST http://example.com/post") == 0) return closing("posted");
        return closing("answer");
    });
    Proxy use(proxy.address());
    Extensions::Extension* ext = netExtension("net_start", R"(
        function Init()
            _G.got = 0
            Net_Start("http://example.com/", function(answer) _G.got = _G.got + 1 _G.first = answer end)
            Net_Start("http://example.com/post", { method = "POST", body = "hi" }, function(answer) _G.posted = answer end)
            _G.collected = Net_Start("http://example.com/")
        end
    )");
    REQUIRE(ext != nullptr);
    sol::state& lua = *ext->luaState;
    CHECK(framesUntil([&]() { return lua["posted"] != sol::lua_nil && lua["got"].get_or(0) == 1; }));
    for (int i = 0; i < 5; i++) TestExtension::turn();
    CHECK(lua["got"].get_or(0) == 1);
    CHECK(lua["first"]["status"].get_or(0) == 200);
    CHECK(lua["first"]["body"].get_or(std::string()) == "answer");
    CHECK(lua["posted"]["body"].get_or(std::string()) == "posted");

    CHECK(framesUntil([&]() { return TestExtension::run(ext, "_G.result = Net_Result(_G.collected)") && lua["result"] != sol::lua_nil; }));
    CHECK(lua["result"]["body"].get_or(std::string()) == "answer");
    CHECK(TestExtension::run(ext, "_G.again = Net_Result(_G.collected)"));
    CHECK(lua["again"] == sol::lua_nil);
    CHECK(LuaFunctions::netRequestsOf(ext) == 0);
    TestExtension::unloadAll();
}

TEST_CASE("Net_Start's onDone comes on turns with nothing drawn") {
    Replier proxy([](const std::string&) { return closing("answer"); });
    Proxy use(proxy.address());
    Extensions::Extension* ext = netExtension("net_undrawn", R"(
        function Init()
            Net_Start("http://example.com/", function(answer) _G.answer = answer end)
        end
    )");
    REQUIRE(ext != nullptr);
    sol::state& lua = *ext->luaState;
    // the map and menu closed: the loop turns, no frame is drawn
    auto started = std::chrono::steady_clock::now();
    while (lua["answer"] == sol::lua_nil && std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) {
        LuaFunctions::UpdateAnswers();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(lua["answer"]["body"].get_or(std::string()) == "answer");
    TestExtension::unloadAll();
}

TEST_CASE("Net_Start doesn't wait for the answer") {
    Replier proxy([](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        return closing("late");
    });
    Proxy use(proxy.address());
    auto started = std::chrono::steady_clock::now();
    Extensions::Extension* ext = netExtension("net_waits", R"(
        function Init() _G.id = Net_Start("http://example.com/") _G.waiting = Net_Result(_G.id) end
    )");
    REQUIRE(ext != nullptr);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(400));
    sol::state& lua = *ext->luaState;
    CHECK(lua["id"].get_or(0) > 0);
    CHECK(lua["waiting"] == sol::lua_nil);
    // another extension doesn't get it
    Extensions::Extension* other = netExtension("net_other", "function Init() end");
    REQUIRE(other != nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    CHECK(TestExtension::run(other, "_G.stolen = Net_Result(" + std::to_string(lua["id"].get_or(0)) + ")"));
    CHECK((*other->luaState)["stolen"] == sol::lua_nil);
    CHECK(TestExtension::run(ext, "_G.late = Net_Result(_G.id)"));
    CHECK(lua["late"]["body"].get_or(std::string()) == "late");
    TestExtension::unloadAll();
}

TEST_CASE("Net_Start's answer says what went wrong") {
    Replier proxy([](const std::string&) { return closing(std::string(LuaFunctions::NetMaxBody + 1, 'x')); });
    Proxy use(proxy.address());
    Extensions::Extension* ext = netExtension("net_errors", R"(
        function Init()
            Net_Start("http://example.com/", function(answer) _G.big = answer end)
            Net_Start("http://192.168.1.1/admin", function(answer) _G.home = answer end)
        end
    )");
    REQUIRE(ext != nullptr);
    sol::state& lua = *ext->luaState;
    CHECK(framesUntil([&]() { return lua["big"] != sol::lua_nil && lua["home"] != sol::lua_nil; }));
    CHECK(lua["big"]["error"].get_or(std::string()) == "too_big");
    CHECK(lua["big"]["body"] == sol::lua_nil);
    CHECK(lua["home"]["error"].get_or(std::string()) == "refused");
    TestExtension::unloadAll();
}

TEST_CASE("an extension's requests stop with it, and with its Network Access") {
    Replier proxy([](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return closing("late");
    });
    Proxy use(proxy.address());
    const char* lua = R"(
        function Init() Net_Start("http://example.com/", function() _G.called = true end) end
    )";

    Extensions::Extension* ext = netExtension("net_off", lua);
    REQUIRE(ext != nullptr);
    ext->extensionPermissions.erase(Permission::NetworkAccess);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    for (int i = 0; i < 3; i++) TestExtension::turn();
    CHECK((*ext->luaState)["called"] == sol::lua_nil);
    CHECK(LuaFunctions::netRequestsOf(ext) == 0);
    TestExtension::unloadAll();

    ext = netExtension("net_unloads", lua);
    REQUIRE(ext != nullptr);
    CHECK(LuaFunctions::netRequestsOf(ext) == 1);
    auto started = std::chrono::steady_clock::now();
    TestExtension::unloadAll();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(400));
}

TEST_CASE("an extension has so many requests waiting at most") {
    // refused at once, and never collected
    Extensions::Extension* ext = netExtension("net_many", R"(
        function Init()
            _G.ids = {}
            for i = 1, 17 do _G.ids[i] = Net_Start("http://192.168.1.1/") or false end
        end
    )");
    REQUIRE(ext != nullptr);
    {
        // let go of before its Lua state closes
        sol::table ids = (*ext->luaState)["ids"];
        CHECK(ids[LuaFunctions::NetMaxWaiting].get_or(0) > 0);
        CHECK(ids[LuaFunctions::NetMaxWaiting + 1].get_or(true) == false);
    }
    CHECK(LuaFunctions::netRequestsOf(ext) == LuaFunctions::NetMaxWaiting);
    TestExtension::unloadAll();
}


// An extension's web socket: on its own thread, the handlers called in a frame.

TEST_CASE("an extension's web socket connects without waiting and calls its handlers") {
    EchoServer server;
    Proxy use(L"127.0.0.1:" + std::to_wstring(server.port));
    auto started = std::chrono::steady_clock::now();
    Extensions::Extension* ext = netExtension("socket_handlers", R"(
        function Init()
            _G.got = {}
            _G.ws = WebSocket.new()
            _G.started = _G.ws:connect("ws://example.com/", {
                onOpen = function() _G.opened = _G.ws:isConnected() end,
                onMessage = function(message) table.insert(_G.got, message) end,
                onClose = function(reason) _G.reason = reason end,
            })
            -- queued until it's open
            _G.sent = _G.ws:send("hello")
        end
    )");
    REQUIRE(ext != nullptr);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(200));
    sol::state& lua = *ext->luaState;
    CHECK(lua["started"].get_or(false));
    CHECK(lua["sent"].get_or(false));
    CHECK(framesUntil([&]() { return lua["got"][1] != sol::lua_nil; }));
    CHECK(lua["opened"].get_or(false));
    CHECK(lua["got"][1].get_or(std::string()) == "echo: hello");
    // onMessage takes them, poll doesn't
    CHECK(TestExtension::run(ext, "_G.ws:send('again') _G.polled = _G.ws:poll()"));
    CHECK(framesUntil([&]() { return lua["got"][2] != sol::lua_nil; }));
    CHECK(lua["polled"] == sol::lua_nil);

    auto closing = std::chrono::steady_clock::now();
    CHECK(TestExtension::run(ext, "_G.ws:close() _G.after = _G.ws:send('after')"));
    CHECK(std::chrono::steady_clock::now() - closing < std::chrono::milliseconds(50));
    CHECK(lua["after"].get_or(true) == false);
    CHECK(framesUntil([&]() { return lua["reason"] != sol::lua_nil; }));
    CHECK(lua["reason"].get_or(std::string()) == "closed");
    CHECK(framesUntil([&]() { return LuaFunctions::socketsOf(nullptr) == 0; }));
    CHECK(LuaFunctions::socketsOf(ext) == 0);
    server.thread.join();
    CHECK(server.closedByClient);
    TestExtension::unloadAll();
}

TEST_CASE("without handlers an extension polls its web socket") {
    EchoServer server;
    Proxy use(L"127.0.0.1:" + std::to_wstring(server.port));
    Extensions::Extension* ext = netExtension("socket_polls", R"(
        function Init()
            _G.ws = WebSocket.new()
            _G.ws:connect("ws://example.com/")
            _G.ws:send("hello")
        end
    )");
    REQUIRE(ext != nullptr);
    sol::state& lua = *ext->luaState;
    CHECK(framesUntil([&]() { return TestExtension::run(ext, "_G.message = _G.ws:poll()") && lua["message"] != sol::lua_nil; }));
    CHECK(lua["message"].get_or(std::string()) == "echo: hello");
    CHECK(TestExtension::run(ext, "_G.open = _G.ws:isConnected()"));
    CHECK(lua["open"].get_or(false));
    TestExtension::unloadAll();
    CHECK(framesUntil([&]() { return LuaFunctions::socketsOf(nullptr) == 0; }));
}

TEST_CASE("an extension's web socket says why it couldn't connect") {
    Extensions::Extension* ext = netExtension("socket_refused", R"(
        function Init()
            _G.ws = WebSocket.new()
            _G.wrongScheme = _G.ws:connect("http://example.com/")
            _G.ws:connect("ws://192.168.1.1/", { onClose = function(reason) _G.reason = reason end })
        end
    )");
    REQUIRE(ext != nullptr);
    sol::state& lua = *ext->luaState;
    CHECK(lua["wrongScheme"].get_or(true) == false);
    CHECK(framesUntil([&]() { return lua["reason"] != sol::lua_nil; }));
    CHECK(lua["reason"].get_or(std::string()) == "refused");
    TestExtension::unloadAll();
}

TEST_CASE("an extension's web socket ends with it, and with its Network Access") {
    EchoServer server;
    Proxy use(L"127.0.0.1:" + std::to_wstring(server.port));
    const char* lua = R"(
        function Init()
            _G.ws = WebSocket.new()
            _G.ws:connect("ws://example.com/", {
                onMessage = function() _G.called = true end,
                onClose = function() _G.called = true end,
            })
            _G.ws:send("hello")
        end
    )";

    Extensions::Extension* ext = netExtension("socket_off", lua);
    REQUIRE(ext != nullptr);
    ext->extensionPermissions.erase(Permission::NetworkAccess);
    for (int i = 0; i < 3; i++) TestExtension::turn();
    CHECK(LuaFunctions::socketsOf(ext) == 0);
    CHECK(framesUntil([&]() { return LuaFunctions::socketsOf(nullptr) == 0; }));
    CHECK((*ext->luaState)["called"] == sol::lua_nil);
    TestExtension::unloadAll();

    // nothing answers the upgrade, so it's still connecting when it's unloaded
    Replier silent([](const std::string&) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return std::string();
    });
    Proxy quiet(silent.address());
    ext = netExtension("socket_unloads", lua);
    REQUIRE(ext != nullptr);
    CHECK(LuaFunctions::socketsOf(ext) == 1);
    auto started = std::chrono::steady_clock::now();
    TestExtension::unloadAll();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(200));
    CHECK(framesUntil([&]() { return LuaFunctions::socketsOf(nullptr) == 0; }));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

TEST_CASE("an extension's web socket holds so many messages to send at most") {
    Replier silent([](const std::string&) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return std::string();
    });
    Proxy use(silent.address());
    Extensions::Extension* ext = netExtension("socket_full", R"(
        function Init()
            _G.ws = WebSocket.new()
            _G.ws:connect("ws://example.com/")
            _G.sent = 0
            for i = 1, 1001 do if _G.ws:send("m") then _G.sent = _G.sent + 1 end end
        end
    )");
    REQUIRE(ext != nullptr);
    CHECK((*ext->luaState)["sent"].get_or(0) == (int)LuaFunctions::SocketMaxQueued);
    TestExtension::unloadAll();
    CHECK(framesUntil([&]() { return LuaFunctions::socketsOf(nullptr) == 0; }));
}

TEST_CASE("an extension's web socket calls onOpen even when it closed before the frame") {
    EchoServer server(true);
    Proxy use(L"127.0.0.1:" + std::to_wstring(server.port));
    Extensions::Extension* ext = netExtension("socket_quick", R"(
        function Init()
            _G.order = {}
            _G.ws = WebSocket.new()
            _G.ws:connect("ws://example.com/", {
                onOpen = function() table.insert(_G.order, "open") end,
                onMessage = function() table.insert(_G.order, "message") end,
                onClose = function() table.insert(_G.order, "close") end,
            })
            _G.ws:send("hello")
        end
    )");
    REQUIRE(ext != nullptr);
    server.thread.join();
    // no frame runs until it's closed on our side too
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(framesUntil([&]() { return LuaFunctions::socketsOf(ext) == 0; }));
    CHECK(TestExtension::run(ext, "_G.said = table.concat(_G.order, ' ')"));
    CHECK((*ext->luaState)["said"].get_or(std::string()) == "open message close");
    TestExtension::unloadAll();
}

TEST_CASE("connecting a web socket again lets the old connection go quietly") {
    Replier silent([](const std::string&) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return std::string();
    });
    Proxy use(silent.address());
    Extensions::Extension* ext = netExtension("socket_again", R"(
        function Init()
            _G.ws = WebSocket.new()
            _G.ws:connect("ws://example.com/", { onClose = function() _G.oldClosed = true end })
            _G.again = _G.ws:connect("ws://example.com/")
        end
    )");
    REQUIRE(ext != nullptr);
    CHECK((*ext->luaState)["again"].get_or(false));
    CHECK(LuaFunctions::socketsOf(ext) == 1);
    for (int i = 0; i < 3; i++) TestExtension::turn();
    CHECK((*ext->luaState)["oldClosed"] == sol::lua_nil);
    TestExtension::unloadAll();
    CHECK(framesUntil([&]() { return LuaFunctions::socketsOf(nullptr) == 0; }));
}

TEST_CASE("an answer past the memory cap stops the extension, not the overlay") {
    Replier proxy([](const std::string&) { return closing(std::string(4 * 1024 * 1024, 'x')); });
    Proxy use(proxy.address());
    Extensions::Extension* ext = netExtension("net_full", R"(
        function Init() Net_Start("http://example.com/", function(answer) _G.got = #answer.body end) end
    )");
    REQUIRE(ext != nullptr);
    const size_t mb = 1024 * 1024;
    ext->memory->limit = (ext->memory->used / mb + 2) * mb;
    CHECK(framesUntil([&]() { return !ext->stopReason.empty(); }));
    CHECK(ext->stoppedForMemory);
    // closed once stopped, the same turn
    CHECK((!ext->luaState || (*ext->luaState)["got"] == sol::lua_nil));
    TestExtension::unloadAll();
}
