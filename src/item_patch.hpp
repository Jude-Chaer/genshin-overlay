#pragma once
#include <string>

// The steps that turn one raw item pack into the next, so an overlay that has
// the older pack downloads only what changed. A patch is
//   items patch 1\n
// then steps, one after the other:
//   c <offset> <length>\n          copy that many bytes of the old pack
//   a <length>\n<bytes>            add these bytes
// Applying it knows nothing of the files inside, the caller checks the
// result's sha256.

namespace ItemPatch {
    // from the old raw pack to the new one. Lines of the new pack found in the
    // old one are copied, the rest is added, so a pack with one point per line
    // gives one step per changed point. A file's header is a line of its own.
    extern std::string make(const std::string& from, const std::string& to);

    // the new pack into out. False for a patch that isn't whole, copies from
    // past the end of from, or comes to more than maxSize.
    extern bool apply(const std::string& from, const std::string& patch, size_t maxSize, std::string& out);
}
