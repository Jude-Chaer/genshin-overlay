#include <catch2/catch_test_macros.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include "net.hpp"

#pragma comment(lib, "ws2_32.lib")

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
