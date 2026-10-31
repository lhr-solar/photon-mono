#include <limits>
#include "test_harness.hpp"
#include "ui/chart_grid_model.hpp"
#include "ui/video_release.hpp"
#include "ui/video_display.hpp"

void runChartGridTests() {
    using namespace photon::ui;
    CustomView v;
    v.columns = 12;
    v.rowHeight = 160;
    ViewCell c;
    c.kind = ViewCellKind::Line;
    c.rect = {6, 2, 6, 2};
    v.cells.push_back(c);
    migrateGridUnits(v);
    CHECK(v.columns == 24);
    CHECK(near(v.rowHeight, 24));
    CHECK(v.cells[0].rect.x == 12);
    CHECK(v.cells[0].rect.w == 12);
    CHECK(v.cells[0].rect.y == 13);
    CHECK(v.cells[0].rect.h == 13);
    auto once = v.cells[0].rect;
    migrateGridUnits(v);
    CHECK(sameGridRect(once, v.cells[0].rect));
    auto bounds = boundedGridRect({-2, -4, 99, 0}, 8, 13);
    CHECK(bounds.x == 0);
    CHECK(bounds.y == 0);
    CHECK(bounds.w == 24);
    CHECK(bounds.h == 13);
    std::vector<GridEntry> original{{"a", {0, 0, 12, 14}}, {"b", {12, 0, 12, 14}}, {"c", {12, 14, 12, 14}}};
    auto moved = reflowGrid(original, 0, {12, 0, 12, 14});
    CHECK(moved[1].rect.y == 14);
    CHECK(moved[2].rect.y == 28);
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j) CHECK(!gridOverlap(moved[i].rect, moved[j].rect));
    auto reverse = reflowGrid(original, 0, original[0].rect);
    for (int i = 0; i < 3; ++i) CHECK(sameGridRect(original[i].rect, reverse[i].rect));
    // Moving the right-hand chart left trims its left neighbor in place.
    auto shared = reflowGrid(original, 1, {9, 0, 12, 14});
    CHECK(shared[0].rect.x == 0 && shared[0].rect.w == 9 && shared[0].rect.y == 0);
    CHECK(shared[1].rect.x == 9 && shared[1].rect.y == 0);
    CHECK(!gridOverlap(shared[0].rect, shared[1].rect));
    // Expanding the left chart trims the right neighbor from its left edge.
    shared = reflowGrid(original, 0, {0, 0, 15, 14});
    CHECK(shared[1].rect.x == 15 && shared[1].rect.w == 9 && shared[1].rect.y == 0);
    auto limited = original;
    limited[0].minW = 10;
    shared = reflowGrid(limited, 1, {9, 0, 12, 14});
    CHECK(shared[0].rect.w == 12 && shared[0].rect.y >= 14);
    std::vector<GridEntry> values{{"value", {0, 0, 4, 4}, false, 2}, {"plot", {4, 0, 12, 14}}};
    shared = reflowGrid(values, 0, {0, 14, 4, 4});
    CHECK(shared[0].rect.y == 14 && shared[0].rect.h == 4 && shared[0].rect.w == 4);
    original.push_back({"value", {0, 30, 24, 4}, true});
    moved = reflowGrid(original, 0, {0, 30, 12, 14});
    CHECK(moved[0].rect.y == 34);
    CHECK(sameGridRect(original[3].rect, moved[3].rect));
    CHECK(sameGridRect(reflowGrid(original, 3, {5, 0, 1, 1})[3].rect, original[3].rect));
    ExploringValueRange left, right;
    left.include(-10, 10);
    right.include(100, 200);
    left.include(-1, 1);
    CHECK(near(left.lo, -10));
    CHECK(near(left.hi, 10));
    left.include(-20, 30);
    CHECK(near(left.lo, -20));
    CHECK(near(left.hi, 30));
    CHECK(near(right.lo, 100));
    CHECK(near(right.hi, 200));
    left.include(std::numeric_limits<double>::quiet_NaN(), 999);
    left.include(5, 5);
    CHECK(near(left.lo, -20));
    CHECK(near(left.hi, 30));
    CHECK(liveVideoRecentlyActive(false, true, true, 10000, 11000));
    CHECK(!liveVideoRecentlyActive(false, true, true, 10000, 13000));
    CHECK(!liveVideoRecentlyActive(false, false, true, 10000, 11000));
    CHECK(!liveVideoRecentlyActive(false, true, false, 10000, 11000));
    CHECK(!liveVideoRecentlyActive(false, true, true, 12000, 11000));
    CHECK(liveVideoRecentlyActive(true, true, true, 0, 11000));
    // A status reply or lookup completion cannot announce playback without a picture.
    CHECK(videoDisplayState(true, false, true, 0, 10) == VideoDisplayState::Buffering);
    CHECK(videoDisplayState(true, false, true, 0, 11) == VideoDisplayState::Buffering);
    CHECK(videoDisplayState(true, true, true, 10, 11.9) == VideoDisplayState::Ready);
    CHECK(videoDisplayState(true, true, true, 10, 12) == VideoDisplayState::Stalled);
    CHECK(videoDisplayState(true, true, true, 12, 12.1) == VideoDisplayState::Ready);
    CHECK(videoDisplayState(true, true, false, 10, 100) == VideoDisplayState::Ready);
    CHECK(videoDisplayState(false, false, true, 0, 100) == VideoDisplayState::Idle);

}
