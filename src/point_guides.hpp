#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "net.hpp"

// What a map point says about itself, for GetPointGuide: HoYoLAB's guide text
// and picture, and what people wrote on the same point on appsample's map
// (paired in the item files), with appsample's one line summary of it.
// Pictures are links for WebImages.
//
// Fetched on one thread when a guide is first asked, kept in memory only:
// the last Kept asked, a failed part asked again after RetryAfter, all of it
// after FreshFor (the old one is given meanwhile).
//
// A comment in another language than the overlay's is shown in appsample's
// own translation when it has one, otherwise by Google's web translate (all
// of a guide's in one request, Google says which are in another language),
// then MyMemory one by one by appsample's tag, and as it was written when
// both fail.

namespace PointGuides {
    inline const size_t Kept = 64;
    inline const auto RetryAfter = std::chrono::seconds(10);
    inline const auto FreshFor = std::chrono::minutes(10);
    // comments asked from appsample, the most liked first
    inline const int MaxComments = 30;
    // longer comments stay as they were written
    inline const size_t MaxTranslated = 2000;
    // a Google request's comments at most this long together, the rest go in the next
    inline const size_t MaxPerRequest = 4000;

    struct Comment {
        std::string author;
        std::string date;      // 2022-02-04
        std::string text;      // in the overlay's language when it could be
        std::string original;  // as it was written
        std::string language;  // the code appsample gave ("es"), empty when it gave none
        std::string from;      // the language it was translated from, empty when it wasn't
        bool translating = false;
        std::string picture;
        int likes = 0;
    };

    struct Guide {
        std::string text;
        std::string picture;
        std::string summary;  // appsample's, empty when it has none
        std::vector<Comment> comments;
        // nothing came back from either site
        bool failed = false;
    };

    // One point to ask about, from the item files
    struct Point {
        int64_t id = 0;
        int map = 0;
        int type = 0;
        int32_t appsample = 0;  // 0 when appsample doesn't have it
    };

    // The guide for point in language ("en"). False until the first answer
    // is in; the first call starts it.
    bool get(const Point& point, const std::string& language, Guide& out);
    // stops the thread and drops every guide, at shutdown
    void stop();

    // the hosts a guide's requests go to
    const std::vector<std::string>& sites();

    // the pieces, for the tests
    size_t queued();
    std::string infoUrl(const Point& point, const std::string& language);
    std::string commentsUrl(const Point& point);
    std::string googleUrl(const std::vector<std::string>& texts, const std::string& language);
    std::string myMemoryUrl(const std::string& text, const std::string& from, const std::string& language);
    // HoYoLAB's answer, false when it isn't one (a point it doesn't have is an answer, with no text)
    bool readInfo(const std::string& body, Guide& out);
    // appsample's answer into out's comments and summary, most liked first.
    // Comments in language take their text as it is, the others appsample's
    // translation into it when there is one, or are left translating.
    bool readComments(const std::string& body, const std::string& language, Guide& out);
    // Google's answer for count texts: each one's text and the language it says it was in
    struct Translated {
        std::string text;
        std::string from;
    };
    bool readGoogle(const std::string& body, size_t count, std::vector<Translated>& out);
    bool readMyMemory(const std::string& body, std::string& text);
    // English name of a language code ("es" is Spanish), the code when it doesn't know it
    std::string languageName(const std::string& code);

    // how a request is made (hoyolab adds HoYoLAB's headers) and the time,
    // the tests set both
    extern std::function<bool(const std::string& url, bool hoyolab, Net::Response& out, Net::Stopper* stopper)> fetch;
    extern std::function<std::chrono::steady_clock::time_point()> now;
}
