#include "appsample.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <unordered_map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace Appsample {
    using Types = std::unordered_map<int, std::vector<const Point*>>;

    // types with more points than this add more noise than votes
    static constexpr size_t RARE = 20;
    static constexpr size_t AREA_RARE = 400;
    static constexpr size_t SAMPLE = 3000;
    // in HoYoLAB map pixels
    static constexpr double CLOSE = 6.0;
    static constexpr int MIN_VOTES = 10;

    static int typeOf(const json& name) {
        if (!name.is_string()) return 0;
        const std::string& text = name.get_ref<const std::string&>();
        if (text.size() < 2 || text.size() > 9 || text[0] != 'o') return 0;
        for (size_t i = 1; i < text.size(); i++) {
            if (text[i] < '0' || text[i] > '9') return 0;
        }
        return std::stoi(text.substr(1));
    }

    bool decode(const std::string& body, std::vector<Point>& out) {
        json file = json::parse(body, nullptr, false);
        if (file.is_discarded() || !file.is_object() || !file.contains("headers") || !file.contains("data")) return false;
        // with a code, the digits of lng and lat are swapped around
        if (file.value("code", 0) != 0) return false;

        const json& headers = file["headers"];
        const json& data = file["data"];
        if (!headers.is_array()) return false;
        int column[5] = { -1, -1, -1, -1, -1 };
        const char* names[5] = { "id", "type", "mid", "lng", "lat" };
        for (int i = 0; i < (int)headers.size(); i++) {
            for (int c = 0; c < 5; c++) {
                if (headers[i] == names[c]) column[c] = i;
            }
        }
        for (int c : column) {
            if (c < 0) return false;
        }

        // either one array per column with the ids as steps, or one array per point
        bool byColumn = file.value("compressionVersion", 0) == 1;
        size_t count = 0;
        if (byColumn) {
            if (!data.is_object()) return false;
            for (const char* name : names) {
                if (!data.contains(name) || !data[name].is_array()) return false;
            }
            count = data["id"].size();
            for (const char* name : names) {
                if (data[name].size() != count) return false;
            }
        }
        else {
            if (!data.is_array()) return false;
            count = data.size();
        }

        int64_t id = 0;
        for (size_t i = 0; i < count; i++) {
            auto value = [&](int c) -> const json& {
                return byColumn ? data[names[c]][i] : data[i][column[c]];
            };
            if (!byColumn && (!data[i].is_array() || data[i].size() <= (size_t)*std::max_element(column, column + 5))) continue;
            if (!value(0).is_number_integer()) return false;
            id = byColumn ? id + value(0).get<int64_t>() : value(0).get<int64_t>();

            int type = typeOf(value(1));
            if (type <= 0 || !value(2).is_number_integer() || !value(3).is_number() || !value(4).is_number()) continue;
            out.push_back({ id, value(2).get<int>(), type, 0, value(3).get<double>(), value(4).get<double>() });
        }
        return !out.empty();
    }

    struct Peak {
        int votes = 0;
        double x = 0, y = 0;
    };

    // The most common gap between same type points of both sites, in HoYoLAB
    // pixels. appsample's y goes up, HoYoLAB's goes down.
    static Peak peak(const std::vector<const Point*>& points, const Types& types, double scale, double bin, size_t rare) {
        std::unordered_map<int64_t, int> votes;
        for (const Point* point : points) {
            auto found = types.find(point->type);
            if (found == types.end() || found->second.size() > rare) continue;
            for (const Point* other : found->second) {
                int64_t bx = std::llround((other->x / scale - point->x) / bin);
                int64_t by = std::llround((-other->y / scale - point->y) / bin);
                votes[(bx << 32) ^ (by & 0xffffffff)]++;
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
    static double findScale(const std::vector<const Point*>& points, const std::vector<const Point*>& others, const Types& types) {
        std::vector<double> ours, theirs;
        for (const Point* point : points) ours.push_back(point->x);
        for (const Point* other : others) theirs.push_back(other->x);
        double ourSpread = spread(ours);
        if (ourSpread <= 0) return 0;
        double guess = spread(theirs) / ourSpread;
        if (guess <= 0) return 0;

        std::vector<const Point*> rare;
        for (const Point* point : points) {
            auto found = types.find(point->type);
            if (found != types.end() && found->second.size() <= RARE) rare.push_back(point);
        }
        std::vector<const Point*> sample;
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
        double coarse = best;
        for (int k = -50; k <= 50; k++) {
            double scale = coarse * (1 + k * 0.0001);
            int votes = peak(sample, types, scale, 4, RARE).votes;
            if (votes > bestVotes) { bestVotes = votes; best = scale; }
        }
        return best;
    }

    std::map<int64_t, int64_t> pair(const std::vector<Point>& hoyolab, const std::vector<Point>& appsample) {
        std::map<int, std::map<int, std::vector<const Point*>>> ours;  // map, area
        std::map<int, std::vector<const Point*>> theirs;
        for (const Point& point : hoyolab) ours[point.map][point.area].push_back(&point);
        for (const Point& point : appsample) theirs[point.map].push_back(&point);

        // distance, HoYoLAB id, appsample id
        std::vector<std::tuple<double, int64_t, int64_t>> close;
        for (const auto& [map, areas] : ours) {
            auto others = theirs.find(map);
            if (others == theirs.end()) continue;
            Types types;
            for (const Point* other : others->second) types[other->type].push_back(other);

            std::vector<const Point*> all;
            for (const auto& [area, points] : areas) all.insert(all.end(), points.begin(), points.end());
            double scale = findScale(all, others->second, types);
            if (scale <= 0) continue;

            // a few regions are moved on appsample's map, so each gets its own offset
            for (const auto& [area, points] : areas) {
                Peak offset = peak(points, types, scale, 4, AREA_RARE);
                std::vector<double> xs, ys;
                for (const Point* point : points) {
                    auto found = types.find(point->type);
                    if (found == types.end() || found->second.size() > AREA_RARE) continue;
                    for (const Point* other : found->second) {
                        double dx = other->x / scale - point->x - offset.x;
                        double dy = -other->y / scale - point->y - offset.y;
                        if (std::abs(dx) < CLOSE && std::abs(dy) < CLOSE) {
                            xs.push_back(dx);
                            ys.push_back(dy);
                        }
                    }
                }
                if ((int)xs.size() < MIN_VOTES) continue;
                std::nth_element(xs.begin(), xs.begin() + xs.size() / 2, xs.end());
                std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
                double ox = offset.x + xs[xs.size() / 2], oy = offset.y + ys[ys.size() / 2];

                for (const Point* point : points) {
                    auto found = types.find(point->type);
                    if (found == types.end()) continue;
                    for (const Point* other : found->second) {
                        double distance = std::hypot(other->x / scale - ox - point->x, -other->y / scale - oy - point->y);
                        if (distance < CLOSE) close.emplace_back(distance, point->id, other->id);
                    }
                }
            }
        }

        // closest first, each point used once
        std::sort(close.begin(), close.end());
        std::map<int64_t, int64_t> pairs;
        std::set<int64_t> taken;
        for (const auto& [distance, ourId, theirId] : close) {
            if (pairs.count(ourId) || taken.count(theirId)) continue;
            pairs[ourId] = theirId;
            taken.insert(theirId);
        }
        return pairs;
    }
}
