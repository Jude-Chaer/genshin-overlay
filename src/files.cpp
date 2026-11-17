#include "files.hpp"
#include "net.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <exception>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include <zlib.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace Files {
	const std::string RELEASE = "https://github.com/Jude-Chaer/genshin-overlay/releases/download/map-index/";
	static const std::string API = "https://sg-public-api.hoyolab.com/common/map_user/ys_obc";
	static const std::string STATIC_API = "https://sg-public-api-static.hoyolab.com/common/map_user/ys_obc";

	std::string readText(const fs::path& path) {
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file) return std::string();
		std::streamoff size = file.tellg();
		if (size <= 0) return std::string();
		std::string text((size_t)size, '\0');
		file.seekg(0);
		if (!file.read(text.data(), size)) return std::string();
		return text;
	}

	bool writeFile(const fs::path& path, const std::string& data) {
		std::error_code ec;
		fs::create_directories(path.parent_path(), ec);
		// two threads, or two overlays, can be saving the same file
		fs::path tmp = path;
		tmp += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(GetCurrentThreadId()) + ".part";
		{
			std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
			if (!file) return false;
			file.write(data.data(), (std::streamsize)data.size());
			// the last part only reaches the disk on close, a full disk shows there
			file.close();
			if (!file) {
				fs::remove(tmp, ec);
				return false;
			}
		}
		fs::rename(tmp, path, ec);
		if (!ec) return true;
		fs::remove(tmp, ec);
		return false;
	}

	bool gunzip(const std::string& in, std::string& out, size_t expectedSize) {
		z_stream stream = {};
		if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) return false;
		out.resize(expectedSize);
		stream.next_in = (Bytef*)in.data();
		stream.avail_in = (uInt)in.size();
		stream.next_out = (Bytef*)out.data();
		stream.avail_out = (uInt)out.size();
		int result = inflate(&stream, Z_FINISH);
		inflateEnd(&stream);
		return result == Z_STREAM_END && stream.total_out == expectedSize;
	}

	bool gzip(const std::string& in, std::string& out) {
		z_stream stream = {};
		if (deflateInit2(&stream, 9, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
		out.resize(deflateBound(&stream, (uLong)in.size()));
		stream.next_in = (Bytef*)in.data();
		stream.avail_in = (uInt)in.size();
		stream.next_out = (Bytef*)out.data();
		stream.avail_out = (uInt)out.size();
		int result = deflate(&stream, Z_FINISH);
		out.resize(stream.total_out);
		deflateEnd(&stream);
		return result == Z_STREAM_END;
	}

	std::string sha256(const std::string& data) {
		unsigned char hash[32] = {};
		BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), hash, sizeof(hash));
		static const char* hex = "0123456789abcdef";
		std::string out;
		for (unsigned char c : hash) {
			out += hex[c >> 4];
			out += hex[c & 15];
		}
		return out;
	}

	std::string utcNow(long long secondsAgo) {
		char built[32];
		std::time_t now = std::time(nullptr) - (std::time_t)secondsAgo;
		std::tm utc;
		gmtime_s(&utc, &now);
		std::strftime(built, sizeof(built), "%Y-%m-%dT%H:%M:%SZ", &utc);
		return built;
	}

	// meta files come from the internet, so every value is checked before it's used
	Meta readMeta(const std::string& body, int format, json& meta) {
		meta = json::parse(body, nullptr, false);
		if (!meta.is_object() || !meta.contains("format") || !meta["format"].is_number_integer()) return Meta::Broken;
		int64_t found = meta["format"].get<int64_t>();
		if (found > format) return Meta::Newer;
		return found == format ? Meta::Ok : Meta::Broken;
	}

	Meta getMeta(const std::string& name, int format, json& meta, Net::Stopper* stop) {
		Net::Response response;
		if (!Net::get(RELEASE + name, response, false, nullptr, stop)) return Meta::Unreached;
		if (response.status == 404) return Meta::Missing;
		if (response.status != 200) return Meta::Unreached;
		return readMeta(response.body, format, meta);
	}

	std::string stringField(const json& meta, const char* key, const std::string& otherwise) {
		return meta.is_object() && meta.contains(key) && meta[key].is_string() ? meta[key].get<std::string>() : otherwise;
	}

	size_t sizeField(const json& meta, const char* key) {
		return meta.is_object() && meta.contains(key) && meta[key].is_number_unsigned() ? meta[key].get<size_t>() : 0;
	}

	bool unpack(const std::string& packed, const json& meta, size_t maxRawSize, std::string& raw, std::string& problem) {
		std::string hash = stringField(meta, "sha256");
		size_t rawSize = sizeField(meta, "rawSize");
		if (hash.empty() || rawSize == 0 || rawSize > maxRawSize) {
			problem = "its meta file has no sha256 or a wrong size";
			return false;
		}
		if (!gunzip(packed, raw, rawSize)) {
			problem = "it doesn't unzip to its size";
			return false;
		}
		if (sha256(raw) != hash) {
			problem = "its sha256 isn't the one in its meta file";
			return false;
		}
		return true;
	}

	bool getChecked(const std::string& name, const json& meta, size_t maxRawSize, std::string& raw, std::string& problem, Net::Stopper* stop) {
		Net::Response response;
		if (!Net::get(RELEASE + name, response, false, nullptr, stop) || response.status != 200) {
			problem = "it couldn't be downloaded";
			return false;
		}
		return unpack(response.body, meta, maxRawSize, raw, problem);
	}

	void inParallel(size_t count, const std::function<void(size_t i)>& each, const std::function<bool()>& stop,
		const std::function<void(size_t done)>& tick) {
		const int THREADS = 16;
		std::atomic<size_t> next{ 0 }, done{ 0 };
		// the first exception in a worker ends them all and is thrown here after
		std::atomic<bool> failed{ false };
		std::exception_ptr error;
		std::mutex errorLock;
		auto stopped = [&]() { return failed || (stop && stop()); };
		auto worker = [&]() {
			while (!stopped()) {
				size_t i = next.fetch_add(1);
				if (i >= count) return;
				try {
					each(i);
				}
				catch (...) {
					std::lock_guard<std::mutex> hold(errorLock);
					if (!error) error = std::current_exception();
					failed = true;
					return;
				}
				done++;
			}
		};
		std::vector<std::thread> pool;
		for (int t = 0; t < THREADS; t++) pool.emplace_back(worker);
		if (tick) {
			while (done < count && !stopped()) {
				tick(done);
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
			}
		}
		for (auto& thread : pool) thread.join();
		if (error) std::rethrow_exception(error);
	}

	bool getJson(const std::string& path, json& data, bool isStatic, Net::Stopper* stop) {
		Net::Response response;
		if (!Net::get((isStatic ? STATIC_API : API) + path, response, true, nullptr, stop) || response.status != 200) return false;

		json body = json::parse(response.body, nullptr, false);
		if (body.is_discarded() || body.value("retcode", -1) != 0 || !body.contains("data")) return false;
		data = body["data"];
		return true;
	}
}
