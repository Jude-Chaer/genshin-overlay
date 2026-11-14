#pragma once
#include <string>

// The language the overlay shows things in, as a code like "en". Only English
// is in. Guides and their comments are asked in it and translated to it.

namespace Language {
    inline std::string current() { return "en"; }
}
