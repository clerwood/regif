#include "TestFramework.h"

#include <regif/EditorGeometry.h>

#include <cmath>

using namespace regif;

namespace {

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}

bool same(const CropRect& a, const CropRect& b)
{
    return near(a.x, b.x) && near(a.y, b.y) && near(a.width, b.width) && near(a.height, b.height);
}

const CropRect kRect{ 100, 50, 200, 100 }; // edges: left 100, top 50, right 300, bottom 150

} // namespace

TEST_CASE("crop hit testing finds corners, edges and the inside")
{
    CHECK(hitTestCrop(kRect, 101, 52, 8) == CropHandle::TopLeft);
    CHECK(hitTestCrop(kRect, 305, 146, 8) == CropHandle::BottomRight);
    CHECK(hitTestCrop(kRect, 96, 100, 8) == CropHandle::Left);
    CHECK(hitTestCrop(kRect, 200, 155, 8) == CropHandle::Bottom);
    CHECK(hitTestCrop(kRect, 200, 100, 8) == CropHandle::Move);
    CHECK(hitTestCrop(kRect, 50, 100, 8) == CropHandle::None);
    CHECK(hitTestCrop(kRect, 200, 170, 8) == CropHandle::None);

    // In a rectangle narrower than two grips, the nearer edge wins.
    const CropRect thin{ 100, 50, 10, 100 };
    CHECK(hitTestCrop(thin, 102, 100, 8) == CropHandle::Left);
    CHECK(hitTestCrop(thin, 109, 100, 8) == CropHandle::Right);
}

TEST_CASE("free crop drags move edges and stay inside the frame")
{
    CHECK(same(dragCrop(kRect, CropHandle::Right, 40, 999, 640, 360, 0, 16), CropRect{ 100, 50, 240, 100 }));
    CHECK(same(dragCrop(kRect, CropHandle::TopLeft, -30, -20, 640, 360, 0, 16), CropRect{ 70, 30, 230, 120 }));

    // Past the frame edge: clamped.
    CHECK(same(dragCrop(kRect, CropHandle::Left, -500, 0, 640, 360, 0, 16), CropRect{ 0, 50, 300, 100 }));
    // Past the opposite edge: stops at the minimum size.
    CHECK(same(dragCrop(kRect, CropHandle::Bottom, 0, -500, 640, 360, 0, 16), CropRect{ 100, 50, 200, 16 }));

    // Moving keeps the size and stops at the frame edge.
    CHECK(same(dragCrop(kRect, CropHandle::Move, 1000, -1000, 640, 360, 0, 16), CropRect{ 440, 0, 200, 100 }));
    CHECK(same(dragCrop(kRect, CropHandle::None, 10, 10, 640, 360, 0, 16), kRect));
}

TEST_CASE("aspect-locked crop drags keep the ratio")
{
    // Corner: the opposite corner stays put and the larger movement decides the size.
    const CropRect corner = dragCrop(kRect, CropHandle::BottomRight, 100, 10, 640, 360, 2.0, 16);
    CHECK(same(corner, CropRect{ 100, 50, 300, 150 }));

    // Limited by the frame: 540 px of room to the right (270 high, which fits in the 310 below).
    const CropRect big = dragCrop(kRect, CropHandle::BottomRight, 5000, 5000, 640, 360, 2.0, 16);
    CHECK(same(big, CropRect{ 100, 50, 540, 270 }));
    const CropRect tall = dragCrop(kRect, CropHandle::BottomRight, 0, 5000, 640, 200, 2.0, 16);
    CHECK(same(tall, CropRect{ 100, 50, 300, 150 })); // 150 px of room below the top edge

    // Edge: the other axis grows around its centre (y centre 100).
    const CropRect edge = dragCrop(kRect, CropHandle::Right, 100, 0, 640, 360, 2.0, 16);
    CHECK(same(edge, CropRect{ 100, 25, 300, 150 }));

    // Edge near the frame border: shifted to stay inside rather than overflowing.
    const CropRect shifted = dragCrop(CropRect{ 0, 0, 100, 50 }, CropHandle::Right, 100, 0, 640, 360, 2.0, 16);
    CHECK(same(shifted, CropRect{ 0, 0, 200, 100 }));

    // Shrinking past the anchor stops at the minimum, which applies to the shorter side.
    const CropRect tiny = dragCrop(kRect, CropHandle::TopLeft, 1000, 1000, 640, 360, 2.0, 16);
    CHECK(same(tiny, CropRect{ 268, 134, 32, 16 }));
}

TEST_CASE("fitting an aspect ratio inside the current crop")
{
    CHECK(same(fitAspect(CropRect{ 0, 0, 1920, 1080 }, 1.0), CropRect{ 420, 0, 1080, 1080 }));
    CHECK(same(fitAspect(CropRect{ 0, 0, 1920, 1080 }, 9.0 / 16.0), CropRect{ 656.25, 0, 607.5, 1080 }));
    CHECK(same(fitAspect(CropRect{ 10, 10, 400, 400 }, 2.0), CropRect{ 10, 110, 400, 200 }));
    CHECK(same(fitAspect(kRect, 0.0), kRect));
}

TEST_CASE("crop rectangles round to whole pixels inside the frame")
{
    const CropOp op = toCropOp(CropRect{ 10.4, 20.6, 100.2, 50.3 }, 640, 360);
    CHECK_EQ(op.x, 10);
    CHECK_EQ(op.y, 21);
    CHECK_EQ(op.width, 101); // right edge 110.6 -> 111
    CHECK_EQ(op.height, 50); // bottom edge 70.9 -> 71

    const CropOp edge = toCropOp(CropRect{ 539.6, 0, 100.4, 360 }, 640, 360);
    CHECK_EQ(edge.x + edge.width, 640);
    CHECK_EQ(edge.height, 360);

    const CropOp outside = toCropOp(CropRect{ -5, -5, 2000, 2000 }, 640, 360);
    CHECK_EQ(outside.x, 0);
    CHECK_EQ(outside.width, 640);
    CHECK_EQ(outside.height, 360);
}

TEST_CASE("thumbnail times sit in the middle of equal slots")
{
    const auto times = thumbnailTimes(10.0, 4);
    CHECK_EQ(times.size(), std::size_t(4));
    CHECK(near(times.front(), 1.25));
    CHECK(near(times.back(), 8.75));
    CHECK(thumbnailTimes(0.0, 4).empty());
    CHECK(thumbnailTimes(10.0, 0).empty());
}
