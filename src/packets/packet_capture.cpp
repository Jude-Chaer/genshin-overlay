#include "packet_capture.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <type_traits>
#include "../helpers.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace PacketCapture {
    static constexpr const char* FILTER = "udp port 22101 or udp port 22102";
    // a few seconds of play even when the worker is held up, never 100s of MB
    static constexpr size_t MAX_WAITING_BYTES = 8 * 1024 * 1024;
    // how often a capture thread looks at Stop
    static constexpr int TIMEOUT_MS = 200;
    static constexpr size_t MAX_ADAPTERS = 255;

    const char* ProblemCode(Problem problem) {
        switch (problem) {
        case Problem::None: return "";
        case Problem::NpcapMissing: return "npcap_missing";
        case Problem::NpcapTooOld: return "npcap_too_old";
        case Problem::NpcapAdminOnly: return "npcap_admin_only";
        case Problem::NoAdapter: return "no_adapter";
        }
        return "";
    }

    Problem PickProblem(Npcap npcap, bool adminOnly, bool admin) {
        if (npcap == Npcap::Missing)
            return Problem::NpcapMissing;
        if (npcap == Npcap::TooOld)
            return Problem::NpcapTooOld;
        if (adminOnly && !admin)
            return Problem::NpcapAdminOnly;
        return Problem::NoAdapter;
    }

    bool GamePacket(const uint8_t* ip, size_t size) {
        return Frames::Read(ip, size, Frames::Start::Ip).kind != Frames::Kind::None;
    }

    static uint64_t nowMs() {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    void Queue::Push(Frame&& frame) {
        {
            std::lock_guard lock(m_mutex);
            if (m_bytes + frame.data.size() > m_maxBytes) {
                m_missed++;
                return;
            }
            m_bytes += frame.data.size();
            m_frames.push_back(std::move(frame));
        }
        m_wake.notify_one();
    }

    void Queue::Take(std::vector<Frame>& out, int milliseconds) {
        out.clear();
        std::unique_lock lock(m_mutex);
        m_wake.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return !m_frames.empty(); });
        out.assign(std::make_move_iterator(m_frames.begin()), std::make_move_iterator(m_frames.end()));
        m_frames.clear();
        m_bytes = 0;
    }

    void Queue::Clear() {
        std::lock_guard lock(m_mutex);
        m_frames.clear();
        m_bytes = 0;
    }

    uint64_t Queue::Missed() const {
        std::lock_guard lock(m_mutex);
        return m_missed;
    }

    size_t Queue::Bytes() const {
        std::lock_guard lock(m_mutex);
        return m_bytes;
    }

    // the parts of pcap.h that are used, so the Npcap SDK isn't needed
    struct pcap;
    struct pcap_if {
        pcap_if* next;
        char* name;
        char* description;
        void* addresses;
        uint32_t flags;
    };
    struct pcap_pkthdr {
        // struct timeval on Windows, two longs
        long sec, usec;
        uint32_t caplen, len;
    };
    struct bpf_program {
        unsigned int length;
        void* instructions;
    };
    static constexpr int ERRBUF_SIZE = 256;
    static constexpr int DLT_EN10MB = 1;
    // vpn tunnels like WireGuard give the ip packet with nothing in front
    static constexpr int DLT_RAW = 12;
    static constexpr uint32_t NETMASK_UNKNOWN = 0xffffffff;

    static struct {
        int (*findalldevs)(pcap_if**, char*);
        void (*freealldevs)(pcap_if*);
        pcap* (*create)(const char*, char*);
        int (*set_promisc)(pcap*, int);
        int (*set_immediate_mode)(pcap*, int);
        int (*set_timeout)(pcap*, int);
        int (*activate)(pcap*);
        int (*datalink)(pcap*);
        int (*compile)(pcap*, bpf_program*, const char*, int, uint32_t);
        int (*setfilter)(pcap*, bpf_program*);
        void (*freecode)(bpf_program*);
        int (*next_ex)(pcap*, pcap_pkthdr**, const unsigned char**);
        void (*close)(pcap*);
    } api;
    static Npcap npcap = Npcap::Missing;

    // Npcap puts its wpcap.dll in System32\Npcap, not on the search path.
    // Altered search path lets it find Packet.dll next to it. Loaded once.
    static Npcap loadNpcap() {
        if (npcap == Npcap::Loaded)
            return npcap;
        wchar_t system[MAX_PATH];
        UINT n = GetSystemDirectoryW(system, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return npcap = Npcap::Missing;
        std::wstring path = std::wstring(system) + L"\\Npcap\\wpcap.dll";
        HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll)
            return npcap = Npcap::Missing;
        bool ok = true;
        auto get = [&](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(GetProcAddress(dll, name));
            if (!function)
                ok = false;
        };
        get(api.findalldevs, "pcap_findalldevs");
        get(api.freealldevs, "pcap_freealldevs");
        get(api.create, "pcap_create");
        get(api.set_promisc, "pcap_set_promisc");
        get(api.set_immediate_mode, "pcap_set_immediate_mode");
        get(api.set_timeout, "pcap_set_timeout");
        get(api.activate, "pcap_activate");
        get(api.datalink, "pcap_datalink");
        get(api.compile, "pcap_compile");
        get(api.setfilter, "pcap_setfilter");
        get(api.freecode, "pcap_freecode");
        get(api.next_ex, "pcap_next_ex");
        get(api.close, "pcap_close");
        if (!ok) {
            FreeLibrary(dll);
            return npcap = Npcap::TooOld;
        }
        return npcap = Npcap::Loaded;
    }

    // "Restrict Npcap driver's access to Administrators only", ticked in its installer
    static bool npcapAdminOnly() {
        DWORD value = 0, size = sizeof(value);
        return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\npcap\\Parameters", L"AdminOnly",
            RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value != 0;
    }

    // one adapter, only the game's ports. Nullptr for loopback and the like, or when it can't
    static pcap* openNpcap(const char* name, Frames::Start& start) {
        char error[ERRBUF_SIZE] = {};
        pcap* handle = api.create(name, error);
        if (!handle)
            return nullptr;
        api.set_promisc(handle, 0);
        api.set_immediate_mode(handle, 1);
        api.set_timeout(handle, TIMEOUT_MS);
        int link = api.activate(handle) < 0 ? -1 : api.datalink(handle);
        if (link != DLT_EN10MB && link != DLT_RAW) {
            api.close(handle);
            return nullptr;
        }
        start = link == DLT_RAW ? Frames::Start::Ip : Frames::Start::Ethernet;
        bpf_program program = {};
        bool filtered = api.compile(handle, &program, FILTER, 1, NETMASK_UNKNOWN) >= 0 && api.setfilter(handle, &program) >= 0;
        api.freecode(&program);
        if (!filtered) {
            api.close(handle);
            return nullptr;
        }
        return handle;
    }

    struct Listener::Opened {
        pcap* handle = nullptr;
        Frames::Start start = Frames::Start::Ethernet;
        SOCKET socket = INVALID_SOCKET;
        std::string name, key;
    };

    Listener::Listener() : m_queue(MAX_WAITING_BYTES) {}

    Listener::~Listener() {
        Stop();
    }

    bool Listener::startNpcap(std::vector<Opened>& opened) {
        char error[ERRBUF_SIZE] = {};
        pcap_if* devices = nullptr;
        if (api.findalldevs(&devices, error) < 0)
            return false;
        m_listed = 0;
        for (pcap_if* device = devices; device; device = device->next)
            m_listed++;
        for (pcap_if* device = devices; device && opened.size() < MAX_ADAPTERS; device = device->next) {
            Frames::Start start = Frames::Start::Ethernet;
            pcap* handle = openNpcap(device->name, start);
            if (!handle)
                continue;
            Opened one;
            one.handle = handle;
            one.start = start;
            one.name = device->description ? device->description : device->name;
            one.key = device->name;
            opened.push_back(std::move(one));
        }
        api.freealldevs(devices);
        return !opened.empty();
    }

    // the ipv4 addresses of the adapters that are up, raw sockets bind to one each
    static std::vector<std::pair<sockaddr_in, std::string>> rawAddresses() {
        std::vector<std::pair<sockaddr_in, std::string>> out;
        ULONG size = 16 * 1024;
        std::vector<uint8_t> list;
        ULONG result = ERROR_BUFFER_OVERFLOW;
        for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; tries++) {
            list.resize(size);
            result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(list.data()), &size);
        }
        if (result != NO_ERROR)
            return out;
        for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(list.data()); adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, name, sizeof(name), nullptr, nullptr);
            for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
                if (address->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                out.push_back({ *reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr), name });
            }
        }
        return out;
    }

    static std::string addressText(const sockaddr_in& address) {
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
        return text;
    }

    // Only ipv4, raw sockets see every packet of the address and keep the
    // game's. SIO_RCVALL needs admin.
    bool Listener::startRaw(std::vector<Opened>& opened) {
        if (!m_winsock) {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
                return false;
            m_winsock = true;
        }
        auto addresses = rawAddresses();
        m_listed = addresses.size();
        for (auto& [address, adapter] : addresses) {
            if (opened.size() >= MAX_ADAPTERS)
                break;
            SOCKET s = socket(AF_INET, SOCK_RAW, IPPROTO_IP);
            if (s == INVALID_SOCKET)
                continue;
            DWORD on = RCVALL_ON, returned = 0, timeout = TIMEOUT_MS;
            int bufferSize = 8 * 1024 * 1024;
            if (bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
                || WSAIoctl(s, SIO_RCVALL, &on, sizeof(on), nullptr, 0, &returned, nullptr, nullptr) != 0) {
                closesocket(s);
                continue;
            }
            // without the timeout recv could wait forever and Stop with it
            if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
                closesocket(s);
                continue;
            }
            setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufferSize), sizeof(bufferSize));
            Opened one;
            one.socket = s;
            one.key = addressText(address);
            one.name = adapter + " " + one.key + " (raw socket)";
            opened.push_back(std::move(one));
        }
        return !opened.empty();
    }

    std::atomic<bool> skipNpcap = false;

    bool Listener::Start() {
        Stop();
        std::vector<Opened> opened;
        bool admin = Helpers::isElevated();
        Npcap loaded = skipNpcap ? Npcap::Missing : loadNpcap();
        bool adminOnly = loaded == Npcap::Loaded && npcapAdminOnly();
        if (loaded == Npcap::Loaded && (!adminOnly || admin) && startNpcap(opened))
            m_way = Way::Npcap;
        // without Npcap, or when it shut the overlay out
        else if (admin && startRaw(opened))
            m_way = Way::RawSockets;
        else {
            m_problem = PickProblem(loaded, adminOnly, admin);
            return false;
        }

        m_problem = Problem::None;
        m_stopping = false;
        m_queue.Clear();
        m_live = int(opened.size());
        for (Opened& one : opened) {
            m_names.push_back(one.name);
            m_keys.push_back(one.key);
            if (one.handle) {
                m_threads.emplace_back([this, handle = one.handle, start = one.start] {
                    while (!m_stopping) {
                        pcap_pkthdr* header = nullptr;
                        const unsigned char* data = nullptr;
                        int result = api.next_ex(handle, &header, &data);
                        if (result == 0)
                            continue;
                        // the adapter went away
                        if (result < 0)
                            break;
                        Frame frame;
                        frame.ms = uint64_t(header->sec) * 1000 + uint64_t(header->usec) / 1000;
                        frame.start = start;
                        frame.data.assign(data, data + header->caplen);
                        m_queue.Push(std::move(frame));
                    }
                    api.close(handle);
                    m_live--;
                });
            }
            else {
                m_threads.emplace_back([this, s = one.socket] {
                    std::vector<uint8_t> buffer(65536);
                    while (!m_stopping) {
                        int n = recv(s, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0);
                        if (n == SOCKET_ERROR) {
                            if (WSAGetLastError() == WSAETIMEDOUT)
                                continue;
                            break;
                        }
                        if (!GamePacket(buffer.data(), size_t(n)))
                            continue;
                        Frame frame;
                        frame.ms = nowMs();
                        frame.start = Frames::Start::Ip;
                        frame.data.assign(buffer.begin(), buffer.begin() + n);
                        m_queue.Push(std::move(frame));
                    }
                    closesocket(s);
                    m_live--;
                });
            }
        }
        return true;
    }

    void Listener::Stop() {
        m_stopping = true;
        for (std::thread& thread : m_threads)
            thread.join();
        m_threads.clear();
        m_names.clear();
        m_keys.clear();
        m_live = 0;
        m_way = Way::None;
        if (m_winsock) {
            WSACleanup();
            m_winsock = false;
        }
    }

    std::vector<std::string> Listener::list() const {
        std::vector<std::string> keys;
        if (m_way == Way::Npcap) {
            char error[ERRBUF_SIZE] = {};
            pcap_if* devices = nullptr;
            if (api.findalldevs(&devices, error) < 0)
                return keys;
            for (pcap_if* device = devices; device; device = device->next)
                keys.push_back(device->name);
            api.freealldevs(devices);
        }
        else if (m_way == Way::RawSockets) {
            for (auto& [address, adapter] : rawAddresses())
                keys.push_back(addressText(address));
        }
        return keys;
    }

    void Listener::Refresh() {
        if (m_way == Way::None || m_live < int(m_threads.size())) {
            Start();
            return;
        }
        std::vector<std::string> now = list();
        // the list has the ones that didn't open too (loopback, an address
        // that won't bind), so it's compared by count plus the opened ones
        bool changed = now.size() != m_listed;
        for (const std::string& key : m_keys)
            changed |= std::find(now.begin(), now.end(), key) == now.end();
        if (changed)
            Start();
    }
}
