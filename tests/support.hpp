#pragma once
// Shared setup: every simulation test needs the real content tree, loaded once.

#include <filesystem>

#include "game/content/content_db.hpp"

namespace testing {

// Walks up from the working directory until it finds content/, so the tests run
// from a build directory as well as from the source root.
inline std::filesystem::path contentRoot() {
    std::filesystem::path p = "content";
    for (int i = 0; i < 5 && !std::filesystem::exists(p); ++i) p = ".." / p;
    return p;
}

inline const content::ContentDb& sharedContent() {
    static content::ContentDb db = [] {
        content::ContentDb d;
        d.load(contentRoot());
        return d;
    }();
    return db;
}

} // namespace testing
