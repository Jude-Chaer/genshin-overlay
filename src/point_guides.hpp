#pragma once
#include <cstdint>
#include <string>
#include <vector>

// What a map point says about itself: HoYoLAB's guide text and picture, and
// the comments people left on the same point on appsample's map (paired in
// items/appsample.json). Fetched in the background, pictures are URLs for
// WebImages.

namespace PointGuides {
    struct Comment {
        std::string author;
        std::string date;
        std::string text;
        std::string picture;
        int likes = 0;
    };

    struct Guide {
        std::string text;
        std::string picture;
        std::vector<Comment> comments;
        // nothing came back from either site
        bool failed = false;
    };

    extern void Init();
    extern void Shutdown();

    // false while it's loading, the first call starts it
    extern bool Get(int mapId, int64_t pointId, int typeId, Guide& out);
}
