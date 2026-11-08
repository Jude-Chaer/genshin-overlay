#include "appsample.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <unordered_map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace Appsample {
    using Types = std::unordered_map<int, std::vector<const Point*>>;  // type: appsample points of it

    // types with more points than this add more noise than votes
    static constexpr size_t RARE = 20;
    static constexpr size_t AREA_RARE = 400;
    static constexpr size_t SAMPLE = 3000;
    // in HoYoLAB map pixels
    static constexpr double CLOSE = 6.0;
    static constexpr int MIN_VOTES = 10;
    // how far around a point the pairs that give its own shift are, in HoYoLAB pixels, and how many it needs
    static constexpr double LOCAL = 300.0;
    static constexpr int MIN_NEIGHBOURS = 5;
    // An appsample point with no floor matches our points on any floor (see sameFloor),
    // but one on the same floor goes first unless it's this much further, in HoYoLAB pixels.
    static constexpr double OTHER_FLOOR = 1.0;

    static int typeOf(const json& name) {
        if (!name.is_string()) return 0;
        const std::string& text = name.get_ref<const std::string&>();
        if (text.size() < 2 || text.size() > 9 || text[0] != 'o') return 0;
        for (size_t i = 1; i < text.size(); i++) {
            if (text[i] < '0' || text[i] > '9') return 0;
        }
        return std::stoi(text.substr(1));
    }

    // "l=<ids>|<group>|<floor>", sometimes with "i;" in front: the floor
    // underground, as HoYoLAB numbers it. Anything else is the surface.
    static void floorOf(const json& meta, Point& point) {
        if (!meta.is_string()) return;
        const std::string& text = meta.get_ref<const std::string&>();
        size_t first = text.find('|');
        if (first == std::string::npos) return;
        size_t second = text.find('|', first + 1);
        auto digits = [&](size_t from, size_t to, int& out) {
            if (to <= from || to - from > 6) return false;
            for (size_t i = from; i < to; i++) {
                if (text[i] < '0' || text[i] > '9') return false;
            }
            out = std::stoi(text.substr(from, to - from));
            return true;
        };
        if (second == std::string::npos || !digits(first + 1, second, point.group)) return;
        // a few have the group and no floor
        if (!digits(second + 1, text.size(), point.floor)) point.floor = 0;
    }

    bool decode(const std::string& body, std::vector<Point>& out) {
        json file = json::parse(body, nullptr, false);
        if (file.is_discarded() || !file.is_object() || !file.contains("headers") || !file.contains("data")) return false;
        // with a code, the digits of lng and lat are swapped around
        if (file.contains("code") && file["code"] != 0) return false;

        const json& headers = file["headers"];
        const json& data = file["data"];
        if (!headers.is_array()) return false;
        // meta is extra, without it every point is on the surface
        const int COLUMNS = 6, NEEDED = 5;
        int column[COLUMNS] = { -1, -1, -1, -1, -1, -1 };
        const char* names[COLUMNS] = { "id", "type", "mid", "lng", "lat", "meta" };
        for (int i = 0; i < (int)headers.size(); i++) {
            for (int c = 0; c < COLUMNS; c++) {
                if (headers[i] == names[c]) column[c] = i;
            }
        }
        for (int c = 0; c < NEEDED; c++) {
            if (column[c] < 0) return false;
        }
        bool hasMeta = column[5] >= 0;

        // either one array per column with the ids as steps, or one array per point
        bool byColumn = file.contains("compressionVersion") && file["compressionVersion"] == 1;
        size_t count = 0;
        if (byColumn) {
            if (!data.is_object()) return false;
            for (int c = 0; c < NEEDED; c++) {
                if (!data.contains(names[c]) || !data[names[c]].is_array()) return false;
            }
            count = data["id"].size();
            for (int c = 0; c < NEEDED; c++) {
                if (data[names[c]].size() != count) return false;
            }
            const char* meta = names[5];
            if (hasMeta && (!data.contains(meta) || !data[meta].is_array() || data[meta].size() != count)) hasMeta = false;
        }
        else {
            if (!data.is_array()) return false;
            count = data.size();
        }

        int widest = *std::max_element(column, column + NEEDED);
        int64_t id = 0;
        for (size_t i = 0; i < count; i++) {
            auto value = [&](int c) -> const json& {
                return byColumn ? data[names[c]][i] : data[i][column[c]];
            };
            if (!byColumn && (!data[i].is_array() || data[i].size() <= (size_t)widest)) continue;
            if (!value(0).is_number_integer()) return false;
            id = byColumn ? id + value(0).get<int64_t>() : value(0).get<int64_t>();

            int type = typeOf(value(1));
            if (type <= 0 || !value(2).is_number_integer() || !value(3).is_number() || !value(4).is_number()) continue;
            Point point;
            point.id = id;
            point.map = value(2).get<int>();
            point.type = type;
            point.lng = value(3).get<double>();
            point.lat = value(4).get<double>();
            // a row too short for meta is on the surface
            if (hasMeta && (byColumn || data[i].size() > (size_t)column[5])) floorOf(value(5), point);
            out.push_back(point);
        }
        return !out.empty();
    }

    struct Peak {
        int votes = 0;
        double x = 0, y = 0;
    };

    // Where an appsample point lands in HoYoLAB pixels at this scale, before
    // the offset. appsample's lat goes up, HoYoLAB's y goes down.
    static double xOf(const Point& other, double scale) { return other.lng / scale; }
    static double yOf(const Point& other, double scale) { return -other.lat / scale; }

    // The most common gap between same type points of both sites, in HoYoLAB pixels.
    static Peak peak(const std::vector<const Ours*>& points, const Types& types, double scale, double bin, size_t rare) {
        std::unordered_map<int64_t, int> votes;
        for (const Ours* point : points) {
            auto found = types.find(point->type);
            if (found == types.end() || found->second.size() > rare) continue;
            for (const Point* other : found->second) {
                int64_t bx = std::llround((xOf(*other, scale) - point->x) / bin);
                int64_t by = std::llround((yOf(*other, scale) - point->y) / bin);
                votes[(int64_t)(((uint64_t)bx << 32) ^ ((uint64_t)by & 0xffffffff))]++;
            }
        }
        Peak best;
        int64_t bestKey = 0;
        for (const auto& [key, count] : votes) {
            if (count > best.votes || (count == best.votes && key < bestKey)) {
                best.votes = count;
                bestKey = key;
            }
        }
        best.x = (double)(bestKey >> 32) * bin;
        best.y = (double)(int32_t)(bestKey & 0xffffffff) * bin;
        return best;
    }

    // how wide the middle 90% of the values is
    static double spread(std::vector<double> values) {
        if (values.size() < 2) return 0;
        std::sort(values.begin(), values.end());
        return values[values.size() * 95 / 100] - values[values.size() * 5 / 100];
    }

    // Starts from how wide each site's points spread, then the scale with the
    // clearest peak. Only rare types vote, on a sample of the points.
    static double findScale(const std::vector<const Ours*>& points, const std::vector<const Point*>& others, const Types& types) {
        std::vector<double> ours, theirs;
        for (const Ours* point : points) ours.push_back(point->x);
        for (const Point* other : others) theirs.push_back(other->lng);
        double ourSpread = spread(ours);
        if (ourSpread <= 0) return 0;
        double guess = spread(theirs) / ourSpread;
        if (guess <= 0) return 0;

        std::vector<const Ours*> rare;
        for (const Ours* point : points) {
            auto found = types.find(point->type);
            if (found != types.end() && found->second.size() <= RARE) rare.push_back(point);
        }
        std::vector<const Ours*> sample;
        size_t step = std::max<size_t>(1, rare.size() / SAMPLE);
        for (size_t i = 0; i < rare.size(); i += step) sample.push_back(rare[i]);

        int bestVotes = 0;
        double best = 0;
        for (int k = -350; k <= 350; k++) {
            double scale = guess * std::exp(k * 0.004);
            int votes = peak(sample, types, scale, 8, RARE).votes;
            if (votes > bestVotes) { bestVotes = votes; best = scale; }
        }
        if (bestVotes < MIN_VOTES) return 0;
        // then finer around it, against its own count in the finer bins
        double coarse = best;
        int fineVotes = peak(sample, types, coarse, 4, RARE).votes;
        for (int k = -50; k <= 50; k++) {
            double scale = coarse * (1 + k * 0.0001);
            int votes = peak(sample, types, scale, 4, RARE).votes;
            if (votes > fineVotes) { fineVotes = votes; best = scale; }
        }
        return best;
    }

    // appsample names the floor of most underground points, and then it has to
    // be ours (a few name only the group). One it doesn't name can be either:
    // caves, pathways and waypoints sit between floors, HoYoLAB files them
    // under one and appsample on the surface (544 of the pairs, 2026-10-26).
    static bool sameFloor(const Ours& point, const Point& other) {
        if (other.group == 0) return true;
        return point.group == other.group && (other.floor == 0 || point.floor == other.floor);
    }

    struct Shift {
        double x = 0, y = 0;
    };
    // distance, our point, theirs
    using Close = std::vector<std::tuple<double, const Ours*, const Point*>>;

    // every same type, same floor point of theirs closer than CLOSE once our points are shifted
    static Close candidates(const std::unordered_map<const Ours*, Shift>& shifts, const Types& types, double scale) {
        Close close;
        for (const auto& [point, shift] : shifts) {
            auto found = types.find(point->type);
            if (found == types.end()) continue;
            for (const Point* other : found->second) {
                if (!sameFloor(*point, *other)) continue;
                double distance = std::hypot(xOf(*other, scale) - shift.x - point->x, yOf(*other, scale) - shift.y - point->y);
                if (distance >= CLOSE) continue;
                // ours underground and theirs with no floor: a surface point stacked on ours may be the one
                if (point->group != 0 && other->group == 0) distance += OTHER_FLOOR;
                close.emplace_back(distance, point, other);
            }
        }
        return close;
    }

    // closest first, each point once
    static Close match(Close close) {
        std::sort(close.begin(), close.end(), [](const auto& a, const auto& b) {
            return std::make_tuple(std::get<0>(a), std::get<1>(a)->id, std::get<2>(a)->id) < std::make_tuple(std::get<0>(b), std::get<1>(b)->id, std::get<2>(b)->id);
        });
        std::set<int64_t> ourTaken, theirTaken;
        Close pairs;
        for (const auto& candidate : close) {
            if (!ourTaken.insert(std::get<1>(candidate)->id).second) continue;
            if (!theirTaken.insert(std::get<2>(candidate)->id).second) {
                ourTaken.erase(std::get<1>(candidate)->id);
                continue;
            }
            pairs.push_back(candidate);
        }
        return pairs;
    }

    static double median(std::vector<double>& values) {
        std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
        return values[values.size() / 2];
    }

    // Each point's own shift: the middle of the shifts of the pairs around it
    // (not its own), or what it had when there are too few.
    static void shiftsFromNeighbours(const Close& pairs, double scale, std::unordered_map<const Ours*, Shift>& shifts) {
        struct Seen { const Ours* point; Shift shift; };
        std::map<std::pair<int64_t, int64_t>, std::vector<Seen>> cells;
        auto cellOf = [](double v) { return (int64_t)std::floor(v / LOCAL); };
        for (const auto& [distance, point, other] : pairs) {
            cells[{ cellOf(point->x), cellOf(point->y) }].push_back({ point, { xOf(*other, scale) - point->x, yOf(*other, scale) - point->y } });
        }
        for (auto& [point, shift] : shifts) {
            std::vector<double> xs, ys;
            for (int64_t cx = cellOf(point->x) - 1; cx <= cellOf(point->x) + 1; cx++) {
                for (int64_t cy = cellOf(point->y) - 1; cy <= cellOf(point->y) + 1; cy++) {
                    auto cell = cells.find({ cx, cy });
                    if (cell == cells.end()) continue;
                    for (const Seen& seen : cell->second) {
                        // another region can be moved differently on appsample's map
                        if (seen.point == point || seen.point->area != point->area || std::abs(seen.point->x - point->x) >= LOCAL || std::abs(seen.point->y - point->y) >= LOCAL) continue;
                        xs.push_back(seen.shift.x);
                        ys.push_back(seen.shift.y);
                    }
                }
            }
            if ((int)xs.size() < MIN_NEIGHBOURS) continue;
            shift = { median(xs), median(ys) };
        }
    }

    Result pair(const std::vector<Ours>& hoyolab, const std::vector<Point>& appsample) {
        Result result;
        std::map<int, std::map<int, std::vector<const Ours*>>> ours;  // map, area
        std::map<int, std::vector<const Point*>> theirs;
        for (const Ours& point : hoyolab) ours[point.map][point.area].push_back(&point);
        for (const Point& point : appsample) theirs[point.map].push_back(&point);

        for (const auto& [map, areas] : ours) {
            MapResult& stats = result.maps[map];
            for (const auto& [area, points] : areas) stats.ours += (int)points.size();
            auto others = theirs.find(map);
            if (others == theirs.end()) {
                stats.skipped = "appsample has no points on it";
                continue;
            }
            stats.theirs = (int)others->second.size();
            Types types;
            for (const Point* other : others->second) types[other->type].push_back(other);

            std::vector<const Ours*> all;
            for (const auto& [area, points] : areas) all.insert(all.end(), points.begin(), points.end());
            double scale = findScale(all, others->second, types);
            if (scale <= 0) {
                stats.skipped = "no scale fits it";
                continue;
            }
            stats.scale = scale;

            // First each region's offset: a few regions are moved on appsample's map.
            std::unordered_map<const Ours*, Shift> shifts;
            for (const auto& [area, points] : areas) {
                Peak offset = peak(points, types, scale, 4, AREA_RARE);
                std::vector<double> xs, ys;
                for (const Ours* point : points) {
                    auto found = types.find(point->type);
                    if (found == types.end() || found->second.size() > AREA_RARE) continue;
                    for (const Point* other : found->second) {
                        double dx = xOf(*other, scale) - point->x - offset.x;
                        double dy = yOf(*other, scale) - point->y - offset.y;
                        if (std::abs(dx) < CLOSE && std::abs(dy) < CLOSE) {
                            xs.push_back(dx);
                            ys.push_back(dy);
                        }
                    }
                }
                if ((int)xs.size() < MIN_VOTES) {
                    stats.unplaced += (int)points.size();
                    continue;
                }
                Shift shift = { offset.x + median(xs), offset.y + median(ys) };
                for (const Ours* point : points) shifts[point] = shift;
            }

            // Then each point's own: appsample's map drifts by a pixel or two
            // inside a region, as much as two points of a pair of chests are
            // apart, so with the region's offset alone such pairs cross.
            shiftsFromNeighbours(match(candidates(shifts, types, scale)), scale, shifts);
            std::vector<double> distances;
            for (const auto& [distance, point, other] : match(candidates(shifts, types, scale))) {
                result.pairs[point->id] = other->id;
                distances.push_back(distance);
            }
            stats.paired = (int)distances.size();
            if (!distances.empty()) stats.residual = median(distances);
        }
        return result;
    }
}
