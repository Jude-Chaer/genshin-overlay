#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "net_stop.hpp"

// The item files (item_files.hpp says what's in the pack) loaded once into
// plain structs, straight from the pack in memory, so nothing reads JSON again
// after that.
//
// A catalog is built whole and then handed over with publish. Readers take
// current() and keep the shared_ptr for as long as they use it, so they never
// see half a catalog, and an old one lives until its last reader lets go.

namespace ItemCatalog {
	// One point, 40 bytes (about 95,000 of them in the full pack). The small fields are
	// 16 bit because every value fits with room to spare (map 40, area 16,
	// group 234, floor 348, type 874). A point whose values don't fit is skipped.
	struct Point {
		int32_t id = 0;
		int32_t appsample = 0;  // appsample's id for the same point, 0 when it has none
		double x = 0, y = 0;    // HoYoLAB map units
		uint16_t map = 0;
		uint16_t area = 0;      // the region, by id (not the box the point sits in)
		uint16_t group = 0;     // group and floor when it's underground, 0 and 0 when not
		uint16_t floor = 0;
		uint16_t type = 0;
	};

	struct Type {
		int id = 0;
		std::string name;
		std::vector<std::pair<int, int>> counts;  // (map, points on it), by map; empty for a type with no points
	};

	struct TypeGroup {
		int id = 0;
		std::string name;
		std::vector<Type> types;
	};

	struct Region {
		int id = 0;
		int map = 0;
		std::string name;
		double left = 0, top = 0, right = 0, bottom = 0;
	};

	// the points of one type, in the order of its file
	struct Points {
		const Point* first = nullptr;
		size_t count = 0;
		const Point* begin() const { return first; }
		const Point* end() const { return first + count; }
		size_t size() const { return count; }
		bool empty() const { return count == 0; }
		const Point& operator[](size_t i) const { return first[i]; }
	};

	class Catalog {
	public:
		uint64_t version = 0;           // set by publish, 1 for the first one
		std::string sha256, built;      // of the pack it came from, set by ItemFiles::open
		std::vector<TypeGroup> groups;  // HoYoLAB's tree order, groups and types once each
		std::vector<Region> regions;    // every map's, once each

		// empty for a type it doesn't know
		Points points(int type) const;
		// nullptr for an id it doesn't know
		const Point* point(int64_t id) const;
		// appsample's id for a HoYoLAB point, 0 when there's no pair
		int32_t appsamplePair(int64_t id) const;
		// the icon's png bytes, empty when the type has none; they live as long as the catalog
		std::string_view icon(int type) const;
		size_t pointCount() const { return all.size(); }
		// every point, by type and then as in its file
		const std::vector<Point>& allPoints() const { return all; }
		// points left out because a value was missing or didn't fit
		size_t skippedPoints() const { return skipped; }
		// the same points by the field that left each out ("x_pos": 3)
		const std::map<std::string, size_t>& skippedFields() const { return skippedBy; }
		// positions files left out because they aren't a JSON list
		size_t skippedFiles() const { return brokenFiles; }

	private:
		friend std::shared_ptr<Catalog> load(const std::string& raw, const Net::Stopper* stop);
		struct Slot {
			int type = 0;
			uint32_t first = 0, count = 0;
			uint32_t iconAt = 0, iconSize = 0;  // in icons
		};
		std::vector<Point> all;                         // by type, then as in the file
		std::vector<Slot> slots;                        // by type
		std::vector<std::pair<int32_t, uint32_t>> ids;  // (point id, index in all), by id
		std::string icons;                              // every icon's png, one after the other
		size_t skipped = 0;
		std::map<std::string, size_t> skippedBy;
		size_t brokenFiles = 0;
		const Slot* slotOf(int type) const;
	};

	// Reads a raw format 2 pack (as ItemFiles::pack makes it). nullptr when
	// the pack isn't whole, tree.json, areas.json or every positions file is
	// missing, or stop came first. A positions file that isn't JSON costs only
	// its own type and is counted in skippedFiles; whether that's good enough
	// is for the caller to say.
	extern std::shared_ptr<Catalog> load(const std::string& raw, const Net::Stopper* stop = nullptr);

	// hands a new catalog to everyone and gives it the next version, once:
	// publishing the same one again does nothing
	extern void publish(std::shared_ptr<Catalog> catalog);
	// the catalog in use, nullptr until the first one is in
	extern std::shared_ptr<const Catalog> current();
	// 0 while there's none, goes up by one with each new catalog. Cheap to
	// check every frame; what you keep, take with its catalog's own version.
	extern uint64_t version();
}
