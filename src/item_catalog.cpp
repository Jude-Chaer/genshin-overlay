#include "item_catalog.hpp"
#include "item_files.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace ItemCatalog {
	static_assert(sizeof(Point) == 40, "a bigger point costs about 1 MB for every 8 bytes");

	static std::mutex lock;
	static std::shared_ptr<const Catalog> shared;
	static std::atomic<uint64_t> lastVersion = 0;

	// an integer from min to max, false for anything else (missing, null, a string, too big)
	template <typename T>
	static bool integer(const json& from, const char* key, T& out, int64_t min = 0, int64_t max = std::numeric_limits<T>::max()) {
		if (!from.is_object()) return false;
		auto it = from.find(key);
		if (it == from.end() || !it->is_number_integer()) return false;
		if (it->is_number_unsigned() && it->get<uint64_t>() > (uint64_t)std::numeric_limits<int64_t>::max()) return false;
		int64_t value = it->get<int64_t>();
		if (value < min || value > max) return false;
		out = (T)value;
		return true;
	}

	static bool number(const json& from, const char* key, double& out) {
		if (!from.is_object()) return false;
		auto it = from.find(key);
		if (it == from.end() || !it->is_number()) return false;
		out = it->get<double>();
		return std::isfinite(out);
	}

	// a name, empty when it's missing (the builder writes null then), so a group
	// without a name still shows its types
	static std::string name(const json& from) {
		auto it = from.find("name");
		return it != from.end() && it->is_string() ? it->get<std::string>() : std::string();
	}

	// the point, or the name of the first field it's missing (or that doesn't fit)
	static const char* readPoint(const json& from, uint16_t type, Point& point) {
		point = {};
		point.type = type;
		if (!integer(from, "id", point.id, 1)) return "id";
		if (!integer(from, "map_id", point.map)) return "map_id";
		if (!integer(from, "area_id", point.area)) return "area_id";
		if (!number(from, "x_pos", point.x)) return "x_pos";
		if (!number(from, "y_pos", point.y)) return "y_pos";
		auto group = from.find("point_group");
		if (group != from.end() && !group->is_null()) {
			if (!integer(*group, "group_id", point.group)) return "point_group.group_id";
			if (!integer(*group, "floor_id", point.floor)) return "point_group.floor_id";
		}
		// the pair is extra, a bad one costs the pair and not the point
		if (from.contains("appsample") && !integer(from, "appsample", point.appsample, 1)) point.appsample = 0;
		return nullptr;
	}

	// "<type>.json" in positions/ or "<type>.png" in icons/, false for any other name
	static bool typeOf(const std::string& name, const std::string& folder, const std::string& extension, uint16_t& type) {
		if (name.size() <= folder.size() + extension.size() || name.compare(0, folder.size(), folder) != 0
			|| name.compare(name.size() - extension.size(), extension.size(), extension) != 0) return false;
		std::string stem = name.substr(folder.size(), name.size() - folder.size() - extension.size());
		if (stem.empty() || stem.size() > 5 || stem[0] == '0' || !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
		int value = std::stoi(stem);
		if (value < 1 || value > std::numeric_limits<uint16_t>::max()) return false;
		type = (uint16_t)value;
		return true;
	}

	std::shared_ptr<Catalog> load(const std::string& raw, const Net::Stopper* stop) {
		auto stopped = [&]() { return stop && stop->stopped(); };
		auto catalog = std::make_shared<Catalog>();
		json tree = json(json::value_t::discarded), areas = tree;
		std::vector<std::pair<uint16_t, std::string_view>> positions, icons;
		// the pack is sorted by name, but the order of the reads below doesn't lean on it
		bool intact = ItemFiles::readPack(raw, [&](const std::string& name, std::string_view data) {
			uint16_t type;
			if (name == "tree.json") tree = json::parse(data, nullptr, false);
			else if (name == "areas.json") areas = json::parse(data, nullptr, false);
			else if (typeOf(name, "positions/", ".json", type)) positions.push_back({ type, data });
			else if (typeOf(name, "icons/", ".png", type)) icons.push_back({ type, data });
			return !stopped();
		});
		// no positions at all is a broken pack, not a week without points
		if (!intact || !tree.is_array() || !areas.is_array() || positions.empty()) return nullptr;
		std::sort(positions.begin(), positions.end());
		std::sort(icons.begin(), icons.end());

		std::map<int, std::map<int, int>> counts;  // type: map: points
		for (const auto& [type, data] : positions) {
			if (stopped()) return nullptr;
			json points = json::parse(data, nullptr, false);
			if (!points.is_array()) {
				catalog->brokenFiles++;
				continue;
			}
			Catalog::Slot slot{ type, (uint32_t)catalog->all.size(), 0 };
			std::map<int, int>& perMap = counts[type];
			for (const json& from : points) {
				Point point;
				if (const char* missing = readPoint(from, type, point)) {
					catalog->skipped++;
					catalog->skippedBy[missing]++;
					continue;
				}
				catalog->all.push_back(point);
				perMap[point.map]++;
			}
			slot.count = (uint32_t)(catalog->all.size() - slot.first);
			catalog->slots.push_back(slot);
		}
		catalog->all.shrink_to_fit();

		// copied out of the pack, so the pack can go once this is loaded
		size_t iconBytes = 0;
		for (const auto& icon : icons) iconBytes += icon.second.size();
		catalog->icons.reserve(iconBytes);
		// an icon without points still counts, the type list shows it
		for (const auto& [type, data] : icons) {
			auto slot = std::lower_bound(catalog->slots.begin(), catalog->slots.end(), (int)type, [](const Catalog::Slot& s, int t) { return s.type < t; });
			if (slot == catalog->slots.end() || slot->type != type) slot = catalog->slots.insert(slot, Catalog::Slot{ type, 0, 0 });
			slot->iconAt = (uint32_t)catalog->icons.size();
			slot->iconSize = (uint32_t)data.size();
			catalog->icons.append(data);
		}

		catalog->ids.reserve(catalog->all.size());
		for (uint32_t i = 0; i < catalog->all.size(); i++) catalog->ids.push_back({ catalog->all[i].id, i });
		std::sort(catalog->ids.begin(), catalog->ids.end());

		for (const json& from : tree) {
			TypeGroup group;
			if (!integer(from, "id", group.id)) continue;
			group.name = name(from);
			// the builder writes each group, its types and each region once; a type in two groups shows in both
			if (from.contains("children") && from["children"].is_array()) {
				for (const json& child : from["children"]) {
					Type type;
					if (!integer(child, "id", type.id)) continue;
					type.name = name(child);
					auto perMap = counts.find(type.id);
					if (perMap != counts.end()) {
						for (const auto& [map, n] : perMap->second) type.counts.push_back({ map, n });
					}
					group.types.push_back(std::move(type));
				}
			}
			catalog->groups.push_back(std::move(group));
		}

		for (const json& from : areas) {
			Region region;
			if (!integer(from, "id", region.id) || !integer(from, "map_id", region.map)) continue;
			region.name = name(from);
			if (!number(from, "l_x", region.left) || !number(from, "l_y", region.top) || !number(from, "r_x", region.right) || !number(from, "r_y", region.bottom)) continue;
			catalog->regions.push_back(std::move(region));
		}
		return catalog;
	}

	const Catalog::Slot* Catalog::slotOf(int type) const {
		auto slot = std::lower_bound(slots.begin(), slots.end(), type, [](const Slot& s, int t) { return s.type < t; });
		return slot == slots.end() || slot->type != type ? nullptr : &*slot;
	}

	Points Catalog::points(int type) const {
		const Slot* slot = slotOf(type);
		if (!slot || slot->count == 0) return {};
		return { all.data() + slot->first, slot->count };
	}

	const Point* Catalog::point(int64_t id) const {
		if (id < 1 || id > std::numeric_limits<int32_t>::max()) return nullptr;
		auto it = std::lower_bound(ids.begin(), ids.end(), (int32_t)id, [](const auto& entry, int32_t i) { return entry.first < i; });
		return it == ids.end() || it->first != id ? nullptr : &all[it->second];
	}

	int32_t Catalog::appsamplePair(int64_t id) const {
		const Point* found = point(id);
		return found ? found->appsample : 0;
	}

	std::string_view Catalog::icon(int type) const {
		const Slot* slot = slotOf(type);
		if (!slot || slot->iconSize == 0) return {};
		return std::string_view(icons).substr(slot->iconAt, slot->iconSize);
	}

	void publish(std::shared_ptr<Catalog> catalog) {
		// a catalog is published once, readers may already hold it
		if (!catalog || catalog->version != 0) return;
		std::lock_guard<std::mutex> guard(lock);
		catalog->version = lastVersion + 1;
		shared = std::move(catalog);
		lastVersion = shared->version;
	}

	std::shared_ptr<const Catalog> current() {
		std::lock_guard<std::mutex> guard(lock);
		return shared;
	}

	uint64_t version() {
		return lastVersion;
	}
}
