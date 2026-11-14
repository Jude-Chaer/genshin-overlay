#pragma once
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include "lit_floor.hpp"

// Finds where the in-game map is looking from a screenshot. The screenshot is
// matched against HoYoLAB's map art (SIFT + RANSAC). Every map and underground
// floor is in one index, with keypoints stored in HoYoLAB map coordinates.

namespace MapLocator {

    struct Result {
        bool found = false;
        int mapId = 0;     // HoYoLAB map id, 2 is Teyvat
        int groupId = 0;   // underground area, 0 on the surface
        int floorId = 0;
        // the floor is the one the game draws lit, seen on the screen
        bool drawnLit = false;
        // map position under the (cx, cy) pixel passed to Match
        double lat = 0.0;
        double lng = 0.0;
        double unitsPerPixel = 0.0;
        int inliers = 0;
    };

    // FNV-1a 64 of the manifest text, index.bin stores it to know what it was built from
    uint64_t ManifestSignature(const std::string& manifest);
    // true if index.bin at this path was built from this manifest
    bool IndexMatchesManifest(const std::filesystem::path& indexPath, const std::string& manifest);

    // 255 where a w x h map screen shows the map, 0 over the game's UI
    cv::Mat GameUiMask(int w, int h);

    class Index {
    public:
        Index() = default;
        Index(const Index&) = delete;
        Index& operator=(const Index&) = delete;

        // Reads index.bin next to the manifest, or builds it from the reference
        // images if it's missing or stale. Takes ~2s from the cache, ~10s to build.
        bool Load(const std::filesystem::path& manifest, const std::atomic<bool>* cancel = nullptr, int maxThreads = 12);
        void Unload();
        bool IsReady() const { return !m_descriptors.empty() && m_kd != nullptr; }

        // fast skips the full resolution pass, used while tracking.
        // searchNearLast retries around the last position when nothing else matched, which is slow.
        Result Match(const cv::Mat& bgr, double cx, double cy, bool fast = false, bool searchNearLast = true);

        // With fast, Match tries the game 480 wide first and 960 only if that
        // isn't a strong match. Quicker when 480 is enough, slower when it isn't.
        bool smallFirst = false;

    private:
        struct Piece {
            int mapId = 0;
            int groupId = 0;
            int floorId = 0;
        };

        struct Fit {
            int inliers = 0;
            int piece = -1;
            cv::Mat transform;
        };

        // a floor image from the manifest, its art read when it's first in view
        struct FloorImage {
            Piece piece;
            std::filesystem::path path;
            double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0;
        };

        struct Kd;

        bool LoadImpl(const std::filesystem::path& manifest, const std::atomic<bool>* cancel, int maxThreads);
        bool Build(const std::filesystem::path& manifest, const std::vector<uchar>& bytes, const std::filesystem::path& cachePath,
            uint64_t signature, const std::atomic<bool>* cancel, int maxThreads);
        bool ReadCache(const std::filesystem::path& cachePath, uint64_t signature);
        void WriteCache(const std::filesystem::path& cachePath, uint64_t signature);
        Result MatchImpl(const cv::Mat& bgr, double cx, double cy, bool fast, bool searchNearLast);
        void ReadFloorImages(const std::filesystem::path& manifest, const std::vector<uchar>& bytes);
        const FloorImage* LitFloorAt(const cv::Mat& gray, const cv::Mat& mask, const cv::Mat& transform, int mapId);
        void Knn(const cv::Mat& query, std::vector<std::vector<cv::DMatch>>& out) const;
        void TryMatch(const std::vector<cv::Point2f>& queryPoints, const std::vector<std::vector<cv::DMatch>>& knn,
            const std::vector<int>* subset, Fit& surface, Fit& floor);

        std::vector<Piece> m_pieces;
        std::vector<cv::Point2f> m_points;
        std::vector<int> m_pieceOf;
        std::vector<FloorImage> m_floorImages;
        std::map<size_t, FloorArt> m_floorArt;  // by m_floorImages index
        // n x 128 CV_8U, the KD-trees point into this so there's only one copy
        cv::Mat m_descriptors;
        std::shared_ptr<Kd> m_kd;
        cv::Ptr<cv::SIFT> m_sift;

        bool m_hasLast = false;
        Result m_last;
    };

}
