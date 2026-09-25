#include "framework.hpp"

#include "engine/render/geometry/page_residency.hpp"

TEST(page_residency_pins_roots_and_falls_back_to_them) {
    engine::GeometryPageResidency residency;
    const engine::GeometryPageDesc pages[] = {
        {0, engine::GeometryPageDesc::kNoParent, 4, true},
        {1, 0, 8, false}, {2, 1, 8, false},
    };
    CHECK(residency.configure(pages, 20));
    CHECK(residency.pinRoot(0));
    CHECK(residency.request(2));
    CHECK_EQ(residency.fallback(2), 0u);
    CHECK(residency.commit(1));
    CHECK_EQ(residency.fallback(2), 1u);
    CHECK(residency.commit(2));
    CHECK_EQ(residency.fallback(2), 2u);
}

TEST(page_residency_requests_are_idempotent_and_ordered) {
    engine::GeometryPageResidency residency;
    const engine::GeometryPageDesc pages[] = {
        {0, engine::GeometryPageDesc::kNoParent, 1, true},
        {1, 0, 1, false}, {2, 0, 1, false},
    };
    CHECK(residency.configure(pages, 3));
    CHECK(residency.request(2));
    CHECK(residency.request(1));
    CHECK(residency.request(2));
    CHECK_EQ(residency.requests().size(), std::size_t(2));
    CHECK_EQ(residency.requests()[0], 2u);
    CHECK_EQ(residency.requests()[1], 1u);
    CHECK(residency.commit(2));
    CHECK(residency.request(2));
    CHECK_EQ(residency.requests().size(), std::size_t(2));
}

TEST(page_residency_commit_evicts_oldest_unpinned_page) {
    engine::GeometryPageResidency residency;
    const engine::GeometryPageDesc pages[] = {
        {0, engine::GeometryPageDesc::kNoParent, 4, true},
        {1, 0, 4, false}, {2, 0, 4, false}, {3, 0, 4, false},
    };
    CHECK(residency.configure(pages, 12));
    CHECK(residency.pinRoot(0));
    residency.beginFrame(1); CHECK(residency.commit(1));
    residency.beginFrame(2); CHECK(residency.commit(2));
    residency.beginFrame(3); CHECK(residency.request(1));
    residency.beginFrame(4); CHECK(residency.commit(3));
    CHECK_EQ(residency.residentBytes(), std::size_t(12));
    CHECK(residency.resident(0));
    CHECK(residency.resident(1));
    CHECK(residency.resident(3));
    CHECK(!residency.resident(2));
}

TEST(page_residency_rejects_unknown_parent_duplicate_and_cycle) {
    engine::GeometryPageResidency residency;
    const engine::GeometryPageDesc unknown[] = {{1, 99, 1, false}};
    CHECK(!residency.configure(unknown, 4));
    const engine::GeometryPageDesc duplicate[] = {
        {1, engine::GeometryPageDesc::kNoParent, 1, true},
        {1, engine::GeometryPageDesc::kNoParent, 1, true},
    };
    CHECK(!residency.configure(duplicate, 4));
    const engine::GeometryPageDesc cycle[] = {{1, 2, 1, false}, {2, 1, 1, false}};
    CHECK(!residency.configure(cycle, 4));
}
