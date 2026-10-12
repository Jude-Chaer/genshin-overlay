#include "map_data.hpp"
#include "net.hpp"
#include "locator/map_locator.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <nlohmann/json.hpp>
#include <zlib.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace MapData {
	static const std::string API = "https://sg-public-api.hoyolab.com/common/map_user/ys_obc";
	static const std::string STATIC_API = "https://sg-public-api-static.hoyolab.com/common/map_user/ys_obc";
	static const std::string TILES = "https://act-webstatic.hoyoverse.com/map_manage/map/";
	static const std::string RELEASE ="https://github.com/Jude-Chaer/genshin-overlay/releases/download/map-index/";
	static const std::string INDEX_FILE = "map-index.bin.gz";
	static const std::string META_FILE = "map-index.json";
	static constexpr int META_FORMAT = 1;
	static constexpr int DOWNLOAD_THREADS = 16;

	// Teyvat, Enkanomiya, The Chasm, and the separate maps from later versions.
	// Ids without their own tiles get skipped.
	static constexpr int MAP_IDS[] = { 2, 7, 9, 34, 36, 37, 40 };

	static bool getJson(const std::string& path, json& data, bool isStatic = false) {
		Net::Response response;
		if (!Net::get((isStatic ? STATIC_API : API) + path, response, true) || response.status != 200) return false;

		json body = json::parse(response.body, nullptr, false);
		if (body.is_discarded() || body.value("retcode", -1) != 0 || !body.contains("data")) return false;
		data = body["data"];
		return true;
	}

	// numbers go into the manifest exactly as HoYoLAB sends them
	static std::string number(const json& value) {
		return value.dump();
	}

	static std::string readText(const fs::path& path) {
		std::ifstream file(path, std::ios::binary);
		std::ostringstream text;
		text << file.rdbuf();
		return text.str();
	}

	static bool writeFile(const fs::path& path, const std::string& data) {
		std::error_code ec;
		fs::create_directories(path.parent_path(), ec);
		fs::path tmp = path;
		tmp += ".part";
		{
			std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
			if (!file) return false;
			file.write(data.data(), (std::streamsize)data.size());
			if (!file) return false;
		}
		fs::rename(tmp, path, ec);
		return !ec;
	}

	Manifest readManifest(const fs::path& folder) {
		Manifest out;
		std::map<std::string, std::string> urls;
		std::ifstream urlFile(folder / "overlay_urls.tsv");
		std::string line;
		while (std::getline(urlFile, line)) {
			size_t tab = line.find('\t');
			if (tab != std::string::npos) urls[line.substr(0, tab)] = line.substr(tab + 1);
		}

		std::ifstream manifest(folder / "manifest.tsv");
		while (std::getline(manifest, line)) {
			std::istringstream fields(line);
			std::string kind, path;
			fields >> kind;
			// grid <map> <cols> <rows> <tile size> <origin x> <origin y> <dir>
			if (kind == "grid") {
				int id = 0;
				Grid grid;
				if (!(fields >> id >> grid.cols >> grid.rows >> grid.tileSize >> grid.originX >> grid.originY >> path)) continue;
				grid.dir = folder / fs::u8path(path);
				out.grids[id] = grid;
			}
			// image <map> <group> <floor> <left> <top> <path> <right> <bottom>
			else if (kind == "image") {
				Floor floor;
				if (!(fields >> floor.mapId >> floor.groupId >> floor.floorId >> floor.left >> floor.top >> path >> floor.right >> floor.bottom)) continue;
				floor.path = folder / fs::u8path(path);
				floor.url = urls[path];
				out.floors.push_back(floor);
			}
		}
		return out;
	}

	fs::path tilePath(const Grid& grid, int x, int y) {
		return grid.dir / (std::to_string(x) + "_" + std::to_string(y) + ".webp");
	}

	bool fetch(const std::string& url, const fs::path& path) {
		if (fs::exists(path)) return true;
		Net::Response response;
		if (url.empty() || !Net::get(url, response, true)) return false;
		// an empty file stops us asking again, tiles outside the drawn map don't exist
		if (response.status == 404) return writeFile(path, "");
		return response.status == 200 && writeFile(path, response.body);
	}

	bool fetchTile(int mapId, const Grid& grid, int x, int y) {
		// dir is tiles/<id>_<version>
		std::string dir = grid.dir.filename().string();
		return fetch(TILES + std::to_string(mapId) + "/" + dir.substr(dir.find('_') + 1) + "/" + std::to_string(x) + "_" + std::to_string(y) + "_P0.webp",
			tilePath(grid, x, y));
	}

	cv::Mat readGray(const fs::path& path, int shrunkBy) {
		std::ifstream file(path, std::ios::binary);
		std::vector<uchar> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (bytes.empty()) return {};
		cv::Mat gray;
		try {
			cv::Mat image = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
			if (image.empty() || image.depth() != CV_8U) return {};
			if (image.channels() == 4) {
				std::vector<cv::Mat> parts;
				cv::split(image, parts);
				cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
				cv::multiply(gray, parts[3], gray, 1.0 / 255.0);
			}
			else if (image.channels() == 3) cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
			else gray = image;
			if (shrunkBy > 1) cv::resize(gray, gray, cv::Size(std::max(1, gray.cols / shrunkBy), std::max(1, gray.rows / shrunkBy)), 0, 0, cv::INTER_AREA);
		}
		catch (...) {
			return {};
		}
		return gray;
	}

	// one tile as gray, shrunk, empty if there's no such tile
	const cv::Mat& Tiles::tile(int mapId, const Grid& grid, int x, int y, int shrunkBy) {
		Key key{ mapId, x, y, shrunkBy };
		auto it = m_tiles.find(key);
		if (it != m_tiles.end()) return it->second;

		if (m_fetch) fetchTile(mapId, grid, x, y);
		cv::Mat image = readGray(tilePath(grid, x, y), shrunkBy);

		if (m_order.size() >= m_kept) {
			m_tiles.erase(m_order.front());
			m_order.pop_front();
		}
		m_order.push_back(key);
		return m_tiles[key] = image;
	}

	cv::Mat Tiles::Draw(int mapId, const Grid& grid, double left, double top, int side, double perPixel, int shrunkBy) {
		int size = grid.tileSize / shrunkBy;
		// drawn pixel u lands on shrunk tile pixel a * u + b, pixel middles lined up
		double a = perPixel / shrunkBy;
		double bx = (left + 0.5 * perPixel + grid.originX) / shrunkBy - 0.5;
		double by = (top + 0.5 * perPixel + grid.originY) / shrunkBy - 0.5;
		int x0 = (int)std::floor(bx) - 2, y0 = (int)std::floor(by) - 2;
		int x1 = (int)std::ceil(bx + a * (side - 1)) + 3, y1 = (int)std::ceil(by + a * (side - 1)) + 3;

		cv::Mat area(y1 - y0, x1 - x0, CV_8U, cv::Scalar(0));
		for (int ty = std::max(0, (int)std::floor((double)y0 / size)); ty <= std::min(grid.rows - 1, (int)std::floor((double)(y1 - 1) / size)); ++ty) {
			for (int tx = std::max(0, (int)std::floor((double)x0 / size)); tx <= std::min(grid.cols - 1, (int)std::floor((double)(x1 - 1) / size)); ++tx) {
				const cv::Mat& image = tile(mapId, grid, tx, ty, shrunkBy);
				if (image.empty()) continue;
				cv::Rect in(tx * size, ty * size, size, size);
				cv::Rect both = in & cv::Rect(x0, y0, x1 - x0, y1 - y0);
				if (both.empty()) continue;
				image(both - in.tl()).copyTo(area(both - cv::Point(x0, y0)));
			}
		}
		cv::Mat drawn;
		cv::Matx23d to(a, 0.0, bx - x0, 0.0, a, by - y0);
		cv::warpAffine(area, drawn, to, cv::Size(side, side), cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT, cv::Scalar(0));
		return drawn;
	}

	void Tiles::Clear() {
		m_tiles.clear();
		m_order.clear();
	}

	bool makePlan(Plan& plan) {
		std::vector<std::string> lines = { "# genshin-overlay map manifest v1" };
		plan.files.clear();
		plan.tileDirs.clear();

		try {
			for (int id : MAP_IDS) {
				std::string query = "map_id=" + std::to_string(id) + "&app_sn=ys_obc&lang=en-us";
				json info;
				if (!getJson("/v1/map/info?" + query, info)) return false;

				const json& detail = info["info"]["detail_v2"];
				if (!detail.is_object() || !detail.contains("total_size") || !detail.contains("origin")) continue;
				const json& size = detail["total_size"];
				const json& origin = detail["origin"];
				std::string version = detail.value("map_version", "");
				if (size.size() < 2 || origin.size() < 2 || version.empty()) continue;

				// the map is cut into 256px tiles named x_y
				const int tile = 256;
				int cols = size[0].get<int>() / tile;
				int rows = size[1].get<int>() / tile;
				std::string dir = "tiles/" + std::to_string(id) + "_" + version;
				plan.tileDirs.insert(std::to_string(id) + "_" + version);
				for (int y = 0; y < rows; y++) {
					for (int x = 0; x < cols; x++) {
						std::string name = std::to_string(x) + "_" + std::to_string(y);
						plan.files.push_back({
							TILES + std::to_string(id) + "/" + version + "/" + name + "_P0.webp",
							dir + "/" + name + ".webp"
							});
					}
				}
				lines.push_back("grid\t" + std::to_string(id) + "\t" + std::to_string(cols) + "\t" + std::to_string(rows) + "\t"
					+ std::to_string(tile) + "\t" + number(origin[0]) + "\t" + number(origin[1]) + "\t" + dir);

				// underground floors, each one is a single image placed over the map
				json groups;
				if (!getJson("/v2/map/point_group?" + query, groups)) return false;
				for (const json& group : groups.value("list", json::array())) {
					for (const json& floor : group.value("floors", json::array())) {
						if (!floor.contains("overlay") || !floor["overlay"].is_object()) continue;
						const json& overlay = floor["overlay"];
						std::string url = overlay.value("url", "");
						if (url.empty()) continue;

						// the file name is a hash of the image, so a redrawn floor gets a new name
						std::string fileName = url.substr(url.find_last_of('/') + 1);
						std::string path = "overlays/" + number(floor["id"]) + "_" + fileName;
						plan.files.push_back({ url, path });
						lines.push_back("image\t" + std::to_string(id) + "\t" + number(group["id"]) + "\t" + number(floor["id"]) + "\t"
							+ number(overlay["l_x"]) + "\t" + number(overlay["l_y"]) + "\t" + path + "\t"
							+ number(overlay["r_x"]) + "\t" + number(overlay["r_y"]));
					}
				}
			}
		}
		catch (const json::exception& e) {
			std::cerr << "Unexpected map API response: " << e.what() << std::endl;
			return false;
		}

		plan.manifest.clear();
		for (const auto& line : lines) {
			plan.manifest += line + "\n";
		}
		return true;
	}

	bool downloadMissing(const fs::path& folder, const std::vector<File>& files, const Progress& progress, const std::atomic<bool>* cancel) {
		std::vector<const File*> missing;
		for (const auto& file : files) {
			if (!fs::exists(folder / fs::u8path(file.path))) {
				missing.push_back(&file);
			}
		}
		if (missing.empty()) return true;

		std::atomic<size_t> next{ 0 };
		std::atomic<size_t> done{ 0 };
		std::atomic<bool> failed{ false };
		auto stop = [&]() { return failed || (cancel && cancel->load()); };

		auto worker = [&]() {
			while (!stop()) {
				size_t i = next.fetch_add(1);
				if (i >= missing.size()) return;

				const File& file = *missing[i];
				bool saved = false;
				for (int attempt = 0; attempt < 3 && !saved; attempt++) {
					saved = fetch(file.url, folder / fs::u8path(file.path));
				}
				if (!saved) failed = true;
				done++;
			}
			};

		std::vector<std::thread> pool;
		for (int t = 0; t < DOWNLOAD_THREADS; t++) {
			pool.emplace_back(worker);
		}
		while (done < missing.size() && !stop()) {
			if (progress) progress("Downloading map images", (int)(done * 100 / missing.size()));
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
		for (auto& thread : pool) thread.join();
		return !stop();
	}

	// tile folders from map versions HoYoLAB has replaced
	static void removeOldTiles(const fs::path& folder, const Plan& plan) {
		std::error_code ec;
		fs::path tiles = folder / "tiles";
		if (!fs::exists(tiles)) return;
		for (const auto& entry : fs::directory_iterator(tiles, ec)) {
			if (entry.is_directory() && plan.tileDirs.count(entry.path().filename().string()) == 0) {
				fs::remove_all(entry.path(), ec);
			}
		}
	}

	static bool gunzip(const std::string& in, std::string& out, size_t expectedSize) {
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

	static bool gzip(const std::string& in, std::string& out) {
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

	static std::string sha256(const std::string& data) {
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

	static bool fetchPublishedMeta(json& meta) {
		Net::Response response;
		if (!Net::get(RELEASE + META_FILE, response) || response.status != 200) return false;
		meta = json::parse(response.body, nullptr, false);
		return !meta.is_discarded() && meta.value("format", 0) == META_FORMAT;
	}

	bool downloadPrebuiltIndex(const fs::path& folder, const Plan& plan, const Progress& progress) {
		json meta;
		if (!fetchPublishedMeta(meta) || meta.value("manifest", "") != plan.manifest) return false;

		if (progress) progress("Downloading map index", 0);
		Net::Response response;
		if (!Net::get(RELEASE + meta.value("file", INDEX_FILE), response) || response.status != 200) return false;

		std::string raw;
		size_t rawSize = meta.value("rawSize", (size_t)0);
		if (!gunzip(response.body, raw, rawSize) || sha256(raw) != meta.value("sha256", "")) {
			std::cerr << "The downloaded map index is damaged" << std::endl;
			return false;
		}

		fs::path index = folder / "index.bin";
		if (!writeFile(index, raw)) return false;
		return MapLocator::IndexMatchesManifest(index, plan.manifest);
	}

	bool ensure(const fs::path& folder, const Progress& progress, const std::atomic<bool>* cancel) {
		fs::path manifestPath = folder / "manifest.tsv";

		if (progress) progress("Checking map data", 0);
		Plan plan;
		if (!makePlan(plan)) {
			std::cerr << "Couldn't reach HoYoLAB, using the map data already on disk" << std::endl;
			return fs::exists(manifestPath);
		}

		if (!fs::exists(manifestPath) || readText(manifestPath) != plan.manifest) {
			if (!writeFile(manifestPath, plan.manifest)) return false;
		}

		// where each underground floor image comes from, for drawing them later
		std::string overlayUrls;
		for (const auto& file : plan.files) {
			if (file.path.rfind("overlays/", 0) == 0) {
				overlayUrls += file.path + "\t" + file.url + "\n";
			}
		}
		writeFile(folder / "overlay_urls.tsv", overlayUrls);

		if (MapLocator::IndexMatchesManifest(folder / "index.bin", plan.manifest)) return true;
		if (downloadPrebuiltIndex(folder, plan, progress)) return true;

		// no matching index published yet, get the images and build it here
		removeOldTiles(folder, plan);
		return downloadMissing(folder, plan.files, progress, cancel);
	}

	int makeIndex(const fs::path& folder) {
		std::cout << "Reading HoYoLAB map info..." << std::endl;
		Plan plan;
		if (!makePlan(plan)) {
			std::cerr << "Couldn't read the map API" << std::endl;
			return 1;
		}

		if (!downloadItems()) {
			std::cerr << "Downloading item data failed" << std::endl;
			return 1;
		}

		json published;
		if (fetchPublishedMeta(published) && published.value("manifest", "") == plan.manifest) {
			std::cout << "The published index is already current" << std::endl;
			return 3;
		}

		std::cout << plan.files.size() << " images" << std::endl;
		int lastShown = -10;
		bool downloaded = downloadMissing(folder, plan.files, [&](const char*, int percent) {
			if (percent >= lastShown + 10) {
				lastShown = percent;
				std::cout << "  " << percent << "%" << std::endl;
			}
			});
		if (!downloaded) {
			std::cerr << "Download failed" << std::endl;
			return 1;
		}
		removeOldTiles(folder, plan);
		fs::path manifestPath = folder / "manifest.tsv";
		if (!writeFile(manifestPath, plan.manifest)) return 1;

		std::cout << "Building the index..." << std::endl;
		MapLocator::Index index;
		if (!index.Load(manifestPath)) {
			std::cerr << "Building the index failed" << std::endl;
			return 1;
		}

		std::string raw = readText(folder / "index.bin");
		std::string packed;
		if (raw.empty() || !gzip(raw, packed)) return 1;

		char built[32];
		std::time_t now = std::time(nullptr);
		std::tm utc;
		gmtime_s(&utc, &now);
		std::strftime(built, sizeof(built), "%Y-%m-%dT%H:%M:%SZ", &utc);

		json meta = {
			{ "format", META_FORMAT },
			{ "built", built },
			{ "file", INDEX_FILE },
			{ "size", packed.size() },
			{ "rawSize", raw.size() },
			{ "sha256", sha256(raw) },
			{ "manifest", plan.manifest },
		};

		fs::path out = folder / "out";
		if (!writeFile(out / INDEX_FILE, packed) || !writeFile(out / META_FILE, meta.dump(1))) return 1;
		std::cout << "Wrote " << INDEX_FILE << " (" << packed.size() / 1000000.0 << " MB) and " << META_FILE << std::endl;
		return 0;
	}

	extern bool downloadItems() {
		for (int map_id : MAP_IDS) {
			if (!downloadItemIcons(map_id) || !downloadItemPositionData(map_id)) {
				std::cerr << "[icons] failed to download icons for map " << map_id << std::endl;
				return false;
			}
		}
		return true;
	}

	// Need to implement for other maps (Not just map 2)
	bool downloadItemIcons(int map_id) {
		json data;
		if (!getJson("/v2/map/label/tree?map_id=" + std::to_string(map_id) + "&app_sn=ys_obc&lang=en-us", data, true)
			|| !data.is_object() || !data.contains("tree") || !data["tree"].is_array()) {
			std::cerr << "[icons] label tree request failed or has an unexpected shape" << std::endl;
			return false;
		}

		struct Icon { std::string url, path; };
		std::map<int, Icon> icons;
		fs::path folder = fs::path("items") / "icons";

		std::vector<const json*> stack;
		for (const json& node : data["tree"]) stack.push_back(&node);
		while (!stack.empty()) {
			const json& node = *stack.back();
			stack.pop_back();
			if (!node.is_object()) continue;

			if (node.contains("id") && node["id"].is_number_integer()
				&& node.contains("icon") && node["icon"].is_string()) {
				int id = node["id"].get<int>();
				std::string url = node["icon"].get<std::string>();
				if (id > 0 && !url.empty()) {
					std::string ext = ".png";
					size_t dot = url.find_last_of('.'), slash = url.find_last_of('/');
					if (dot != std::string::npos && slash != std::string::npos && dot > slash && url.size() - dot <= 5) {
						ext = url.substr(dot);
					}
					icons.emplace(id, Icon{ url, (folder / (std::to_string(id) + ext)).string() });
				}
			}

			if (node.contains("children") && node["children"].is_array()) {
				for (const json& child : node["children"]) stack.push_back(&child);
			}
		}

		if (icons.empty()) {
			std::cerr << "[icons] the tree had no id/icon pairs" << std::endl;
			return false;
		}

		std::error_code ec;
		fs::create_directories(folder, ec);

		std::vector<const Icon*> todo;
		for (const auto& entry : icons) {
			if (!fs::exists(fs::u8path(entry.second.path))) todo.push_back(&entry.second);
		}

		std::atomic<size_t> next{ 0 }, saved{ 0 }, failed{ 0 };
		auto worker = [&]() {
			for (;;) {
				size_t i = next.fetch_add(1);
				if (i >= todo.size()) return;

				bool ok = false;
				for (int attempt = 0; attempt < 3 && !ok; attempt++) {
					Net::Response response;
					if (Net::get(todo[i]->url, response, true) && response.status == 200 && !response.body.empty()) {
						ok = writeFile(fs::u8path(todo[i]->path), response.body);
					}
				}
				if (ok) saved++;
				else {
					failed++;
					std::cerr << "[icons] failed: " << todo[i]->url << std::endl;
				}
			}
			};

		std::vector<std::thread> pool;
		for (int t = 0; t < DOWNLOAD_THREADS; t++) pool.emplace_back(worker);
		for (auto& thread : pool) thread.join();

		std::cout << "[icons] " << icons.size() << " found, " << saved << " saved, "
			<< (icons.size() - todo.size()) << " already there, " << failed << " failed" << std::endl;
		return true;
	}

	extern bool downloadItemPositionData(int map_id)
	{
		json data;

		std::string query =
			"app_sn=ys_obc&lang=en-us&map_id=" + std::to_string(map_id);

		if (!getJson("/v1/map/point/list?" + query, data))
			return false;

		if (!data.contains("point_list") || !data["point_list"].is_array())
			return false;

		fs::path folder = fs::path("items") / "positions";
		fs::create_directories(folder);

		std::unordered_map<int, json> positions;

		for (const auto& point : data["point_list"])
		{
			if (!point.is_object() ||
				!point.contains("label_id") ||
				!point["label_id"].is_number_integer())
			{
				continue;
			}

			int labelId = point["label_id"].get<int>();

			if (labelId <= 0)
				continue;

			// Cull the author_name and ctime, saves roughly 20% of the size of positions directory.
			json cleanedPoint = point;
			cleanedPoint.erase("author_name");
			cleanedPoint.erase("ctime");

			positions[labelId].push_back(std::move(cleanedPoint));
		}

		for (const auto& [labelId, points] : positions)
		{
			if (!writeFile(
				folder / (std::to_string(labelId) + ".json"),
				points.dump(2)))
			{
				return false;
			}
		}

		return true;
	}
}
