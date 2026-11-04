#include "map_data.hpp"
#include "files.hpp"
#include "net.hpp"
#include "locator/map_locator.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace MapData {
	static const std::string TILES = "https://act-webstatic.hoyoverse.com/map_manage/map/";
	static const std::string INDEX_FILE = "map-index.bin.gz";
	static const std::string META_FILE = "map-index.json";
	// the small whole maps: whole_<map>_<version>.png on disk, whole_<map>.png on the release
	static const std::string WHOLE = "whole_";
	static constexpr int META_FORMAT = 1;
	// index.bin is about 36 MB
	static constexpr size_t MAX_INDEX_SIZE = 512u * 1024 * 1024;

	// numbers go into the manifest exactly as HoYoLAB sends them
	static std::string number(const json& value) {
		return value.dump();
	}

	Box bounds(const Floor& floor) {
		return { (std::min)(floor.left, floor.right), (std::min)(floor.top, floor.bottom),
			(std::max)(floor.left, floor.right), (std::max)(floor.top, floor.bottom) };
	}

	std::vector<int> groupFloors(const std::vector<Floor>& floors, int mapId, int groupId) {
		std::vector<int> out;
		for (const Floor& floor : floors) {
			if (floor.mapId == mapId && floor.groupId == groupId) out.push_back(floor.floorId);
		}
		return out;
	}

	bool groupBox(const std::vector<Floor>& floors, int mapId, int groupId, Box& out) {
		bool any = false;
		for (const Floor& floor : floors) {
			if (floor.mapId != mapId || floor.groupId != groupId) continue;
			Box box = bounds(floor);
			if (any) {
				box = { (std::min)(out.left, box.left), (std::min)(out.top, box.top),
					(std::max)(out.right, box.right), (std::max)(out.bottom, box.bottom) };
			}
			out = box;
			any = true;
		}
		return any;
	}

	bool groupInside(const std::vector<Floor>& floors, int mapId, int inner, int outer) {
		Box in, out;
		if (!groupBox(floors, mapId, inner, in) || !groupBox(floors, mapId, outer, out)) return false;
		double x = (in.left + in.right) / 2, y = (in.top + in.bottom) / 2;
		return x >= out.left && x <= out.right && y >= out.top && y <= out.bottom;
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
		if (response.status == 404) return Files::writeFile(path, "");
		return response.status == 200 && Files::writeFile(path, response.body);
	}

	// HoYoLAB's address of a grid's tile
	static std::string tileUrl(int mapId, const Grid& grid, int x, int y) {
		// dir is tiles/<id>_<version>
		std::string dir = grid.dir.filename().string();
		return TILES + std::to_string(mapId) + "/" + dir.substr(dir.find('_') + 1) + "/" + std::to_string(x) + "_" + std::to_string(y) + "_P0.webp";
	}

	bool fetchTile(int mapId, const Grid& grid, int x, int y) {
		return fetch(tileUrl(mapId, grid, x, y), tilePath(grid, x, y));
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

		// not kept while it isn't on disk, it can be there at the next draw
		static const cv::Mat none;
		fs::path path = tilePath(grid, x, y);
		if (!fs::exists(path) && !(m_fetch && m_fetch(mapId, grid, x, y))) return none;
		cv::Mat image = readGray(path, shrunkBy);

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

	fs::path wholeMapPath(const fs::path& folder, const Grid& grid) {
		return folder / (WHOLE + grid.dir.filename().string() + ".png");
	}

	bool makeWholeMap(const fs::path& folder, int mapId, const Grid& grid, const std::atomic<bool>* cancel) {
		std::string dir = "tiles/" + grid.dir.filename().string() + "/";
		std::vector<File> files;
		for (int y = 0; y < grid.rows; ++y) {
			for (int x = 0; x < grid.cols; ++x) {
				files.push_back({ tileUrl(mapId, grid, x, y), dir + std::to_string(x) + "_" + std::to_string(y) + ".webp" });
			}
		}
		// with a tile missing it would have a black hole in it for good
		if (!downloadMissing(folder, files, nullptr, cancel)) return false;

		int size = grid.tileSize / SMALL_BY;
		cv::Mat whole(grid.rows * size, grid.cols * size, CV_8U, cv::Scalar(0));
		for (int y = 0; y < grid.rows; ++y) {
			for (int x = 0; x < grid.cols; ++x) {
				if (cancel && cancel->load()) return false;
				cv::Mat image = readGray(tilePath(grid, x, y));
				if (image.empty()) continue;
				cv::resize(image, whole(cv::Rect(x * size, y * size, size, size)), cv::Size(size, size), 0, 0, cv::INTER_AREA);
			}
		}
		std::vector<uchar> bytes;
		if (!cv::imencode(".png", whole, bytes)) return false;
		return Files::writeFile(wholeMapPath(folder, grid), std::string(bytes.begin(), bytes.end()));
	}

	bool makePlan(Plan& plan, Net::Stopper* stop) {
		std::vector<std::string> lines = { "# genshin-overlay map manifest v1" };
		plan.files.clear();
		plan.tileDirs.clear();

		try {
			for (int id : MAP_IDS) {
				std::string query = "map_id=" + std::to_string(id) + "&app_sn=ys_obc&lang=en-us";
				json info;
				if (!Files::getJson("/v1/map/info?" + query, info, false, stop)) return false;

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
				if (!Files::getJson("/v2/map/point_group?" + query, groups, false, stop)) return false;
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

		std::atomic<bool> failed{ false };
		auto stop = [&]() { return failed || (cancel && cancel->load()); };
		Files::inParallel(missing.size(), [&](size_t i) {
			const File& file = *missing[i];
			bool saved = false;
			for (int attempt = 0; attempt < 3 && !saved; attempt++) {
				saved = fetch(file.url, folder / fs::u8path(file.path));
			}
			if (!saved) failed = true;
		}, stop, [&](size_t done) {
			if (progress) progress("Downloading map images", (int)(done * 100 / missing.size()));
		});
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

	static bool fetchPublishedMeta(json& meta, Net::Stopper* stop = nullptr) {
		return Files::getMeta(META_FILE, META_FORMAT, meta, stop) == Files::Meta::Ok;
	}

	bool downloadPrebuiltIndex(const fs::path& folder, const Plan& plan, const Progress& progress, Net::Stopper* stop) {
		json meta;
		if (!fetchPublishedMeta(meta, stop) || Files::stringField(meta, "manifest", "") != plan.manifest) return false;

		if (progress) progress("Downloading map index", 0);
		std::string raw, problem;
		if (!Files::getChecked(Files::stringField(meta, "file", INDEX_FILE), meta, MAX_INDEX_SIZE, raw, problem, stop)) {
			std::cerr << "The downloaded map index is refused: " << problem << std::endl;
			return false;
		}

		fs::path index = folder / "index.bin";
		if (!Files::writeFile(index, raw)) return false;
		return MapLocator::IndexMatchesManifest(index, plan.manifest);
	}

	// The small whole maps that aren't on disk, from the release if it has
	// them for this map. What's still missing after, the app makes from the
	// tiles when it needs it, see makeWholeMap.
	static void downloadWholeMaps(const fs::path& folder, const Plan& plan, Net::Stopper* stop) {
		std::set<std::string> wanted;
		for (const std::string& dir : plan.tileDirs) wanted.insert(WHOLE + dir + ".png");
		// The ones of maps HoYoLAB has replaced. And they were called small_
		// before, made from whatever tiles happened to be on disk.
		std::error_code ec;
		std::vector<fs::path> old;
		for (const auto& entry : fs::directory_iterator(folder, ec)) {
			std::string name = entry.path().filename().string();
			bool whole = name.rfind(WHOLE, 0) == 0 || name.rfind("small_", 0) == 0;
			if (whole && entry.path().extension() == ".png" && wanted.count(name) == 0) old.push_back(entry.path());
		}
		for (const fs::path& path : old) fs::remove(path, ec);

		std::vector<std::string> missing;
		for (const std::string& dir : plan.tileDirs) {
			if (!fs::exists(folder / (WHOLE + dir + ".png"))) missing.push_back(dir);
		}
		if (missing.empty()) return;

		json meta;
		if (!fetchPublishedMeta(meta, stop) || Files::stringField(meta, "manifest", "") != plan.manifest) return;
		if (!meta.contains("whole") || !meta["whole"].is_object()) return;
		for (const std::string& dir : missing) {
			std::string id = dir.substr(0, dir.find('_'));
			json hash = meta["whole"].value(id, json());
			Net::Response response;
			if (!hash.is_string() || !Net::get(Files::RELEASE + WHOLE + id + ".png", response, false, nullptr, stop) || response.status != 200) continue;
			if (Files::sha256(response.body) != hash.get<std::string>()) continue;
			Files::writeFile(folder / (WHOLE + dir + ".png"), response.body);
		}
	}

	bool ensure(const fs::path& folder, const Progress& progress, Net::Stopper* stop) {
		fs::path manifestPath = folder / "manifest.tsv";

		if (progress) progress("Checking map data", 0);
		Plan plan;
		if (!makePlan(plan, stop)) {
			if (stop && stop->stopped()) return false;
			std::cerr << "Couldn't reach HoYoLAB, using the map data already on disk" << std::endl;
			return fs::exists(manifestPath);
		}

		if (!fs::exists(manifestPath) || Files::readText(manifestPath) != plan.manifest) {
			if (!Files::writeFile(manifestPath, plan.manifest)) return false;
		}

		// where each underground floor image comes from, for drawing them later
		std::string overlayUrls;
		for (const auto& file : plan.files) {
			if (file.path.rfind("overlays/", 0) == 0) {
				overlayUrls += file.path + "\t" + file.url + "\n";
			}
		}
		Files::writeFile(folder / "overlay_urls.tsv", overlayUrls);

		bool ready = MapLocator::IndexMatchesManifest(folder / "index.bin", plan.manifest) || downloadPrebuiltIndex(folder, plan, progress, stop);
		if (!ready) {
			// no matching index published yet, get the images and build it here
			removeOldTiles(folder, plan);
			ready = downloadMissing(folder, plan.files, progress, stop ? stop->flag() : nullptr);
		}
		if (ready) downloadWholeMaps(folder, plan, stop);
		return ready;
	}

	int makeIndex(const fs::path& folder) {
		std::cout << "Reading HoYoLAB map info..." << std::endl;
		Plan plan;
		if (!makePlan(plan)) {
			std::cerr << "Couldn't read the map API" << std::endl;
			return 1;
		}

		json published;
		// an index published before the small whole maps were gets made again with them
		if (fetchPublishedMeta(published) && Files::stringField(published, "manifest", "") == plan.manifest && published.contains("whole")) {
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
		if (!Files::writeFile(manifestPath, plan.manifest)) return 1;

		std::cout << "Building the index..." << std::endl;
		MapLocator::Index index;
		if (!index.Load(manifestPath)) {
			std::cerr << "Building the index failed" << std::endl;
			return 1;
		}

		std::string raw = Files::readText(folder / "index.bin");
		std::string packed;
		if (raw.empty() || !Files::gzip(raw, packed)) return 1;

		json meta = {
			{ "format", META_FORMAT },
			{ "built", Files::utcNow() },
			{ "file", INDEX_FILE },
			{ "size", packed.size() },
			{ "rawSize", raw.size() },
			{ "sha256", Files::sha256(raw) },
			{ "manifest", plan.manifest },
		};

		fs::path out = folder / "out";
		std::cout << "Making the small whole maps..." << std::endl;
		meta["whole"] = json::object();
		for (const auto& [id, grid] : readManifest(folder).grids) {
			if (!makeWholeMap(folder, id, grid)) return 1;
			std::string picture = Files::readText(wholeMapPath(folder, grid));
			if (picture.empty() || !Files::writeFile(out / (WHOLE + std::to_string(id) + ".png"), picture)) return 1;
			meta["whole"][std::to_string(id)] = Files::sha256(picture);
		}

		if (!Files::writeFile(out / INDEX_FILE, packed) || !Files::writeFile(out / META_FILE, meta.dump(1))) return 1;
		std::cout << "Wrote " << INDEX_FILE << " (" << packed.size() / 1000000.0 << " MB), " << META_FILE << " and the small whole maps" << std::endl;
		return 0;
	}
}
