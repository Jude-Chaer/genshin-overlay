#include "item_files.hpp"
#include "files.hpp"
#include "appsample.hpp"
#include "item_catalog.hpp"
#include "item_patch.hpp"
#include "map_data.hpp"
#include "net.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <string_view>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;
// keeps the keys in the order they're set, so the files read like HoYoLAB's
using ordered = nlohmann::ordered_json;

namespace ItemFiles {
	static const std::string APPSAMPLE = "https://game-data.lemonapi.com/gim/markers_all.v5.compressed.json";
	// a pack bigger than this unpacked isn't ours (today's is about 17 MB)
	static constexpr size_t MAX_RAW_SIZE = 128u * 1024 * 1024;
	// A week adds a few points and takes a few away. A map that loses more than
	// this of its points, types or pairs is more likely a change on HoYoLAB's or
	// appsample's side than a real one, so the builder stops and keeps last
	// week's (the smallest map has 48 types, so this is about 10 of them).
	static constexpr int MAX_DROP_PERCENT = 20;
	// how long a patch stays on the release, so an overlay off for longer takes the whole pack
	static constexpr int PATCH_DAYS = 7;

	static std::string dump(const ordered& data) {
		return data.dump(-1, ' ', false, ordered::error_handler_t::replace);
	}

	static ordered field(const json& from, const char* key) {
		return from.is_object() && from.contains(key) ? ordered(from[key]) : ordered();
	}

	static ordered trimPoint(const json& point, const std::map<int64_t, int64_t>& pairs) {
		ordered out;
		for (const char* key : { "id", "map_id", "x_pos", "y_pos", "area_id" }) out[key] = field(point, key);
		const json* group = point.contains("point_group") ? &point["point_group"] : nullptr;
		if (group && group->is_object() && !group->empty()) {
			out["point_group"] = { { "group_id", field(*group, "group_id") }, { "floor_id", field(*group, "floor_id") } };
		}
		else out["point_group"] = nullptr;
		auto pair = pairs.find(point["id"].get<int64_t>());
		if (pair != pairs.end()) out["appsample"] = pair->second;
		return out;
	}

	static bool hasId(const json& node) {
		return node.is_object() && node.contains("id") && node["id"].is_number_integer();
	}

	// Every map's tree and areas repeat the same groups and regions: they go in
	// once each here, and the overlay loads them as they are.
	bool write(const Source& source, const fs::path& folder) {
		ordered tree = ordered::array();
		std::map<int64_t, size_t> groupAt;
		std::map<int64_t, std::set<int64_t>> typesIn;  // group: its types so far
		for (const auto& [map, groups] : source.trees) {
			if (!groups.is_array()) continue;
			for (const json& group : groups) {
				if (!hasId(group)) continue;
				int64_t id = group["id"].get<int64_t>();
				if (!groupAt.count(id)) {
					groupAt[id] = tree.size();
					tree.push_back({ { "id", id }, { "name", field(group, "name") }, { "children", ordered::array() } });
				}
				if (!group.contains("children") || !group["children"].is_array()) continue;
				ordered& children = tree[groupAt[id]]["children"];
				for (const json& type : group["children"]) {
					if (!hasId(type) || !typesIn[id].insert(type["id"].get<int64_t>()).second) continue;
					children.push_back({ { "id", type["id"] }, { "name", field(type, "name") } });
				}
			}
		}

		ordered areas = ordered::array();
		std::set<int64_t> areasSeen;
		for (const auto& [map, list] : source.areas) {
			if (!list.is_array()) continue;
			for (const json& area : list) {
				if (!hasId(area) || !areasSeen.insert(area["id"].get<int64_t>()).second) continue;
				ordered out;
				for (const char* key : { "id", "name", "map_id", "l_x", "l_y", "r_x", "r_y" }) out[key] = field(area, key);
				areas.push_back(std::move(out));
			}
		}

		if (!Files::writeFile(folder / "tree.json", dump(tree)) || !Files::writeFile(folder / "areas.json", dump(areas))) return false;
		// a point id once, under the first type that has it (never seen, HoYoLAB's ids are unique)
		std::set<int64_t> pointsSeen;
		for (const auto& [type, points] : source.points) {
			// one point per line, so a patch carries only the points that changed
			std::string list;
			for (const json& point : points) {
				if (!hasId(point) || !pointsSeen.insert(point["id"].get<int64_t>()).second) continue;
				list += (list.empty() ? "[\n" : ",\n") + dump(trimPoint(point, source.pairs));
			}
			list = list.empty() ? "[]" : list + "\n]";
			if (!Files::writeFile(folder / "positions" / (std::to_string(type) + ".json"), list)) return false;
		}
		return true;
	}

	std::string packName(const std::string& sha256) {
		return "items-" + sha256.substr(0, 12) + ".pack.gz";
	}

	std::string patchName(const std::string& from, const std::string& to) {
		return "items-" + from.substr(0, 12) + "-" + to.substr(0, 12) + ".patch.gz";
	}

	// a release file named in items.json: one the builder makes, nothing else
	static bool releaseName(const std::string& name) {
		if (name == PACK_FILE) return true;
		auto hex = [&](size_t at) {
			for (size_t i = at; i < at + 12; i++) {
				if (!std::isxdigit((unsigned char)name[i]) || std::isupper((unsigned char)name[i])) return false;
			}
			return true;
		};
		if (name.size() == 26 && name.compare(0, 6, "items-") == 0 && name.compare(18, 8, ".pack.gz") == 0) return hex(6);
		return name.size() == 40 && name.compare(0, 6, "items-") == 0 && name[18] == '-' && name.compare(31, 9, ".patch.gz") == 0 && hex(6) && hex(19);
	}

	bool pack(const fs::path& folder, std::string& raw) {
		std::error_code ec;
		// by the path as it's written in the pack, so the order doesn't depend on how paths compare here
		std::vector<std::pair<std::string, fs::path>> files;
		for (const auto& entry : fs::recursive_directory_iterator(folder, ec)) {
			if (entry.is_regular_file()) files.emplace_back(fs::relative(entry.path(), folder).generic_u8string(), entry.path());
		}
		if (ec || files.empty()) return false;
		std::sort(files.begin(), files.end());
		raw.clear();
		for (const auto& [name, path] : files) {
			std::string data = Files::readText(path);
			raw += name + "\t" + std::to_string(data.size()) + "\n";
			raw += data;
		}
		return true;
	}

	// names Windows keeps for devices, whatever comes after the dot
	static bool deviceName(std::string part) {
		part = part.substr(0, part.find('.'));
		for (char& c : part) c = (char)std::tolower((unsigned char)c);
		if (part == "con" || part == "prn" || part == "aux" || part == "nul") return true;
		return part.size() == 4 && (part.rfind("com", 0) == 0 || part.rfind("lpt", 0) == 0) && part[3] >= '0' && part[3] <= '9';
	}

	// Only plain names, each part a real name, so nothing can land outside the
	// folder (no .., no drive, no leading slash) or somewhere Windows would put
	// it instead (a device, or a name ending in a dot that Windows drops).
	static bool safeName(const std::string& name) {
		if (name.empty() || name.size() > 260) return false;
		for (char c : name) {
			if (!std::isalnum((unsigned char)c) && c != '/' && c != '.' && c != '_' && c != '-') return false;
		}
		size_t at = 0;
		for (;;) {
			size_t slash = name.find('/', at);
			std::string part = name.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
			if (part.empty() || part.back() == '.' || deviceName(part)) return false;
			if (slash == std::string::npos) return true;
			at = slash + 1;
		}
	}

	bool readPack(const std::string& raw, const std::function<bool(const std::string& name, std::string_view data)>& each) {
		std::string_view all(raw);
		size_t at = 0;
		int files = 0;
		while (at < all.size()) {
			size_t tab = all.find('\t', at), end = all.find('\n', at);
			if (tab == std::string::npos || end == std::string::npos || tab > end) return false;
			std::string name(all.substr(at, tab - at));
			std::string_view sizeText = all.substr(tab + 1, end - tab - 1);
			if (sizeText.empty() || sizeText.size() > 12 || sizeText.find_first_not_of("0123456789") != std::string::npos) return false;
			size_t size = std::stoull(std::string(sizeText));
			at = end + 1;
			if (size > all.size() - at || !safeName(name)) return false;
			if (!each(name, all.substr(at, size))) return false;
			at += size;
			files++;
		}
		return files > 0;
	}

	int keepIcons(const std::string& raw, const std::vector<int>& types, const fs::path& folder) {
		std::set<std::string> wanted;
		for (int type : types) wanted.insert("icons/" + std::to_string(type) + ".png");
		int kept = 0;
		readPack(raw, [&](const std::string& name, std::string_view data) {
			if (wanted.count(name) && Files::writeFile(folder / fs::u8path(name), std::string(data))) kept++;
			return true;
			});
		return kept;
	}

	// a release's items.json into meta, and why not when this overlay can't use it
	static Reason readMeta(const std::string& body, json& meta) {
		switch (Files::readMeta(body, FORMAT, meta)) {
		case Files::Meta::Ok: return Reason::None;
		case Files::Meta::Newer: return Reason::NewerFormat;
		default: return Reason::BrokenPack;
		}
	}

	// the pack unzipped and checked against its items.json, false with the problem if it isn't right
	static bool unzip(const std::string& packed, const json& meta, std::string& raw, std::string& problem) {
		if (!meta.is_object() || !meta.contains("format") || meta["format"] != FORMAT) {
			problem = "it isn't format " + std::to_string(FORMAT);
			return false;
		}
		return Files::unpack(packed, meta, MAX_RAW_SIZE, raw, problem);
	}

	// the catalog from an unzipped pack, only when every point and file of it loaded
	static std::shared_ptr<ItemCatalog::Catalog> loadAll(const std::string& raw, const Net::Stopper* stop, std::string& problem) {
		std::shared_ptr<ItemCatalog::Catalog> catalog = ItemCatalog::load(raw, stop);
		if (!catalog) problem = stop && stop->stopped() ? "stopped" : "tree.json, areas.json or the positions are missing or broken";
		else if (catalog->pointCount() == 0) problem = "it has no points";
		else if (catalog->skippedPoints() > 0) {
			problem = std::to_string(catalog->skippedPoints()) + " points didn't load (";
			const char* comma = "";
			for (const auto& [field, count] : catalog->skippedFields()) {
				problem += comma + std::to_string(count) + " without a good " + field;
				comma = ", ";
			}
			problem += ")";
		}
		else if (catalog->skippedFiles() > 0) problem = std::to_string(catalog->skippedFiles()) + " positions files aren't JSON";
		else return catalog;
		return nullptr;
	}

	std::map<int, MapCount> countMaps(const ItemCatalog::Catalog& catalog) {
		std::map<int, MapCount> counts;
		std::map<int, std::set<int>> types;
		for (const ItemCatalog::Point& point : catalog.allPoints()) {
			MapCount& count = counts[point.map];
			count.points++;
			if (point.appsample) count.pairs++;
			types[point.map].insert(point.type);
		}
		for (auto& [map, count] : counts) count.types = (int)types[map].size();
		return counts;
	}

	// a count from a published items.json, -1 when it has none
	static int publishedCount(const json& published, int map, const char* key) {
		if (!published.is_object() || !published.contains("maps") || !published["maps"].is_object()) return -1;
		const json& maps = published["maps"];
		std::string id = std::to_string(map);
		if (!maps.contains(id) || !maps[id].is_object() || !maps[id].contains(key) || !maps[id][key].is_number_integer()) return -1;
		int64_t count = maps[id][key].get<int64_t>();
		return count >= 0 && count <= std::numeric_limits<int>::max() ? (int)count : -1;
	}

	std::vector<std::string> checkBuilt(const std::string& raw, const std::vector<int>& maps, const json& published, bool allowDrop,
		std::map<int, MapCount>& counts) {
		std::vector<std::string> problems;
		std::string problem;
		std::shared_ptr<ItemCatalog::Catalog> catalog = loadAll(raw, nullptr, problem);
		if (!catalog) return { "the overlay would refuse it: " + problem };
		counts = countMaps(*catalog);
		for (int map : maps) {
			if (!counts.count(map)) problems.push_back("map " + std::to_string(map) + " has no points");
		}
		if (allowDrop || !published.is_object() || !published.contains("maps") || !published["maps"].is_object()) return problems;
		for (const auto& [id, was] : published["maps"].items()) {
			int map = std::atoi(id.c_str());
			MapCount now = counts.count(map) ? counts[map] : MapCount();
			std::pair<const char*, int> kinds[] = { { "points", now.points }, { "types", now.types }, { "pairs", now.pairs } };
			for (const auto& [key, count] : kinds) {
				int before = publishedCount(published, map, key);
				if (before > 0 && (int64_t)count * 100 < (int64_t)before * (100 - MAX_DROP_PERCENT)) {
					problems.push_back("map " + id + " has " + std::to_string(count) + " " + key + ", the published pack " + std::to_string(before));
				}
			}
		}
		return problems;
	}

	// which pack a catalog came from, so a later check can tell it's the same one
	static void markSource(ItemCatalog::Catalog& catalog, const json& meta) {
		catalog.sha256 = Files::stringField(meta, "sha256");
		catalog.built = Files::stringField(meta, "built");
	}

	std::shared_ptr<ItemCatalog::Catalog> open(const std::string& packed, const json& meta, const Net::Stopper* stop, std::string& problem) {
		std::string raw;
		if (!unzip(packed, meta, raw, problem)) return nullptr;
		std::shared_ptr<ItemCatalog::Catalog> catalog = loadAll(raw, stop, problem);
		if (catalog) markSource(*catalog, meta);
		return catalog;
	}

	bool install(const std::string& packed, const json& meta, const fs::path& folder) {
		// one file, written to a temp name and renamed over the old one, so it's whole either way
		return Files::writeFile(folder / INSTALLED_FILE, meta.dump() + "\n" + packed);
	}

	// items.pack's first line, false when there's none
	static bool readInstalledMeta(const fs::path& folder, json& meta) {
		std::ifstream file(folder / INSTALLED_FILE, std::ios::binary);
		std::string line;
		if (!std::getline(file, line)) return false;
		meta = json::parse(line, nullptr, false);
		return meta.is_object();
	}

	// the gzipped pack after it, empty when it's cut short
	static std::string readInstalledPack(const fs::path& folder) {
		std::string all = Files::readText(folder / INSTALLED_FILE);
		size_t end = all.find('\n');
		if (end == std::string::npos) return std::string();
		all.erase(0, end + 1);
		return all;
	}

	// Before items.pack, dev builds unpacked the pack into the folder and swapped
	// whole folders (items.new, items.old). None of that is read any more.
	static void forgetUnpacked(const fs::path& folder) {
		std::error_code ec;
		fs::path fresh = folder, old = folder;
		fresh += ".new";
		old += ".old";
		if (!fs::exists(folder / "version.txt", ec) && !fs::exists(fresh, ec) && !fs::exists(old, ec)) return;
		for (const char* name : { "tree.json", "areas.json", "positions", "icons", "built.txt", "version.txt" }) fs::remove_all(folder / name, ec);
		fs::remove_all(fresh, ec);
		fs::remove_all(old, ec);
		std::cout << "[items] removed the unpacked item files of an older build" << std::endl;
	}

	Fetch fromRelease(Net::Stopper* stopper) {
		return [stopper](const std::string& name, std::string& body, const Net::Progress& progress) {
			Net::Response response;
			if (!Net::get(Files::RELEASE + name, response, false, progress, stopper) || response.status != 200) return false;
			body = std::move(response.body);
			return true;
		};
	}

	static const char* why(Reason reason) {
		switch (reason) {
		case Reason::NoNetwork: return "the release couldn't be reached";
		case Reason::NewerFormat: return "the release's pack needs a newer overlay";
		case Reason::BrokenPack: return "the release's pack is broken";
		case Reason::CantSave: return "items.pack couldn't be written";
		case Reason::DownloadOff: return "the download is off";
		default: return "";
		}
	}

	Status update(const fs::path& folder, const Fetch& fetch, const std::atomic<bool>& download, const Net::Stopper& stop, const std::function<void(const Status&)>& onChange,
		std::shared_ptr<const ItemCatalog::Catalog> inUse) {
		forgetUnpacked(folder);
		json installedMeta;
		bool haveInstalled = readInstalledMeta(folder, installedMeta);
		std::string installedHash = haveInstalled ? Files::stringField(installedMeta, "sha256") : "";
		std::string inUseHash = inUse ? inUse->sha256 : "";

		// the pack that's there, read and loaded once at most, only when it's needed
		std::shared_ptr<ItemCatalog::Catalog> installed;
		bool installedTried = false;
		auto loadInstalled = [&]() {
			if (!installedTried && haveInstalled) {
				installedTried = true;
				std::string problem, raw;
				bool unzipped = unzip(readInstalledPack(folder), installedMeta, raw, problem);
				if (unzipped) installed = loadAll(raw, &stop, problem);
				if (installed) markSource(*installed, installedMeta);
				else if (!stop.stopped()) std::cerr << "[items] items.pack doesn't load: " << problem << std::endl;
			}
			return installed != nullptr;
		};

		auto finish = [&](State state, Reason reason, std::shared_ptr<ItemCatalog::Catalog> catalog, const json& meta, std::chrono::steady_clock::time_point start) {
			Status status;
			status.state = state;
			status.reason = reason;
			status.built = Files::stringField(meta, "built").substr(0, 10);
			std::cout << "[items] " << (state == State::Ready ? "up to date" : state == State::Old ? "using the pack from " + status.built : "none");
			if (reason != Reason::None) std::cout << ", " << why(reason);
			if (catalog) {
				std::cout << ", " << catalog->pointCount() << " points loaded in "
					<< std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << " ms";
				ItemCatalog::publish(std::move(catalog));
			}
			else if (inUse && state != State::None) std::cout << ", keeping the one in use";
			std::cout << std::endl;
			onChange(status);
			return status;
		};

		Reason reason = Reason::DownloadOff;
		json published;
		if (download) {
			std::string body;
			if (!fetch(META_FILE, body, nullptr)) reason = download ? Reason::NoNetwork : Reason::DownloadOff;
			else {
				reason = readMeta(body, published);
				if (reason != Reason::None) published = nullptr;
			}
		}
		// closing: nothing more to say, the overlay is going
		if (stop.stopped()) return Status();

		auto start = std::chrono::steady_clock::now();
		std::string releasedHash = Files::stringField(published, "sha256");
		if (!published.is_null() && !installedHash.empty() && releasedHash == installedHash) {
			if (inUseHash == installedHash) return finish(State::Ready, Reason::None, nullptr, installedMeta, start);
			if (loadInstalled()) return finish(State::Ready, Reason::None, installed, installedMeta, start);
		}

		if (!published.is_null()) {
			size_t size = Files::sizeField(published, "size");
			Status progress;
			progress.state = State::Downloading;
			onChange(progress);
			std::string packed;
			bool tooBig = false;
			// a pack with no size, or more than items.json says, isn't ours
			bool got = size > 0 && size <= MAX_RAW_SIZE && fetch(PACK_FILE, packed, [&](size_t bytes) {
				int percent = (int)std::min<size_t>(100, bytes * 100 / size);
				if (percent != progress.percent) {
					progress.percent = percent;
					onChange(progress);
				}
				tooBig = tooBig || bytes > size;
				return !stop.stopped() && !tooBig && download;
			});
			if (stop.stopped()) return Status();
			start = std::chrono::steady_clock::now();
			if (!got) {
				// unticked partway: the player cut it, the network didn't
				reason = size == 0 || size > MAX_RAW_SIZE || tooBig ? Reason::BrokenPack : download ? Reason::NoNetwork : Reason::DownloadOff;
				if (reason == Reason::BrokenPack) std::cerr << "[items] the release's pack isn't the size its items.json says" << std::endl;
			}
			else if (!inUseHash.empty() && releasedHash == inUseHash) {
				// in use but not saved (it couldn't write before): checked, then only written
				std::string problem, raw;
				if (unzip(packed, published, raw, problem)) {
					bool saved = install(packed, published, folder);
					if (!saved) std::cerr << "[items] couldn't write " << INSTALLED_FILE << " again" << std::endl;
					return finish(State::Ready, saved ? Reason::None : Reason::CantSave, nullptr, published, start);
				}
				std::cerr << "[items] the downloaded pack is refused: " << problem << std::endl;
				reason = Reason::BrokenPack;
			}
			else {
				std::string problem;
				std::shared_ptr<ItemCatalog::Catalog> fresh = open(packed, published, &stop, problem);
				if (stop.stopped()) return Status();
				if (fresh) {
					bool saved = install(packed, published, folder);
					if (!saved) std::cerr << "[items] couldn't write " << INSTALLED_FILE << ", the pack is used until the overlay closes" << std::endl;
					return finish(State::Ready, saved ? Reason::None : Reason::CantSave, fresh, published, start);
				}
				std::cerr << "[items] the downloaded pack is refused: " << problem << std::endl;
				reason = Reason::BrokenPack;
			}
		}

		// what's in use stays; the one on disk is only loaded when nothing is
		if (inUse) return finish(State::Old, reason, nullptr, json{ { "built", inUse->built } }, start);
		if (loadInstalled()) return finish(State::Old, reason, installed, installedMeta, start);
		if (stop.stopped()) return Status();
		return finish(State::None, reason, nullptr, json(), start);
	}

	bool waitsForNetwork(const Status& status) {
		return status.reason == Reason::NoNetwork;
	}

	static std::thread worker;
	static std::mutex mutex;
	static std::condition_variable wake;
	static bool checkAgain = false;  // under mutex
	static std::atomic<bool> downloadOn{ true };
	static std::unique_ptr<Net::Stopper> stopper;  // stopped under mutex, so the worker's wait sees it
	static Status status;

	// update, without an exception taking the overlay down with it
	static Status updateCaught(const fs::path& folder, const Fetch& fetch, const std::function<void(const Status&)>& onChange) {
		try {
			return update(folder, fetch, downloadOn, *stopper, onChange, ItemCatalog::current());
		}
		catch (const std::exception& e) {
			std::cerr << "[items] the check failed: " << e.what() << std::endl;
		}
		catch (...) {
			std::cerr << "[items] the check failed" << std::endl;
		}
		Status failed;
		failed.reason = Reason::BrokenPack;
		onChange(failed);
		return failed;
	}

	void Start(const fs::path& folder, bool download) {
		if (worker.joinable()) return;
		checkAgain = false;
		downloadOn = download;
		stopper = std::make_unique<Net::Stopper>();
		status = Status();
		status.checking = true;
		worker = std::thread([folder]() {
			// unticking the download cuts one that's running
			Fetch fetch = [release = fromRelease(stopper.get())](const std::string& name, std::string& body, const Net::Progress& progress) {
				return release(name, body, [&progress](size_t got) { return downloadOn && (!progress || progress(got)); });
			};
			auto onChange = [](const Status& now) {
				std::lock_guard<std::mutex> lock(mutex);
				bool checking = status.checking;
				status = now;
				status.started = true;
				status.checking = checking;
			};
			for (;;) {
				Status result = updateCaught(folder, fetch, onChange);
				std::unique_lock<std::mutex> lock(mutex);
				status.checking = false;
				auto asked = []() { return stopper->stopped() || checkAgain; };
				if (waitsForNetwork(result)) wake.wait_for(lock, RETRY, asked);
				else wake.wait(lock, asked);
				if (stopper->stopped()) return;
				checkAgain = false;
				status.checking = true;
			}
		});
	}

	void AskToStop() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			// a download waiting on the network, or a load, gives up now
			if (stopper) stopper->stop();
		}
		wake.notify_all();
	}

	void Stop() {
		AskToStop();
		if (worker.joinable()) worker.join();
	}

	Status GetStatus() {
		std::lock_guard<std::mutex> lock(mutex);
		return status;
	}

	void CheckNow() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!worker.joinable() || stopper->stopped()) return;
			checkAgain = true;
			status.checking = true;
		}
		wake.notify_all();
	}

	void SetDownload(bool on) {
		downloadOn = on;
		CheckNow();
	}

	// Some of HoYoLAB's icons are over 1000 px and 3 MB, to be drawn at about 30 px.
	// Shrunk to fit ICON_SIZE and saved as png, so a few hundred loaded stay small.
	static constexpr int ICON_SIZE = 64;
	static bool shrinkIcon(const std::string& body, std::string& png) {
		std::vector<uchar> bytes(body.begin(), body.end());
		cv::Mat image = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
		if (image.empty()) return false;
		int longest = std::max(image.cols, image.rows);
		if (longest > ICON_SIZE) {
			double scale = (double)ICON_SIZE / longest;
			cv::resize(image, image, cv::Size(std::max(1, (int)std::lround(image.cols * scale)), std::max(1, (int)std::lround(image.rows * scale))), 0, 0, cv::INTER_AREA);
		}
		std::vector<uchar> out;
		if (!cv::imencode(".png", image, out)) return false;
		png.assign(out.begin(), out.end());
		return true;
	}

	// every type's icon in the trees, the first url found for each
	static void findIcons(const json& tree, std::map<int, std::string>& icons) {
		std::vector<const json*> stack;
		for (const json& node : tree) stack.push_back(&node);
		while (!stack.empty()) {
			const json& node = *stack.back();
			stack.pop_back();
			if (!node.is_object()) continue;
			if (hasId(node) && node.contains("icon") && node["icon"].is_string()) {
				int id = node["id"].get<int>();
				std::string url = node["icon"].get<std::string>();
				if (id > 0 && !url.empty()) icons.emplace(id, url);
			}
			if (node.contains("children") && node["children"].is_array()) {
				for (const json& child : node["children"]) stack.push_back(&child);
			}
		}
	}

	// the types whose icon didn't come
	static std::vector<int> downloadIcons(const std::map<int, std::string>& icons, const fs::path& folder) {
		std::vector<std::pair<int, std::string>> todo(icons.begin(), icons.end());
		std::atomic<size_t> saved{ 0 };
		std::mutex lock;
		std::vector<int> failed;
		Files::inParallel(todo.size(), [&](size_t i) {
			bool ok = false;
			for (int attempt = 0; attempt < 3 && !ok; attempt++) {
				Net::Response response;
				std::string icon;
				if (Net::get(todo[i].second, response, true) && response.status == 200 && shrinkIcon(response.body, icon)) {
					ok = Files::writeFile(folder / (std::to_string(todo[i].first) + ".png"), icon);
				}
			}
			if (ok) saved++;
			else {
				std::lock_guard<std::mutex> hold(lock);
				failed.push_back(todo[i].first);
				std::cerr << "[items] icon failed: " << todo[i].second << std::endl;
			}
		});
		std::cout << "[items] " << icons.size() << " icons, " << saved << " saved, " << failed.size() << " failed" << std::endl;
		return failed;
	}

	// What's on the release now, null when there's none yet. False when it
	// can't be read, as the counts to compare with would be missing. A broken
	// one is replaced with --allow-drop.
	static bool fetchPublished(json& published, bool allowDrop) {
		switch (Files::getMeta(META_FILE, FORMAT, published)) {
		case Files::Meta::Ok: return true;
		case Files::Meta::Missing:
			published = nullptr;
			return true;
		case Files::Meta::Broken:
			std::cerr << "[items] the published " << META_FILE << " doesn't read" << std::endl;
			published = nullptr;
			return allowDrop;
		case Files::Meta::Newer:
			std::cerr << "[items] the published " << META_FILE << " is a newer format than this build makes" << std::endl;
			return false;
		default:
			std::cerr << "[items] the published " << META_FILE << " didn't come" << std::endl;
			return false;
		}
	}

	// the published raw pack, empty when there's none or it can't be read
	static std::string fetchPublishedPack(const json& published) {
		if (published.is_null()) return std::string();
		std::string file = Files::stringField(published, "file", PACK_FILE), raw, problem;
		if (!releaseName(file)) file = PACK_FILE;
		if (!Files::getChecked(file, published, MAX_RAW_SIZE, raw, problem)) {
			std::cerr << "[items] couldn't read the published pack: " << problem << std::endl;
			return std::string();
		}
		return raw;
	}

	// An icon that didn't come is taken from the published pack, so a bad
	// week neither loses icons nor makes everyone download a new pack.
	static void keepPublishedIcons(const std::string& publishedRaw, const std::vector<int>& failed, const fs::path& folder) {
		if (failed.empty() || publishedRaw.empty()) return;
		std::cout << "[items] " << keepIcons(publishedRaw, failed, folder) << " of " << failed.size() << " missing icons kept from the published pack" << std::endl;
	}

	// the release files an items.json points to
	static std::set<std::string> filesOf(const json& meta) {
		std::set<std::string> names;
		std::string file = Files::stringField(meta, "file");
		if (releaseName(file)) names.insert(file);
		if (meta.is_object() && meta.contains("patches") && meta["patches"].is_array()) {
			for (const json& step : meta["patches"]) {
				std::string name = Files::stringField(step, "file");
				if (releaseName(name)) names.insert(name);
			}
		}
		return names;
	}

	static bool downloadMap(int map, Source& source) {
		std::string query = "map_id=" + std::to_string(map) + "&app_sn=ys_obc&lang=en-us";
		json tree, areas, points;
		if (!Files::getJson("/v2/map/label/tree?" + query, tree, true) || !tree.is_object() || !tree.contains("tree") || !tree["tree"].is_array()) {
			std::cerr << "[items] the type tree of map " << map << " didn't come" << std::endl;
			return false;
		}
		if (!Files::getJson("/v1/map/get_area_pageLabel?" + query, areas, true) || !areas.is_object() || !areas.contains("list") || !areas["list"].is_array()) {
			std::cerr << "[items] the regions of map " << map << " didn't come" << std::endl;
			return false;
		}
		if (!Files::getJson("/v1/map/point/list?" + query, points) || !points.is_object() || !points.contains("point_list") || !points["point_list"].is_array()) {
			std::cerr << "[items] the points of map " << map << " didn't come" << std::endl;
			return false;
		}
		source.trees[map] = std::move(tree["tree"]);
		source.areas[map] = std::move(areas["list"]);

		// Types are shared between maps (a common chest is 17 on every map), so
		// every map's points go into the one list per type, each with its map_id.
		for (json& point : points["point_list"]) {
			if (!hasId(point)) {
				source.dropped["id"]++;
				continue;
			}
			if (!point.contains("label_id") || !point["label_id"].is_number_integer() || point["label_id"].get<int64_t>() <= 0
				|| point["label_id"].get<int64_t>() > std::numeric_limits<uint16_t>::max()) {
				source.dropped["label_id"]++;
				continue;
			}
			int type = point["label_id"].get<int>();
			point["map_id"] = map;
			source.points[type].push_back(std::move(point));
		}
		return true;
	}

	// Which appsample point each HoYoLAB point is, for showing the comments people left there.
	static bool pairWithAppsample(Source& source) {
		Net::Response response;
		std::vector<Appsample::Point> theirs;
		if (!Net::get(APPSAMPLE, response) || response.status != 200 || !Appsample::decode(response.body, theirs)) {
			std::cerr << "[items] appsample's points didn't come" << std::endl;
			return false;
		}

		std::vector<Appsample::Ours> ours;
		auto integer = [](const json& from, const char* field) {
			return from.is_object() && from.contains(field) && from[field].is_number_integer() ? from[field].get<int>() : 0;
		};
		for (const auto& [type, points] : source.points) {
			for (const json& point : points) {
				if (!point.contains("x_pos") || !point["x_pos"].is_number() || !point.contains("y_pos") || !point["y_pos"].is_number()) continue;
				Appsample::Ours our;
				our.id = point["id"].get<int64_t>();
				our.map = point["map_id"].get<int>();
				our.type = type;
				our.area = integer(point, "area_id");
				if (point.contains("point_group")) {
					our.group = integer(point["point_group"], "group_id");
					our.floor = integer(point["point_group"], "floor_id");
				}
				our.x = point["x_pos"].get<double>();
				our.y = point["y_pos"].get<double>();
				ours.push_back(our);
			}
		}
		Appsample::Result result = Appsample::pair(ours, theirs);
		source.pairs = std::move(result.pairs);
		for (const auto& [map, stats] : result.maps) {
			std::cout << "[items] map " << map << ": ";
			if (!stats.skipped.empty()) {
				std::cout << "not paired, " << stats.skipped << std::endl;
				continue;
			}
			std::cout << stats.paired << " of " << stats.ours << " paired (appsample has " << stats.theirs << "), half within "
				<< stats.residual << " px";
			if (stats.unplaced > 0) std::cout << ", " << stats.unplaced << " in regions that didn't line up";
			std::cout << std::endl;
		}
		std::cout << "[items] " << source.pairs.size() << " of " << ours.size() << " points paired with appsample" << std::endl;
		return true;
	}

	int make(const fs::path& folder, bool allowDrop) {
		// from scratch, so what HoYoLAB removed goes
		fs::path items = folder / "items";
		std::error_code ec;
		fs::remove_all(items, ec);

		std::cout << "Reading HoYoLAB's map items..." << std::endl;
		Source source;
		std::map<int, std::string> icons;
		std::vector<int> maps;
		if (!MapData::mapIds(maps)) {
			std::cerr << "HoYoLAB's map list didn't answer" << std::endl;
			return 1;
		}
		for (int map : maps) {
			if (!downloadMap(map, source)) return 1;
			findIcons(source.trees[map], icons);
		}
		// a point HoYoLAB sends that can't be filed under a type is most likely a renamed key
		for (const auto& [field, count] : source.dropped) std::cerr << "[items] " << count << " points without a good " << field << std::endl;
		if (!source.dropped.empty() && !allowDrop) {
			std::cerr << "The item files aren't made: HoYoLAB's points don't read as before (--allow-drop leaves those points out)" << std::endl;
			return 1;
		}
		json published;
		if (!fetchPublished(published, allowDrop)) {
			std::cerr << "The item files aren't made: the published ones can't be compared with" << std::endl;
			return 1;
		}
		std::string publishedRaw = fetchPublishedPack(published);
		keepPublishedIcons(publishedRaw, downloadIcons(icons, items / "icons"), items);
		// without appsample the pack would lose every pair for a week, so the published one stays
		if (!pairWithAppsample(source) || !write(source, items)) return 1;

		std::string raw;
		if (!pack(items, raw)) return 1;
		std::string hash = Files::sha256(raw);

		// what's published is what the overlay will load, so it's loaded here first the same way
		std::map<int, MapCount> counts;
		std::vector<std::string> problems = checkBuilt(raw, maps, published, allowDrop, counts);
		for (const auto& [map, count] : counts) {
			std::cout << "[items] map " << map << ": " << count.points << " points, " << count.types << " types, " << count.pairs << " pairs" << std::endl;
		}
		if (!problems.empty()) {
			for (const std::string& problem : problems) std::cerr << "[items] " << problem << std::endl;
			// the hint only when dropping is all that's wrong
			std::map<int, MapCount> ignored;
			bool onlyDrops = !allowDrop && checkBuilt(raw, maps, published, true, ignored).empty();
			std::cerr << "The item files aren't published" << (onlyDrops ? " (--allow-drop publishes a drop that's meant)" : "") << std::endl;
			return 1;
		}

		// one published before the counts were gets them now
		bool hasCounts = published.is_object() && published.contains("maps");
		if (!published.is_null() && Files::stringField(published, "sha256") == hash && hasCounts) {
			std::cout << "The published item files are already current" << std::endl;
			return 3;
		}

		std::string packed;
		if (!Files::gzip(raw, packed)) return 1;
		std::string built = Files::utcNow();
		ordered meta;
		meta["format"] = FORMAT;
		meta["built"] = built;
		meta["file"] = packName(hash);
		meta["size"] = packed.size();
		meta["rawSize"] = raw.size();
		meta["sha256"] = hash;
		meta["maps"] = ordered::object();
		for (const auto& [map, count] : counts) {
			meta["maps"][std::to_string(map)] = { { "points", count.points }, { "types", count.types }, { "pairs", count.pairs } };
		}

		// the published patches of the last PATCH_DAYS, and one from the published pack to this one
		fs::path out = folder / "out";
		meta["patches"] = ordered::array();
		std::string from = Files::stringField(published, "sha256");
		if (!publishedRaw.empty() && !from.empty()) {
			std::string cutoff = Files::utcNow(PATCH_DAYS * 24 * 3600);
			if (published.contains("patches") && published["patches"].is_array()) {
				for (const json& step : published["patches"]) {
					if (step.is_object() && Files::stringField(step, "built") >= cutoff) meta["patches"].push_back(ordered(step));
				}
			}
			std::string steps = ItemPatch::make(publishedRaw, raw), patch;
			if (!Files::gzip(steps, patch) || !Files::writeFile(out / patchName(from, hash), patch)) return 1;
			meta["patches"].push_back({ { "from", from }, { "to", hash }, { "file", patchName(from, hash) }, { "size", patch.size() },
				{ "rawSize", steps.size() }, { "built", built } });
			std::cout << "Wrote " << patchName(from, hash) << " (" << patch.size() / 1000.0 << " KB)" << std::endl;
		}

		// what the workflow keeps on the release: this items.json's files and the published one's,
		// so a download that started before this goes up still finishes
		std::set<std::string> keep = filesOf(json::parse(meta.dump())), before = filesOf(published);
		keep.insert(before.begin(), before.end());
		std::string keepList;
		for (const std::string& name : keep) keepList += name + "\n";

		if (!Files::writeFile(out / PACK_FILE, packed) || !Files::writeFile(out / packName(hash), packed)
			|| !Files::writeFile(out / "keep.txt", keepList) || !Files::writeFile(out / META_FILE, meta.dump(1))) return 1;
		std::cout << "Wrote " << packName(hash) << " (" << packed.size() / 1000000.0 << " MB) and " << META_FILE << std::endl;
		return 0;
	}
}
