#include "map_locator.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <opencv2/calib3d.hpp>
#include <opencv2/flann/flann_base.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace fs = std::filesystem;

namespace MapLocator {

    // tuned on 17 real 4K screenshots
    static constexpr double REF_SCALE = 0.5;  // reference art is SIFT'd at half size
    static constexpr int QUERY_WIDTHS[] = { 1920, 960, 480 };
    static constexpr int FAST_QUERY_WIDTHS[] = { 960, 480 };
    static constexpr int SMALL_FIRST_WIDTHS[] = { 480, 960 };
    static constexpr int STRONG_INLIERS = 60;
    static constexpr int MIN_INLIERS = 12;  // real locks got 66+, wrong ones never above 8
    static constexpr double RATIO = 0.8;
    static constexpr int PIECES_TO_VERIFY = 6;
    static constexpr double RANSAC_THRESHOLD = 8.0;
    static constexpr double MAX_ROTATION_DEG = 3.0;
    static constexpr double LOCAL_RADIUS = 6000.0;
    static constexpr uint32_t INDEX_MAGIC = 0x494C5747;  // "GWLI"
    static constexpr uint32_t INDEX_VERSION = 2;
    static cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(3.0, cv::Size(8, 8));
    static cv::Mat m_matchMask;
    static int m_maskWidth = 0;
    static int m_maskHeight = 0;

    // Game UI to leave out of the match, in a 2000x1125 frame. Each rect is
    // anchored to the edge it sits on so other aspect ratios still line up.
    struct UiRect {
        int x0, y0, x1, y1;
        bool right;
    };
    static constexpr UiRect UI_RECTS[] = {
        { 0, 0, 370, 440, false },       // event banner and list
        { 0, 430, 90, 690, false },      // zoom slider
        { 0, 1015, 100, 1115, false },
        { 1030, 15, 2000, 85, true },    // resource bar
        { 1530, 1015, 2000, 1125, true },// region name
        { 1930, 600, 2000, 800, true },  // layer switcher
    };

    // manifest paths can use either slash
    static fs::path fromUtf8(std::string s) {
        std::replace(s.begin(), s.end(), '\\', '/');
        return fs::u8path(s);
    }

    static bool readFileBytes(const fs::path& path, std::vector<uchar>& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        file.seekg(0, std::ios::end);
        std::streamoff size = file.tellg();
        if (size <= 0) return false;
        file.seekg(0, std::ios::beg);
        out.resize((size_t)size);
        file.read((char*)out.data(), size);
        return (bool)file;
    }

    // cv::imread can't open non-ASCII paths on Windows
    static cv::Mat readImage(const fs::path& path, int flags) {
        std::vector<uchar> bytes;
        if (!readFileBytes(path, bytes)) return cv::Mat();
        try {
            return cv::imdecode(bytes, flags);
        }
        catch (...) {
            return cv::Mat();
        }
    }

    static uint64_t fnv1a(const uchar* data, size_t size) {
        uint64_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < size; i++) {
            hash ^= data[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    uint64_t ManifestSignature(const std::string& manifest) {
        return fnv1a((const uchar*)manifest.data(), manifest.size());
    }

    bool IndexMatchesManifest(const fs::path& indexPath, const std::string& manifest) {
        std::ifstream file(indexPath, std::ios::binary);
        uint32_t magic = 0, version = 0;
        uint64_t signature = 0;
        file.read((char*)&magic, 4);
        file.read((char*)&version, 4);
        file.read((char*)&signature, 8);
        return file && magic == INDEX_MAGIC && version == INDEX_VERSION && signature == ManifestSignature(manifest);
    }

    static std::vector<std::string> split(const std::string& s, char separator) {
        std::vector<std::string> out;
        std::string current;
        for (char c : s) {
            if (c == separator) {
                out.push_back(current);
                current.clear();
            }
            else if (c != '\r') {
                current.push_back(c);
            }
        }
        out.push_back(current);
        return out;
    }

    struct JobPiece {
        int mapId = 0;
        int groupId = 0;
        int floorId = 0;
    };

    // a block of map tiles, or one underground floor image
    struct Job {
        JobPiece piece;

        bool isGrid = false;
        fs::path dir;
        int blockX = 0, blockY = 0, blockW = 0, blockH = 0;
        int tile = 256;
        double originX = 0.0, originY = 0.0;

        fs::path path;
        double left = 0.0, top = 0.0;
        // HoYoLAB stretches floor images over this box, which isn't always the image size
        double right = 0.0, bottom = 0.0;
    };

    struct JobOut {
        std::vector<cv::Point2f> points;
        cv::Mat descriptors;
    };

    static void runJob(const Job& job, cv::Ptr<cv::SIFT>& sift, JobOut& out) {
        cv::Mat img;
        double unitsX = 1.0 / REF_SCALE, unitsY = 1.0 / REF_SCALE;
        double x0 = 0.0, y0 = 0.0;

        if (job.isGrid) {
            int t = (int)(job.tile * REF_SCALE);
            img = cv::Mat::zeros(job.blockH * t, job.blockW * t, CV_8UC3);
            bool any = false;
            for (int y = 0; y < job.blockH; y++) {
                for (int x = 0; x < job.blockW; x++) {
                    std::string name = std::to_string(job.blockX + x) + "_" + std::to_string(job.blockY + y) + ".webp";
                    cv::Mat tile = readImage(job.dir / name, cv::IMREAD_COLOR);
                    if (tile.empty()) continue;

                    // skip empty sea
                    cv::Scalar mean, stddev;
                    cv::meanStdDev(tile, mean, stddev);
                    if (stddev[0] + stddev[1] + stddev[2] <= 9.0) continue;

                    cv::Mat small;
                    cv::resize(tile, small, cv::Size(t, t), 0, 0, cv::INTER_AREA);
                    small.copyTo(img(cv::Rect(x * t, y * t, t, t)));
                    any = true;
                }
            }
            if (!any) return;
            x0 = job.blockX * job.tile - job.originX;
            y0 = job.blockY * job.tile - job.originY;
        }
        else {
            cv::Mat raw = readImage(job.path, cv::IMREAD_UNCHANGED);
            if (raw.empty()) return;
            if (raw.channels() == 1) cv::cvtColor(raw, raw, cv::COLOR_GRAY2BGRA);
            if (raw.channels() == 3) cv::cvtColor(raw, raw, cv::COLOR_BGR2BGRA);

            // transparent parts become black, like the game draws floors
            std::vector<cv::Mat> channels;
            cv::split(raw, channels);
            cv::Mat alpha;
            channels[3].convertTo(alpha, CV_32F, 1.0 / 255.0);
            cv::Mat rgb;
            cv::merge(std::vector<cv::Mat>{ channels[0], channels[1], channels[2] }, rgb);
            rgb.convertTo(rgb, CV_32FC3);
            cv::Mat alpha3;
            cv::merge(std::vector<cv::Mat>{ alpha, alpha, alpha }, alpha3);
            rgb = rgb.mul(alpha3);
            rgb.convertTo(img, CV_8UC3);

            double sx = job.right > job.left ? (job.right - job.left) / img.cols : 1.0;
            double sy = job.bottom > job.top ? (job.bottom - job.top) / img.rows : 1.0;
            // same detail level as the surface, but at least 256px across and never upscaled
            int longest = (std::max)(img.cols, img.rows);
            double r = (std::min)(1.0, (std::max)(REF_SCALE * sx, 256.0 / longest));
            if (r != 1.0) {
                cv::resize(img, img, cv::Size(), r, r, cv::INTER_AREA);
            }
            unitsX = sx / r;
            unitsY = sy / r;
            x0 = job.left;
            y0 = job.top;
        }

        cv::Mat gray;
        cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
        std::vector<cv::KeyPoint> keypoints;
        cv::Mat descriptors;
        sift->detectAndCompute(gray, cv::noArray(), keypoints, descriptors);
        if (keypoints.empty() || descriptors.empty()) return;

        out.points.reserve(keypoints.size());
        for (const auto& k : keypoints) {
            out.points.emplace_back((float)(k.pt.x * unitsX + x0), (float)(k.pt.y * unitsY + y0));
        }
        out.descriptors = descriptors;
    }

    static bool cancelled(const std::atomic<bool>* cancel) {
        return cancel && cancel->load();
    }

    // same spot (within 3% of the width) at about the same zoom (within 10%)
    static bool agree(const cv::Mat& a, const cv::Mat& b, int w, int h) {
        auto at = [](const cv::Mat& m, double x, double y) {
            return cv::Point2d(m.at<double>(0, 0) * x + m.at<double>(0, 1) * y + m.at<double>(0, 2),
                m.at<double>(1, 0) * x + m.at<double>(1, 1) * y + m.at<double>(1, 2));
        };
        double scaleA = std::hypot(a.at<double>(0, 0), a.at<double>(1, 0));
        double scaleB = std::hypot(b.at<double>(0, 0), b.at<double>(1, 0));
        if (std::abs(scaleA / scaleB - 1.0) > 0.10) return false;
        cv::Point2d d = at(a, w / 2.0, h / 2.0) - at(b, w / 2.0, h / 2.0);
        return std::hypot(d.x, d.y) < 0.03 * w * scaleA;
    }

    // Same KD-trees cv::FlannBasedMatcher would build (4 trees, 64 checks), but
    // straight over m_descriptors so the descriptors aren't copied again.
    struct Index::Kd {
        cvflann::Matrix<uchar> data;
        cvflann::KDTreeIndex<cvflann::L2<uchar>> index;

        explicit Kd(const cv::Mat& descriptors)
            : data(descriptors.data, (size_t)descriptors.rows, (size_t)descriptors.cols),
            index(data, cvflann::KDTreeIndexParams(4)) {
            index.buildIndex();
        }
    };

    void Index::Knn(const cv::Mat& query, std::vector<std::vector<cv::DMatch>>& out) const {
        CV_Assert(query.type() == CV_8U && query.isContinuous() && query.cols == m_descriptors.cols);
        int n = query.rows;
        cv::Mat indices(n, 2, CV_32S), dists(n, 2, CV_32F);
        cvflann::Matrix<uchar> q(query.data, (size_t)n, (size_t)query.cols);
        cvflann::Matrix<int> matrixIndices(indices.ptr<int>(), (size_t)n, 2);
        cvflann::Matrix<float> matrixDists(dists.ptr<float>(), (size_t)n, 2);
        m_kd->index.knnSearch(q, matrixIndices, matrixDists, 2, cvflann::SearchParams(64));

        // flann gives squared distances
        out.assign((size_t)n, std::vector<cv::DMatch>());
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < 2; j++) {
                int idx = indices.at<int>(i, j);
                if (idx >= 0) {
                    out[i].emplace_back(i, idx, 0, std::sqrt(dists.at<float>(i, j)));
                }
            }
        }
    }

    bool Index::Load(const fs::path& manifest, const std::atomic<bool>* cancel, int maxThreads) {
        bool ok = false;
        try {
            ok = LoadImpl(manifest, cancel, maxThreads);
        }
        catch (...) {
            ok = false;
        }
        if (!ok) {
            m_pieces.clear();
            m_points.clear();
            m_pieceOf.clear();
            m_kd.reset();
            m_descriptors.release();
        }
        m_hasLast = false;
        return ok;
    }

    bool Index::LoadImpl(const fs::path& manifest, const std::atomic<bool>* cancel, int maxThreads) {
        m_sift = cv::SIFT::create(0, 3, 0.04, 10, 1.6, CV_8U);
        m_kd.reset();

        std::vector<uchar> bytes;
        if (!readFileBytes(manifest, bytes)) return false;
        uint64_t signature = fnv1a(bytes.data(), bytes.size());
        fs::path cache = manifest.parent_path() / "index.bin";
        if (!ReadCache(cache, signature) && !Build(manifest, bytes, cache, signature, cancel, maxThreads)) {
            return false;
        }
        if (cancelled(cancel)) return false;

        m_kd = std::make_shared<Kd>(m_descriptors);
        return true;
    }

    // Manifest is tab separated, paths relative to its folder:
    //   grid  <map> <cols> <rows> <tile> <originX> <originY> <dir>
    //   image <map> <group> <floor> <left> <top> <path> [<right> <bottom>]
    bool Index::Build(const fs::path& manifest, const std::vector<uchar>& bytes, const fs::path& cachePath,
        uint64_t signature, const std::atomic<bool>* cancel, int maxThreads) {
        fs::path base = manifest.parent_path();
        std::vector<Job> jobs;
        std::vector<int> jobPiece;
        std::vector<Piece> newPieces;
        std::map<std::tuple<int, int, int>, int> pieceIndex;

        auto pieceId = [&](const JobPiece& p) {
            auto key = std::make_tuple(p.mapId, p.groupId, p.floorId);
            auto it = pieceIndex.find(key);
            if (it != pieceIndex.end()) return it->second;
            int id = (int)newPieces.size();
            newPieces.push_back(Piece{ p.mapId, p.groupId, p.floorId });
            pieceIndex[key] = id;
            return id;
        };

        std::string text(bytes.begin(), bytes.end());
        for (const std::string& line : split(text, '\n')) {
            auto f = split(line, '\t');
            if (f.empty() || f[0].empty() || f[0][0] == '#') continue;

            if (f[0] == "grid" && f.size() >= 8) {
                JobPiece p{ std::atoi(f[1].c_str()), 0, 0 };
                int cols = std::atoi(f[2].c_str());
                int rows = std::atoi(f[3].c_str());
                int tile = std::atoi(f[4].c_str());
                double originX = std::atof(f[5].c_str());
                double originY = std::atof(f[6].c_str());
                fs::path dir = base / fromUtf8(f[7]);
                int pid = pieceId(p);

                // 16x16 tile blocks, big enough for SIFT to see across tile seams
                for (int by = 0; by < rows; by += 16) {
                    for (int bx = 0; bx < cols; bx += 16) {
                        Job job;
                        job.piece = p;
                        job.isGrid = true;
                        job.dir = dir;
                        job.blockX = bx;
                        job.blockY = by;
                        job.blockW = (std::min)(16, cols - bx);
                        job.blockH = (std::min)(16, rows - by);
                        job.tile = tile > 0 ? tile : 256;
                        job.originX = originX;
                        job.originY = originY;
                        jobs.push_back(job);
                        jobPiece.push_back(pid);
                    }
                }
            }
            else if (f[0] == "image" && f.size() >= 7) {
                Job job;
                job.piece = JobPiece{ std::atoi(f[1].c_str()), std::atoi(f[2].c_str()), std::atoi(f[3].c_str()) };
                job.left = std::atof(f[4].c_str());
                job.top = std::atof(f[5].c_str());
                job.path = base / fromUtf8(f[6]);
                if (f.size() >= 9) {
                    job.right = std::atof(f[7].c_str());
                    job.bottom = std::atof(f[8].c_str());
                }
                jobs.push_back(job);
                jobPiece.push_back(pieceId(job.piece));
            }
        }
        if (jobs.empty()) return false;

        std::vector<JobOut> outs(jobs.size());
        std::atomic<size_t> next{ 0 };
        unsigned hardware = std::thread::hardware_concurrency();
        unsigned cap = (unsigned)(std::max)(1, maxThreads);
        unsigned threadCount = (std::max)(1u, (std::min)(hardware > 2 ? hardware - 1 : 1u, cap));

        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threadCount; t++) {
            pool.emplace_back([&]() {
                cv::Ptr<cv::SIFT> sift = cv::SIFT::create(0, 3, 0.04, 10, 1.6, CV_8U);
                for (;;) {
                    size_t i = next.fetch_add(1);
                    if (i >= jobs.size() || cancelled(cancel)) break;
                    try {
                        runJob(jobs[i], sift, outs[i]);
                    }
                    catch (...) {
                        outs[i] = JobOut();
                    }
                }
            });
        }
        for (auto& thread : pool) thread.join();
        if (cancelled(cancel)) return false;

        size_t total = 0;
        for (const auto& o : outs) total += o.points.size();
        if (total == 0) return false;

        m_points.clear();
        m_pieceOf.clear();
        m_points.reserve(total);
        m_pieceOf.reserve(total);
        cv::Mat descriptors((int)total, 128, CV_8U);
        int row = 0;
        for (size_t i = 0; i < outs.size(); i++) {
            const auto& o = outs[i];
            if (o.points.empty()) continue;
            o.descriptors.copyTo(descriptors.rowRange(row, row + o.descriptors.rows));
            row += o.descriptors.rows;
            m_points.insert(m_points.end(), o.points.begin(), o.points.end());
            m_pieceOf.insert(m_pieceOf.end(), o.points.size(), jobPiece[i]);
        }
        m_pieces = newPieces;
        m_descriptors = descriptors;
        WriteCache(cachePath, signature);
        return true;
    }

    bool Index::ReadCache(const fs::path& cachePath, uint64_t signature) {
        std::ifstream file(cachePath, std::ios::binary);
        if (!file) return false;

        uint32_t magic = 0, version = 0;
        uint64_t fileSignature = 0;
        uint32_t pieceCount = 0, n = 0;
        file.read((char*)&magic, 4);
        file.read((char*)&version, 4);
        file.read((char*)&fileSignature, 8);
        file.read((char*)&pieceCount, 4);
        file.read((char*)&n, 4);
        if (!file || magic != INDEX_MAGIC || version != INDEX_VERSION || fileSignature != signature || n == 0 || pieceCount == 0) {
            return false;
        }

        std::vector<Piece> pieces(pieceCount);
        file.read((char*)pieces.data(), sizeof(Piece) * pieceCount);
        std::vector<cv::Point2f> points(n);
        file.read((char*)points.data(), sizeof(cv::Point2f) * n);
        std::vector<int> pieceOf(n);
        file.read((char*)pieceOf.data(), sizeof(int) * n);
        cv::Mat descriptors((int)n, 128, CV_8U);
        file.read((char*)descriptors.data, (std::streamsize)n * 128);
        if (!file) return false;

        m_pieces = std::move(pieces);
        m_points = std::move(points);
        m_pieceOf = std::move(pieceOf);
        m_descriptors = descriptors;
        return true;
    }

    void Index::WriteCache(const fs::path& cachePath, uint64_t signature) {
        fs::path tmp = cachePath;
        tmp += ".tmp";
        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            if (!file) return;
            uint32_t pieceCount = (uint32_t)m_pieces.size();
            uint32_t n = (uint32_t)m_points.size();
            file.write((const char*)&INDEX_MAGIC, 4);
            file.write((const char*)&INDEX_VERSION, 4);
            file.write((const char*)&signature, 8);
            file.write((const char*)&pieceCount, 4);
            file.write((const char*)&n, 4);
            file.write((const char*)m_pieces.data(), sizeof(Piece) * pieceCount);
            file.write((const char*)m_points.data(), sizeof(cv::Point2f) * n);
            file.write((const char*)m_pieceOf.data(), sizeof(int) * n);
            file.write((const char*)m_descriptors.data, (std::streamsize)n * 128);
            if (!file) return;
        }
        std::error_code ec;
        fs::rename(tmp, cachePath, ec);
        if (ec) fs::remove(tmp, ec);
    }

    // Groups matches by piece and fits a similarity transform (screen px to map
    // units) for the best few. Keeps the best surface fit and best floor fit.
    // subset maps local train indices back to global ones for the local fallback.
    void Index::TryMatch(const std::vector<cv::Point2f>& queryPoints, const std::vector<std::vector<cv::DMatch>>& knn,
        const std::vector<int>* subset, Fit& surface, Fit& floor) {
        std::map<int, std::vector<std::pair<int, int>>> byPiece;
        for (const auto& m : knn) {
            if (m.size() < 2 || m[0].distance >= RATIO * m[1].distance) continue;
            int train = subset ? (*subset)[m[0].trainIdx] : m[0].trainIdx;
            byPiece[m_pieceOf[train]].emplace_back(m[0].queryIdx, train);
        }

        std::vector<std::pair<int, int>> order;
        for (const auto& entry : byPiece) {
            order.emplace_back((int)entry.second.size(), entry.first);
        }
        std::sort(order.rbegin(), order.rend());

        for (size_t i = 0; i < order.size() && i < PIECES_TO_VERIFY; i++) {
            if (order[i].first < 6) break;

            const auto& matches = byPiece[order[i].second];
            std::vector<cv::Point2f> src, dst;
            src.reserve(matches.size());
            dst.reserve(matches.size());
            for (const auto& m : matches) {
                src.push_back(queryPoints[m.first]);
                dst.push_back(m_points[m.second]);
            }

            cv::Mat inliers;
            cv::Mat transform = cv::estimateAffinePartial2D(src, dst, inliers, cv::RANSAC, RANSAC_THRESHOLD, 2000, 0.99, 10);
            if (transform.empty()) continue;

            // the game map is never rotated
            double c = transform.at<double>(0, 0), s = transform.at<double>(1, 0);
            double scale = std::hypot(c, s);
            double rotation = std::atan2(s, c) * 180.0 / CV_PI;
            if (std::abs(rotation) > MAX_ROTATION_DEG || scale < 0.02 || scale > 20.0) continue;

            int count = cv::countNonZero(inliers);
            Fit& target = m_pieces[order[i].second].groupId == 0 ? surface : floor;
            if (count > target.inliers) {
                target.inliers = count;
                target.piece = order[i].second;
                target.transform = transform;
            }
        }
    }

    Result Index::Match(const cv::Mat& bgr, double cx, double cy, bool fast, bool searchNearLast) {
        if (!IsReady()) return Result();
        try {
            return MatchImpl(bgr, cx, cy, fast, searchNearLast);
        }
        catch (...) {
            return Result();
        }
    }

    Result Index::MatchImpl(const cv::Mat& screen, double cx, double cy, bool fast, bool searchNearLast) {
        int w = screen.cols, h = screen.rows;
        if (w < 64 || h < 64) return Result();

        // the game UI scales with the screen height
        if (m_maskWidth != w || m_maskHeight != h) {
            m_matchMask = cv::Mat(h, w, CV_8U, cv::Scalar(255));

            double s = h / 1125.0;

            for (const auto& r : UI_RECTS) {
                int x0, x1;

                if (r.right) {
                    x0 = (int)(w - (2000 - r.x0) * s);
                    x1 = (int)(w - (2000 - r.x1) * s);
                }
                else {
                    x0 = (int)(r.x0 * s);
                    x1 = (int)(r.x1 * s);
                }

                int y0 = (int)(r.y0 * s);
                int y1 = (int)(r.y1 * s);

                cv::Rect rect(
                    cv::Point((std::max)(0, x0), (std::max)(0, y0)),
                    cv::Point((std::min)(w, x1), (std::min)(h, y1))
                );

                if (rect.area() > 0)
                    m_matchMask(rect).setTo(0);
            }

            m_maskWidth = w;
            m_maskHeight = h;
        }

        const cv::Mat& mask = m_matchMask;

        cv::Mat grayFull;
        cv::cvtColor(screen, grayFull, cv::COLOR_BGR2GRAY);

        struct Query {
            std::vector<cv::Point2f> points;
            cv::Mat descriptors;
        };

        auto features = [&](const cv::Mat& graySource, int queryWidth, Query& out) {
            double q = (std::min)(1.0, (double)queryWidth / w);
            cv::Mat gray, m;
            cv::resize(graySource, gray, cv::Size(), q, q, cv::INTER_AREA);
            cv::resize(mask, m, gray.size(), 0, 0, cv::INTER_NEAREST);
            std::vector<cv::KeyPoint> keypoints;
            m_sift->detectAndCompute(gray, m, keypoints, out.descriptors);
            if (keypoints.size() < 8 || out.descriptors.empty()) return false;

            out.points.reserve(keypoints.size());
            for (const auto& k : keypoints) {
                out.points.emplace_back((float)(k.pt.x / q), (float)(k.pt.y / q));
            }
            return true;
        };

        Fit surface, floor;
        auto strongest = [&]() { return (std::max)(surface.inliers, floor.inliers); };

        auto pass = [&](const cv::Mat& graySource, std::vector<Query>* keep) {
            const int* widths = fast ? (smallFirst ? SMALL_FIRST_WIDTHS : FAST_QUERY_WIDTHS) : QUERY_WIDTHS;
            size_t count = fast ? std::size(FAST_QUERY_WIDTHS) : std::size(QUERY_WIDTHS);
            for (size_t i = 0; i < count; i++) {
                Query query;
                if (features(graySource, widths[i], query)) {
                    std::vector<std::vector<cv::DMatch>> knn;
                    Knn(query.descriptors, knn);
                    TryMatch(query.points, knn, nullptr, surface, floor);
                    if (keep) keep->push_back(std::move(query));
                }
                if (strongest() >= STRONG_INLIERS || widths[i] >= w) break;
            }
        };

        pass(grayFull, nullptr);

        // Boosted contrast pass. Helps a lot on plain sand zoomed all the way in
        // and on underground views, where the game dims everything around the floor.
        std::vector<Query> contrast;
        if (strongest() < STRONG_INLIERS) {
            cv::Mat boosted;
            clahe->apply(grayFull, boosted);
            pass(boosted, &contrast);
        }

        // still nothing: try only the keypoints near the last fix
        if (strongest() < MIN_INLIERS && m_hasLast && searchNearLast) {
            std::vector<int> subset;
            for (int i = 0; i < (int)m_points.size(); i++) {
                if (m_pieces[m_pieceOf[i]].mapId != m_last.mapId) continue;
                if (std::abs(m_points[i].x - m_last.lng) > LOCAL_RADIUS || std::abs(m_points[i].y - m_last.lat) > LOCAL_RADIUS) {
                    continue;
                }
                subset.push_back(i);
            }
            if (subset.size() >= 50) {
                cv::Mat sub((int)subset.size(), m_descriptors.cols, m_descriptors.type());
                for (int r = 0; r < (int)subset.size(); r++) {
                    m_descriptors.row(subset[r]).copyTo(sub.row(r));
                }
                cv::BFMatcher matcher(cv::NORM_L2);
                for (const auto& query : contrast) {
                    std::vector<std::vector<cv::DMatch>> knn;
                    matcher.knnMatch(query.descriptors, sub, knn, 2);
                    TryMatch(query.points, knn, &subset, surface, floor);
                }
            }
        }

        // The floor fit says which floor. The position comes from whichever fit is
        // stronger when they agree, usually the dimmed surface around the floor.
        const Fit* place = nullptr;
        const Fit* which = nullptr;
        bool surfaceOk = surface.inliers >= MIN_INLIERS;
        bool floorOk = floor.inliers >= MIN_INLIERS;
        if (floorOk && floor.inliers >= surface.inliers) {
            which = &floor;
            place = surfaceOk && agree(surface.transform, floor.transform, w, h) ? &surface : &floor;
            if (place == &surface && surface.inliers < floor.inliers) place = &floor;
        }
        else if (floorOk && surfaceOk && m_pieces[floor.piece].mapId == m_pieces[surface.piece].mapId
            && agree(surface.transform, floor.transform, w, h)) {
            // small floors give few features, but a weak fit that lands on the
            // same spot as the surface is still trustworthy
            which = &floor;
            place = &surface;
        }
        else if (surfaceOk) {
            which = &surface;
            place = &surface;
        }

        Result best;
        best.inliers = strongest();
        if (!which) return best;

        const Piece& piece = m_pieces[which->piece];
        const cv::Mat& a = place->transform;
        best.found = true;
        best.inliers = place->inliers;
        best.mapId = piece.mapId;
        best.groupId = piece.groupId;
        best.floorId = piece.floorId;
        best.unitsPerPixel = std::hypot(a.at<double>(0, 0), a.at<double>(1, 0));
        best.lng = a.at<double>(0, 0) * cx + a.at<double>(0, 1) * cy + a.at<double>(0, 2);
        best.lat = a.at<double>(1, 0) * cx + a.at<double>(1, 1) * cy + a.at<double>(1, 2);
        m_hasLast = true;
        m_last = best;
        return best;
    }

}
