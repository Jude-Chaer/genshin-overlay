// before windows.h, or it brings the old winsock along
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "net.hpp"

// HACK: glew is *not* needed here, but not putting it here gives a redefinition warning of 'APIENTRY' when compiling (Doesn't cause errors, as far as I could tell though).
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <windows.h>
#include <winhttp.h>
#include <mutex>
#include <iostream>
#include <thread>
#include <deque>
#include <atomic>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace Net {
	static std::wstring widen(const std::string& s) {
		return std::wstring(s.begin(), s.end());
	}

	static std::string narrow(const std::wstring& s) {
		std::string out;
		for (wchar_t c : s) out += c < 128 ? (char)c : '?';
		return out;
	}

	// Every request runs async: an async request can be closed from any thread
	// and from its own callback, a blocking one can't.
	// withTestProxy: through testProxy when it's set
	static HINTERNET openSession(bool withTestProxy) {
		bool named = withTestProxy && !testProxy.empty();
		HINTERNET opened = WinHttpOpen(L"genshin-overlay", named ? WINHTTP_ACCESS_TYPE_NAMED_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			named ? testProxy.c_str() : WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
		if (opened) {
			WinHttpSetTimeouts(opened, 10000, 10000, 30000, 30000);
		}
		return opened;
	}

	// the overlay's own requests share one, made once and never through testProxy
	static HINTERNET asyncSession = nullptr;
	static std::once_flag asyncOnce;

	static HINTERNET sharedSession() {
		std::call_once(asyncOnce, []() { asyncSession = openSession(false); });
		return asyncSession;
	}

	std::wstring testProxy;

	// the first bits of a and prefix are the same
	static bool inside(const unsigned char* a, const unsigned char* prefix, int bits) {
		for (int i = 0; bits > 0; i++, bits -= 8) {
			unsigned char mask = (unsigned char)(0xFF << (8 - (bits < 8 ? bits : 8)));
			if ((a[i] & mask) != (prefix[i] & mask)) return false;
		}
		return true;
	}

	static bool publicV4(const unsigned char* a) {
		struct Range { unsigned char prefix[4]; int bits; };
		static const Range home[] = {
			{ { 0, 0, 0, 0 }, 8 },        // "this network"
			{ { 10, 0, 0, 0 }, 8 },
			{ { 100, 64, 0, 0 }, 10 },    // the ISP's side, and the player's other devices on Tailscale
			{ { 127, 0, 0, 0 }, 8 },      // this PC
			{ { 169, 254, 0, 0 }, 16 },   // link-local
			{ { 172, 16, 0, 0 }, 12 },
			{ { 192, 0, 0, 0 }, 24 },
			{ { 192, 168, 0, 0 }, 16 },
			{ { 224, 0, 0, 0 }, 3 },      // multicast, reserved, broadcast
		};
		for (const Range& range : home) {
			if (inside(a, range.prefix, range.bits)) return false;
		}
		return true;
	}

	// on a network one of this PC's adapters is on: the PC itself, and with
	// IPv6 the devices at home have addresses from the internet's range too
	struct OwnAddress {
		int family;
		unsigned char bytes[16];
		int bits;
	};

	// this PC's addresses, read again when older than 2 s; false when they couldn't be read
	static bool ownAddresses(std::vector<OwnAddress>& out) {
		static std::mutex mutex;
		static std::vector<OwnAddress> cached;
		static ULONGLONG readAt = 0;
		static bool readOk = false;
		std::lock_guard<std::mutex> lock(mutex);
		ULONGLONG now = GetTickCount64();
		if (readAt == 0 || now - readAt > 2000) {
			ULONG size = 16 * 1024;
			std::vector<unsigned char> buffer;
			ULONG result = ERROR_BUFFER_OVERFLOW;
			for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; tries++) {
				buffer.resize(size);
				result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
					nullptr, (IP_ADAPTER_ADDRESSES*)buffer.data(), &size);
			}
			cached.clear();
			readOk = result == NO_ERROR || result == ERROR_NO_DATA;
			if (result == NO_ERROR) {
				for (auto* adapter = (IP_ADAPTER_ADDRESSES*)buffer.data(); adapter; adapter = adapter->Next) {
					for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
						const sockaddr* address = unicast->Address.lpSockaddr;
						if (!address || (address->sa_family != AF_INET && address->sa_family != AF_INET6)) continue;
						OwnAddress own = { address->sa_family, {}, (int)unicast->OnLinkPrefixLength };
						if (address->sa_family == AF_INET) memcpy(own.bytes, &((const sockaddr_in*)address)->sin_addr, 4);
						else memcpy(own.bytes, &((const sockaddr_in6*)address)->sin6_addr, 16);
						cached.push_back(own);
					}
				}
			}
			readAt = now;
		}
		out = cached;
		return readOk;
	}

	// on a network one of this PC's adapters is on: the PC itself, and with
	// IPv6 the devices at home have addresses from the internet's range too
	static bool onThisNetwork(int family, const unsigned char* a) {
		std::vector<OwnAddress> own;
		// without the list, nothing can be told apart: refused
		if (!ownAddresses(own)) return true;
		for (const OwnAddress& address : own) {
			if (address.family != family) continue;
			// the PC's own address always; its network unless it's too wide to be a
			// home network, as a VPN can say it's on everything (0.0.0.0/0)
			int whole = family == AF_INET ? 32 : 128;
			if (inside(a, address.bytes, whole)) return true;
			if (address.bits >= (family == AF_INET ? 8 : 32) && inside(a, address.bytes, address.bits)) return true;
		}
		return false;
	}

	// IPv4, written as such or inside an IPv6 address
	static bool publicV4Here(const unsigned char* a) {
		// Clash and other proxies hand out these for every name, nobody's home is on them
		static const unsigned char fake[4] = { 198, 18, 0, 0 };
		if (inside(a, fake, 15)) return true;
		return publicV4(a) && !onThisNetwork(AF_INET, a);
	}

	bool publicAddress(const std::wstring& text) {
		unsigned char a[16] = {};
		if (InetPtonW(AF_INET, text.c_str(), a) == 1) return publicV4Here(a);
		if (InetPtonW(AF_INET6, text.c_str(), a) != 1) return false;

		// an IPv4 address written as IPv6 goes where the IPv4 one goes
		static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
		static const unsigned char nat64[12] = { 0, 0x64, 0xFF, 0x9B };
		static const unsigned char sixToFour[2] = { 0x20, 0x02 };
		if (inside(a, mapped, 96) || inside(a, nat64, 96)) return publicV4Here(a + 12);
		if (inside(a, sixToFour, 16) && !publicV4Here(a + 2)) return false;

		static const unsigned char linkLocal[2] = { 0xFE, 0x80 };   // and the old site-local, fe80::/9
		static const unsigned char unique[1] = { 0xFC };            // fc00::/7, the home network's own
		if (a[0] == 0 || a[0] == 0xFF || inside(a, linkLocal, 9) || inside(a, unique, 7)) return false;
		return !onThisNetwork(AF_INET6, a);
	}

	// every address the name looks up to here is public, and it looked up
	static bool publicName(std::wstring host) {
		if (host.size() > 2 && host.front() == L'[' && host.back() == L']') host = host.substr(1, host.size() - 2);
		ADDRINFOW hints = {};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		ADDRINFOW* found = nullptr;
		if (GetAddrInfoW(host.c_str(), nullptr, &hints, &found) != 0 || !found) return false;
		bool ok = true;
		for (ADDRINFOW* at = found; at && ok; at = at->ai_next) {
			wchar_t text[64] = {};
			void* address = at->ai_family == AF_INET
				? (void*)&((sockaddr_in*)at->ai_addr)->sin_addr
				: (void*)&((sockaddr_in6*)at->ai_addr)->sin6_addr;
			ok = InetNtopW(at->ai_family, address, text, 64) && publicAddress(text);
		}
		FreeAddrInfoW(found);
		return ok;
	}

	Stopper::Stopper() : event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

	Stopper::~Stopper() {
		if (event) CloseHandle((HANDLE)event);
	}

	void Stopper::stop() {
		done = true;
		if (event) SetEvent((HANDLE)event);
	}

	// One request. WinHTTP runs each step on its own threads and tells
	// onStatus when it's done; the caller waits for that or for its stopper.
	// Closing an async request is how WinHTTP cancels it from any thread, and
	// the Call lives until WinHTTP says the handle is closed, so nothing writes
	// into it after that.
	struct Call {
		HANDLE stepDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		DWORD available = 0;
		DWORD read = 0;
		bool failed = false;
		std::vector<char> piece;

		HINTERNET request = nullptr;
		std::atomic<bool> handleClosed{ false };
		// An extension's request has a session of its own: connections are kept
		// per session, so none rides on one opened before and skips the check.
		HINTERNET ownSession = nullptr;

		// Only the internet, for extensions: the address WinHTTP connects to, and
		// the one the connection has when the request goes out, are checked
		// before anything is sent, after every redirect too. Through a proxy
		// that address is the proxy's, so the name asked for is looked up here
		// instead (it can still look up differently for the proxy).
		bool publicOnly = false;
		std::wstring host;       // the one asked for now, a redirect changes it
		std::wstring resolving;  // what WinHTTP looks up: host, or the proxy
		bool viaProxy = false;
		std::string refused;     // the place refused, for the log
		Sites sites;
		bool notListed = false;
		// GetTickCount64 when the whole request has to be done, 0 for never
		ULONGLONG deadline = 0;
		bool timedOut = false;
		size_t maxBody = 0;
		bool tooBig = false;

		~Call() {
			CloseHandle(stepDone);
			CloseHandle(closed);
		}

		// once only, from whichever thread gets here first
		void close() {
			if (request && !handleClosed.exchange(true)) WinHttpCloseHandle(request);
		}

		static HANDLE stopEvent(const Stopper* stopper) {
			return stopper ? (HANDLE)stopper->event : nullptr;
		}
	};

	static void refuse(Call* call, const std::string& place) {
		call->refused = place;
		call->failed = true;
		call->close();
		SetEvent(call->stepDone);
	}

	bool onSites(const std::string& host, const Sites& sites) {
		if (!sites.only) return true;
		std::string name = host;
		for (char& c : name) c = (char)std::tolower((unsigned char)c);
		for (const std::string& site : sites.names) {
			if (name == site) return true;
			if (name.size() > site.size() && name.compare(name.size() - site.size(), site.size(), site) == 0
				&& name[name.size() - site.size() - 1] == '.') return true;
		}
		return false;
	}

	// false and refused when the host isn't one of the call's sites
	static bool checkSite(Call* call) {
		if (onSites(narrow(call->host), call->sites)) return true;
		call->notListed = true;
		refuse(call, narrow(call->host));
		return false;
	}

	// WinHTTP looking up a name other than the host's is it looking up the proxy
	static void checkAddress(Call* call, const wchar_t* address) {
		bool direct = call->resolving.empty() || _wcsicmp(call->resolving.c_str(), call->host.c_str()) == 0;
		if (direct && !call->viaProxy) {
			if (!publicAddress(address)) refuse(call, narrow(call->host) + " (" + narrow(address) + ")");
			return;
		}
		call->viaProxy = true;
		if (!publicName(call->host)) refuse(call, narrow(call->host));
	}

	// the connection the request is about to go out on, new or kept from before
	static void checkSending(Call* call) {
		if (call->viaProxy) return;
		WINHTTP_CONNECTION_INFO info = {};
		info.cbSize = sizeof(info);
		DWORD size = sizeof(info);
		wchar_t text[64] = {};
		const SOCKADDR_STORAGE& remote = info.RemoteAddress;
		bool read = WinHttpQueryOption(call->request, WINHTTP_OPTION_CONNECTION_INFO, &info, &size)
			&& (remote.ss_family == AF_INET || remote.ss_family == AF_INET6)
			&& InetNtopW(remote.ss_family, remote.ss_family == AF_INET
				? (void*)&((const sockaddr_in*)&remote)->sin_addr
				: (void*)&((const sockaddr_in6*)&remote)->sin6_addr, text, 64);
		if (!read) refuse(call, narrow(call->host));
		else if (!publicAddress(text)) refuse(call, narrow(call->host) + " (" + narrow(text) + ")");
	}

	static void checkRedirect(Call* call, const wchar_t* url) {
		wchar_t host[256] = {};
		URL_COMPONENTS parts = {};
		parts.dwStructSize = sizeof(parts);
		parts.lpszHostName = host;
		parts.dwHostNameLength = 256;
		if (!url || !WinHttpCrackUrl(url, 0, 0, &parts)) {
			refuse(call, url ? narrow(url) : "a redirect");
			return;
		}
		call->host = host;
		call->resolving.clear();
		if (!checkSite(call)) return;
		// a proxy keeps the same connection, so no new address comes to check
		if (call->viaProxy && !publicName(call->host)) refuse(call, narrow(call->host));
	}

	static void CALLBACK onStatus(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD length) {
		Call* call = (Call*)context;
		if (!call) return;
		switch (status) {
		case WINHTTP_CALLBACK_STATUS_RESOLVING_NAME:
			if (call->publicOnly && info) call->resolving = (const wchar_t*)info;
			break;
		case WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER:
		case WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER:
			if (call->publicOnly && !call->failed) {
				if (info) checkAddress(call, (const wchar_t*)info);
				else refuse(call, narrow(call->host));
			}
			break;
		case WINHTTP_CALLBACK_STATUS_SENDING_REQUEST:
			if (call->publicOnly && !call->failed) checkSending(call);
			break;
		case WINHTTP_CALLBACK_STATUS_REDIRECT:
			if ((call->publicOnly || call->sites.only) && !call->failed) checkRedirect(call, (const wchar_t*)info);
			break;
		case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
		case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
			SetEvent(call->stepDone);
			break;
		case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
			call->available = *(DWORD*)info;
			SetEvent(call->stepDone);
			break;
		case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
			call->read = length;
			SetEvent(call->stepDone);
			break;
		case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
			call->failed = true;
			SetEvent(call->stepDone);
			break;
		case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
			SetEvent(call->closed);
			break;
		}
	}

	// waits for the step WinHTTP is on, false when it failed, or stop or the deadline came first
	static bool waitStep(Call& call, const Stopper* stopper) {
		HANDLE events[2] = { call.stepDone, Call::stopEvent(stopper) };
		DWORD wait = INFINITE;
		if (call.deadline) {
			ULONGLONG now = GetTickCount64();
			wait = now >= call.deadline ? 0 : (DWORD)(call.deadline - now);
		}
		DWORD which = WaitForMultipleObjects(events[1] ? 2 : 1, events, FALSE, wait);
		if (which == WAIT_TIMEOUT) call.timedOut = true;
		return which == WAIT_OBJECT_0 && !call.failed;
	}

	static void setLimits(Call& call, const Limits& limits) {
		call.publicOnly = limits.publicOnly;
		call.sites = limits.sites;
		call.maxBody = limits.maxBody;
		if (limits.deadlineMs > 0) call.deadline = GetTickCount64() + limits.deadlineMs;
	}

	// Connects, sends and waits for the reply's headers. call.request and
	// connection are left for the caller to read on and close with finish.
	static bool start(Call& call, const wchar_t* verb, const std::string& url, const std::wstring& headers, const std::string& body,
		bool webSocket, const Stopper* stopper, HINTERNET& connection, int& status) {
		connection = nullptr;
		if (stopper && stopper->stopped()) return false;
		HINTERNET session = call.publicOnly || !testProxy.empty() ? (call.ownSession = openSession(true)) : sharedSession();
		if (!session) return false;

		std::wstring wideUrl = widen(url);
		URL_COMPONENTS parts = {};
		parts.dwStructSize = sizeof(parts);
		wchar_t host[256] = {};
		wchar_t path[2048] = {};
		parts.lpszHostName = host;
		parts.dwHostNameLength = 256;
		parts.lpszUrlPath = path;
		parts.dwUrlPathLength = 2048;
		wchar_t extra[2048] = {};
		parts.lpszExtraInfo = extra;
		parts.dwExtraInfoLength = 2048;
		if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) return false;
		call.host = host;
		if (!checkSite(&call)) return false;

		std::wstring target = std::wstring(path) + extra;
		connection = WinHttpConnect(session, host, parts.nPort, 0);
		if (!connection) return false;
		call.request = WinHttpOpenRequest(connection, verb, target.c_str(), nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
		if (!call.request) return false;

		DWORD_PTR context = (DWORD_PTR)&call;
		WinHttpSetOption(call.request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context));
		WinHttpSetStatusCallback(call.request, onStatus, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);
		if (webSocket && !WinHttpSetOption(call.request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return false;

		DWORD size = (DWORD)body.size();
		if (!(WinHttpSendRequest(call.request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : (DWORD)-1L,
			size ? (LPVOID)body.data() : WINHTTP_NO_REQUEST_DATA, size, size, context) && waitStep(call, stopper)
			&& WinHttpReceiveResponse(call.request, nullptr) && waitStep(call, stopper))) return false;

		DWORD code = 0, codeSize = sizeof(code);
		WinHttpQueryHeaders(call.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize, WINHTTP_NO_HEADER_INDEX);
		status = (int)code;
		return true;
	}

	// Whatever step it was on is cancelled; the Call goes once WinHTTP is done
	// with it. keepSession: the web socket goes on using it.
	static void finish(Call* call, HINTERNET connection, bool keepSession = false) {
		bool opened = call->request != nullptr;
		HINTERNET session = keepSession ? nullptr : call->ownSession;
		call->close();
		if (!opened || WaitForSingleObject(call->closed, 5000) == WAIT_OBJECT_0) delete call;
		else std::cerr << "WinHTTP didn't close a request in 5 s, its buffer is left to it" << std::endl;
		if (connection) WinHttpCloseHandle(connection);
		if (session) WinHttpCloseHandle(session);
	}

	// the body after start; true only when it's whole
	static bool readBody(Call& call, Response& out, const Progress& progress, const Stopper* stopper) {
		out.body.clear();
		// when the server says how long the body is, a shorter one is cut short
		uint64_t expected = 0;
		DWORD expectedSize = sizeof(expected);
		bool sized = WinHttpQueryHeaders(call.request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64, WINHTTP_HEADER_NAME_BY_INDEX,
			&expected, &expectedSize, WINHTTP_NO_HEADER_INDEX);
		if (call.maxBody && sized && expected > call.maxBody) {
			call.tooBig = true;
			return false;
		}

		// in async mode the sizes come in the callback, the pointers stay null
		while (WinHttpQueryDataAvailable(call.request, nullptr) && waitStep(call, stopper)) {
			if (call.available == 0) return !sized || out.body.size() == expected;
			if (call.maxBody && out.body.size() + call.available > call.maxBody) {
				call.tooBig = true;
				return false;
			}
			call.piece.resize(call.available);
			if (!WinHttpReadData(call.request, call.piece.data(), call.available, nullptr) || !waitStep(call, stopper)) return false;
			out.body.append(call.piece.data(), call.read);
			if (progress && !progress(out.body.size())) return false;
		}
		return false;
	}

	static bool request(const wchar_t* verb, const std::string& url, const std::wstring& headers, const std::string& body, Response& out,
		const Progress& progress, Stopper* stopper, const Limits& limits) {
		out.refused.clear();
		out.notListed = false;
		out.timedOut = false;
		out.tooBig = false;
		Call* call = new Call();
		setLimits(*call, limits);
		HINTERNET connection = nullptr;
		int status = 0;
		bool ok = start(*call, verb, url, headers, body, false, stopper, connection, status);
		if (ok) {
			out.status = status;
			ok = readBody(*call, out, progress, stopper);
		}
		out.refused = call->refused;
		out.notListed = call->notListed;
		out.timedOut = call->timedOut;
		out.tooBig = call->tooBig;
		finish(call, connection);
		if (stopper && stopper->stopped()) ok = false;
		return ok;
	}

	static const wchar_t* HOYOLAB_HEADERS = L"Origin: https://act.hoyolab.com\r\nReferer: https://act.hoyolab.com/\r\n";

	bool get(const std::string& url, Response& out, bool hoyolab, const Progress& progress, Stopper* stopper, const Limits& limits) {
		return request(L"GET", url, hoyolab ? HOYOLAB_HEADERS : L"", "", out, progress, stopper, limits);
	}

	bool post(const std::string& url, const std::string& body, Response& out, bool hoyolab, const Limits& limits, Stopper* stopper) {
		std::wstring headers = L"Content-Type: application/json\r\n";
		if (hoyolab) headers += HOYOLAB_HEADERS;
		return request(L"POST", url, headers, body, out, nullptr, stopper, limits);
	}

    // The socket's own steps, told by onSocketStatus. Lives until WinHTTP says
    // the socket's handle is closed.
    struct SocketCall {
        HANDLE readDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE writeDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE closeDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE;
        bool readFailed = false;
        bool writeFailed = false;

        ~SocketCall() {
            CloseHandle(readDone);
            CloseHandle(writeDone);
            CloseHandle(closeDone);
            CloseHandle(closed);
        }
    };

    static void CALLBACK onSocketStatus(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD) {
        SocketCall* call = (SocketCall*)context;
        if (!call) return;
        switch (status) {
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: {
            auto* done = (WINHTTP_WEB_SOCKET_STATUS*)info;
            call->read = done->dwBytesTransferred;
            call->type = done->eBufferType;
            SetEvent(call->readDone);
            break;
        }
        case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
            SetEvent(call->writeDone);
            break;
        case WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE:
            SetEvent(call->closeDone);
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
            auto* result = (WINHTTP_WEB_SOCKET_ASYNC_RESULT*)info;
            if (result->Operation == WINHTTP_WEB_SOCKET_RECEIVE_OPERATION) {
                call->readFailed = true;
                SetEvent(call->readDone);
            }
            else if (result->Operation == WINHTTP_WEB_SOCKET_SEND_OPERATION) {
                call->writeFailed = true;
                SetEvent(call->writeDone);
            }
            else {
                SetEvent(call->closeDone);
            }
            break;
        }
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
            SetEvent(call->closed);
            break;
        }
    }

    WebSocket::~WebSocket() {
        close();
    }

    bool WebSocket::connect(const std::string& url, bool hoyolab, const Limits& limits, Stopper* stopper)
    {
        close();
        refused.clear();
        notListed = false;
        timedOut = false;
        overflow = false;
        {
            std::lock_guard<std::mutex> lock(receiveMutex);
            messages.clear();
        }

        std::string httpUrl;

        if (url.rfind("wss://", 0) == 0) {
            httpUrl = "https://" + url.substr(6);
        }
        else if (url.rfind("ws://", 0) == 0) {
            httpUrl = "http://" + url.substr(5);
        }
        else {
            std::cerr << "WebSocket: invalid URL scheme" << std::endl;
            return false;
        }

        std::wstring headers = hoyolab
            ? L"Origin: https://www.hoyolab.com\r\nReferer: https://www.hoyolab.com/\r\n"
            : L"";

        // the upgrade is a request like any other, checked the same way
        Call* call = new Call();
        setLimits(*call, limits);
        HINTERNET connectionHandle = nullptr;
        int status = 0;
        bool ok = start(*call, L"GET", httpUrl, headers, "", true, stopper, connectionHandle, status);
        refused = call->refused;
        notListed = call->notListed;
        timedOut = call->timedOut;

        if (!ok) {
            if (refused.empty() && !(stopper && stopper->stopped()))
                std::cerr << "WebSocket: couldn't connect: " << GetLastError() << std::endl;
            finish(call, connectionHandle);
            return false;
        }

        if (status != 101) {
            std::cerr << "WebSocket: server returned HTTP "
                << status << std::endl;
            finish(call, connectionHandle);
            return false;
        }

        SocketCall* socketCall = new SocketCall();
        HINTERNET socketHandle = WinHttpWebSocketCompleteUpgrade(
            call->request,
            (DWORD_PTR)socketCall
        );

        if (!socketHandle) {
            std::cerr << "WebSocket: WinHttpWebSocketCompleteUpgrade failed: "
                << GetLastError() << std::endl;
            delete socketCall;
            finish(call, connectionHandle);
            return false;
        }

        WinHttpSetStatusCallback(socketHandle, onSocketStatus, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0);

        // the request is done with, its connection and session stay for the socket
        ownSession = call->ownSession;
        finish(call, nullptr, true);

        connection = connectionHandle;
        socket = socketHandle;
        steps = socketCall;

        running = true;
        connected = true;

        receiveThread = std::thread(
            &WebSocket::receiveLoop,
            this
        );

        return true;
    }

    bool WebSocket::send(const std::string& message) {
        if (!connected || !socket)
            return false;

        std::lock_guard<std::mutex> lock(sendMutex);

        if (!connected || !socket)
            return false;

        steps->writeFailed = false;
        DWORD result = WinHttpWebSocketSend(
            socket,
            WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            (PVOID)message.data(),
            static_cast<DWORD>(message.size())
        );

        // WinHTTP reads the message until it says it's sent (or its own timeout ends it)
        if (result != ERROR_SUCCESS ||
            WaitForSingleObject(steps->writeDone, INFINITE) != WAIT_OBJECT_0 ||
            steps->writeFailed) {
            connected = false;
            return false;
        }

        return true;
    }

    bool WebSocket::receive(
        std::string& message,
        size_t maxMessageSize
    ) {
        message.clear();

        if (!socket)
            return false;

        std::string buffer;
        buffer.reserve(4096);

        std::vector<char> chunk(16 * 1024);

        bool complete = false;

        while (!complete) {
            steps->readFailed = false;
            DWORD result = WinHttpWebSocketReceive(
                socket,
                chunk.data(),
                static_cast<DWORD>(chunk.size()),
                nullptr,
                nullptr
            );

            // closing the socket ends the wait with readFailed
            if (result != ERROR_SUCCESS ||
                WaitForSingleObject(steps->readDone, INFINITE) != WAIT_OBJECT_0 ||
                steps->readFailed) {
                connected = false;
                return false;
            }

            DWORD bytesRead = steps->read;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type = steps->type;

            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                connected = false;
                return false;
            }

            if (buffer.size() + bytesRead > maxMessageSize) {
                connected = false;
                return false;
            }

            if (bytesRead > 0)
                buffer.append(chunk.data(), bytesRead);

            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                complete = true;
            }
        }

        message = std::move(buffer);
        return true;
    }

    void WebSocket::receiveLoop() {
        while (running && connected) {
            std::string message;

            if (!receive(message))
                break;

            if (message.empty())
                continue;

            {
                std::lock_guard<std::mutex> lock(receiveMutex);
                if (maxQueued && messages.size() >= maxQueued) {
                    overflow = true;
                    break;
                }
                messages.push_back(std::move(message));
            }
        }

        connected = false;
        running = false;
    }

    bool WebSocket::poll(std::string& message) {
        std::lock_guard<std::mutex> lock(receiveMutex);

        if (messages.empty())
            return false;

        message = std::move(messages.front());
        messages.pop_front();

        return true;
    }

    std::vector<std::string> WebSocket::pollAll() {
        std::vector<std::string> result;

        std::lock_guard<std::mutex> lock(receiveMutex);

        while (!messages.empty()) {
            result.push_back(std::move(messages.front()));
            messages.pop_front();
        }

        return result;
    }

    void WebSocket::close() {
        running = false;
        connected = false;

        if (socket) {
            // says goodbye if the server answers soon; closing the handle then
            // ends the receive the loop waits on
            if (WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0) == ERROR_SUCCESS)
                WaitForSingleObject(steps->closeDone, 1000);
            WinHttpCloseHandle(socket);
        }

        if (receiveThread.joinable())
            receiveThread.join();

        if (socket) {
            if (WaitForSingleObject(steps->closed, 5000) == WAIT_OBJECT_0)
                delete steps;
            else
                std::cerr << "WinHTTP didn't close a web socket in 5 s, its buffer is left to it" << std::endl;
            socket = nullptr;
            steps = nullptr;
        }

        if (connection) {
            WinHttpCloseHandle(connection);
            connection = nullptr;
        }

        if (ownSession) {
            WinHttpCloseHandle(ownSession);
            ownSession = nullptr;
        }
    }

    bool WebSocket::isConnected() const {
        return connected;
    }
}
