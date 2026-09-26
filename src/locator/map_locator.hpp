#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>

// Finds where the in-game map is looking from a screenshot. The screenshot is
// matched against HoYoLAB's map art (SIFT + RANSAC). Every map and underground
// floor is in one index, with keypoints stored in HoYoLAB map coordinates.

namespace MapLocator {

    struct Result {
        bool found = false;
        int mapId = 0;     // HoYoLAB map id, 2 is Teyvat
        int groupId = 0;   // underground area, 0 on the surface
        int floorId = 0;
        // map position under the (cx, cy) pixel passed to Match
        double lat = 0.0;
        double lng = 0.0;
        double unitsPerPixel = 0.0;
        int inliers = 0;
    };

    struct TrackStep {
        enum Kind {
            Still,  // frame didn't change
            Miss,   // no map on screen
            Apply,  // new position in result
        };
        Kind kind = Miss;
        Result result;
    };

    // FNV-1a 64 of the manifest text, index.bin stores it to know what it was built from
    uint64_t ManifestSignature(const std::string& manifest);
    // true if index.bin at this path was built from this manifest
    bool IndexMatchesManifest(const std::filesystem::path& indexPath, const std::string& manifest);

    class Index {
    public:
        Index() = default;
        Index(const Index&) = delete;
        Index& operator=(const Index&) = delete;

        // Reads index.bin next to the manifest, or builds it from the reference
        // images if it's missing or stale. Takes ~2s from the cache, ~10s to build.
        bool Load(const std::filesystem::path& manifest, const std::atomic<bool>* cancel = nullptr, int maxThreads = 12);
        bool IsReady() const { return !m_descriptors.empty() && m_kd != nullptr; }

        // fast skips the full resolution pass, used while tracking.
        // searchNearLast retries around the last position when nothing else matched, which is slow.
        Result Match(const cv::Mat& bgr, double cx, double cy, bool fast = false, bool searchNearLast = true);

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

        struct Kd;

        bool LoadImpl(const std::filesystem::path& manifest, const std::atomic<bool>* cancel, int maxThreads);
        bool Build(const std::filesystem::path& manifest, const std::vector<uchar>& bytes, const std::filesystem::path& cachePath,
            uint64_t signature, const std::atomic<bool>* cancel, int maxThreads);
        bool ReadCache(const std::filesystem::path& cachePath, uint64_t signature);
        void WriteCache(const std::filesystem::path& cachePath, uint64_t signature);
        Result MatchImpl(const cv::Mat& bgr, double cx, double cy, bool fast, bool searchNearLast);
        void Knn(const cv::Mat& query, std::vector<std::vector<cv::DMatch>>& out) const;
        void TryMatch(const std::vector<cv::Point2f>& queryPoints, const std::vector<std::vector<cv::DMatch>>& knn,
            const std::vector<int>* subset, Fit& surface, Fit& floor);

        std::vector<Piece> m_pieces;
        std::vector<cv::Point2f> m_points;
        std::vector<int> m_pieceOf;
        // n x 128 CV_8U, the KD-trees point into this so there's only one copy
        cv::Mat m_descriptors;
        std::shared_ptr<Kd> m_kd;
        cv::Ptr<cv::SIFT> m_sift;

        bool m_hasLast = false;
        Result m_last;
    };

    // Follows the map frame by frame. Without a fix it only takes a quick look,
    // since most frames are gameplay and not the map. Once there's a fix it
    // matches at half resolution with a full match every few ticks. Switching to
    // another map or floor needs two full matches in a row that agree.
    class Tracker {
    public:
        void Reset();
        TrackStep Step(Index& index, const cv::Mat& bgr, double cx, double cy);

    private:
        bool Decide(const Result& result, bool fromFull, Result& out);

        cv::Mat m_lastThumb;
        bool m_hasFix = false;
        Result m_fix;
        bool m_claimed = false;
        Result m_claim;
        int m_ticksSinceFull = 0;
        int m_misses = 0;
        bool m_lastMissed = false;
    };

}
