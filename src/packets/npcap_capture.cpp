#include "npcap_capture.hpp"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <type_traits>

namespace NpcapCapture {
    static constexpr const char* FILTER = "udp port 22101 or udp port 22102";
    // about a minute of play, frames past this get dropped
    static constexpr size_t MAX_WAITING = 100000;

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
        char* (*geterr)(pcap*);
        void (*close)(pcap*);
    } api;
    static HMODULE library = nullptr;

    static std::vector<std::thread> threads;
    static std::atomic<bool> stopping = false;
    static std::mutex mutex;
    static std::condition_variable wake;
    // guarded by mutex
    static std::deque<Frame> waiting;
    static uint64_t dropped = 0;

    // Npcap puts its wpcap.dll in System32\Npcap, not on the search path.
    // Altered search path lets it find Packet.dll next to it.
    static bool load(std::string& problem) {
        if (library) return true;
        wchar_t system[MAX_PATH];
        UINT n = GetSystemDirectoryW(system, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            problem = "couldn't find the Windows folder";
            return false;
        }
        std::wstring path = std::wstring(system) + L"\\Npcap\\wpcap.dll";
        HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) {
            problem = "Npcap isn't installed (npcap.com)";
            return false;
        }
        bool ok = true;
        auto get = [&](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(GetProcAddress(dll, name));
            if (!function) ok = false;
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
        get(api.geterr, "pcap_geterr");
        get(api.close, "pcap_close");
        if (!ok) {
            FreeLibrary(dll);
            problem = "this Npcap is too old, install the newest from npcap.com";
            return false;
        }
        library = dll;
        return true;
    }

    // one adapter, only the game's ports. Nullptr if it can't, why stays empty
    // for adapters that aren't ethernet.
    static pcap* open(const pcap_if& device, std::string& why) {
        char error[ERRBUF_SIZE] = {};
        pcap* handle = api.create(device.name, error);
        if (!handle) {
            why = error;
            return nullptr;
        }
        api.set_promisc(handle, 0);
        api.set_immediate_mode(handle, 1);
        // so a thread notices Stop at least this often
        api.set_timeout(handle, 200);
        if (api.activate(handle) < 0) {
            why = api.geterr(handle);
            api.close(handle);
            return nullptr;
        }
        // loopback and the like, the game never talks over those
        if (api.datalink(handle) != DLT_EN10MB) {
            api.close(handle);
            return nullptr;
        }
        bpf_program program = {};
        if (api.compile(handle, &program, FILTER, 1, NETMASK_UNKNOWN) < 0 || api.setfilter(handle, &program) < 0) {
            why = api.geterr(handle);
            api.freecode(&program);
            api.close(handle);
            return nullptr;
        }
        api.freecode(&program);
        return handle;
    }

    static void run(pcap* handle, uint8_t adapter) {
        while (!stopping) {
            pcap_pkthdr* header = nullptr;
            const unsigned char* data = nullptr;
            int result = api.next_ex(handle, &header, &data);
            if (result == 0) continue;
            if (result < 0) break;
            Frame frame;
            frame.ms = uint64_t(header->sec) * 1000 + uint64_t(header->usec) / 1000;
            frame.adapter = adapter;
            frame.data.assign(data, data + header->caplen);
            {
                std::lock_guard lock(mutex);
                if (waiting.size() >= MAX_WAITING) {
                    waiting.pop_front();
                    dropped++;
                }
                waiting.push_back(std::move(frame));
            }
            wake.notify_one();
        }
        api.close(handle);
    }

    bool Start(std::vector<std::string>& adapters, std::string& problem) {
        adapters.clear();
        if (!threads.empty()) {
            problem = "already capturing";
            return false;
        }
        if (!load(problem)) return false;

        char error[ERRBUF_SIZE] = {};
        pcap_if* devices = nullptr;
        if (api.findalldevs(&devices, error) < 0) {
            problem = std::string("Npcap can't list the adapters: ") + error;
            return false;
        }
        std::string lastWhy;
        std::vector<pcap*> handles;
        for (pcap_if* device = devices; device && handles.size() < 255; device = device->next) {
            std::string why;
            pcap* handle = open(*device, why);
            if (!handle) {
                if (!why.empty()) lastWhy = why;
                continue;
            }
            handles.push_back(handle);
            adapters.push_back(device->description ? device->description : device->name);
        }
        api.freealldevs(devices);

        if (handles.empty()) {
            // with "restrict to administrators" ticked at install, every open fails
            problem = "no adapter could be opened, Npcap may be set to administrators only";
            if (!lastWhy.empty()) problem += " (" + lastWhy + ")";
            return false;
        }
        stopping = false;
        {
            std::lock_guard lock(mutex);
            waiting.clear();
            dropped = 0;
        }
        for (size_t i = 0; i < handles.size(); i++)
            threads.emplace_back(run, handles[i], uint8_t(i));
        return true;
    }

    void Stop() {
        stopping = true;
        for (std::thread& thread : threads) thread.join();
        threads.clear();
        wake.notify_all();
    }

    bool Running() {
        return !threads.empty();
    }

    void Take(std::vector<Frame>& out, int milliseconds) {
        out.clear();
        std::unique_lock lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(milliseconds), [] { return !waiting.empty() || stopping; });
        out.assign(std::make_move_iterator(waiting.begin()), std::make_move_iterator(waiting.end()));
        waiting.clear();
    }

    uint64_t Dropped() {
        std::lock_guard lock(mutex);
        return dropped;
    }
}
