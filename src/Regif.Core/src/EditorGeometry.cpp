#include <regif/EditorGeometry.h>

#include <algorithm>
#include <cmath>

namespace regif {

namespace {

// std::clamp with the bounds sorted, so an impossible range (a frame smaller than the
// minimum size) degrades to the upper bound instead of undefined behaviour.
double clampTo(double value, double lo, double hi)
{
    return std::clamp(value, std::min(lo, hi), hi);
}

int roundToInt(double value)
{
    return static_cast<int>(std::lround(value));
}

} // namespace

CropHandle hitTestCrop(const CropRect& crop, double px, double py, double grip)
{
    const double l = crop.x, t = crop.y, r = crop.x + crop.width, b = crop.y + crop.height;
    if (px < l - grip || px > r + grip || py < t - grip || py > b + grip) return CropHandle::None;

    // Pick the nearer edge on each axis, so small rectangles stay resizable from both sides.
    const double dl = std::abs(px - l), dr = std::abs(px - r), dt = std::abs(py - t), db = std::abs(py - b);
    const int horizontal = std::min(dl, dr) <= grip ? (dl <= dr ? -1 : 1) : 0;
    const int vertical = std::min(dt, db) <= grip ? (dt <= db ? -1 : 1) : 0;

    if (horizontal < 0 && vertical < 0) return CropHandle::TopLeft;
    if (horizontal > 0 && vertical < 0) return CropHandle::TopRight;
    if (horizontal < 0 && vertical > 0) return CropHandle::BottomLeft;
    if (horizontal > 0 && vertical > 0) return CropHandle::BottomRight;
    if (horizontal < 0) return CropHandle::Left;
    if (horizontal > 0) return CropHandle::Right;
    if (vertical < 0) return CropHandle::Top;
    if (vertical > 0) return CropHandle::Bottom;
    return px >= l && px <= r && py >= t && py <= b ? CropHandle::Move : CropHandle::None;
}

CropRect dragCrop(const CropRect& start, CropHandle handle, double dx, double dy, double frameWidth,
                  double frameHeight, double aspect, double minSize)
{
    const double W = frameWidth, H = frameHeight;
    const double l = start.x, t = start.y, r = start.x + start.width, b = start.y + start.height;
    minSize = std::max(1.0, minSize);

    if (handle == CropHandle::None) return start;
    if (handle == CropHandle::Move) {
        return { clampTo(l + dx, 0.0, W - start.width), clampTo(t + dy, 0.0, H - start.height), start.width,
                 start.height };
    }

    const bool movesL = handle == CropHandle::Left || handle == CropHandle::TopLeft || handle == CropHandle::BottomLeft;
    const bool movesR =
        handle == CropHandle::Right || handle == CropHandle::TopRight || handle == CropHandle::BottomRight;
    const bool movesT = handle == CropHandle::Top || handle == CropHandle::TopLeft || handle == CropHandle::TopRight;
    const bool movesB =
        handle == CropHandle::Bottom || handle == CropHandle::BottomLeft || handle == CropHandle::BottomRight;
    const bool movesX = movesL || movesR;
    const bool movesY = movesT || movesB;

    if (aspect <= 0.0) {
        double nl = l, nt = t, nr = r, nb = b;
        if (movesL) nl = clampTo(l + dx, 0.0, r - minSize);
        if (movesR) nr = clampTo(r + dx, l + minSize, W);
        if (movesT) nt = clampTo(t + dy, 0.0, b - minSize);
        if (movesB) nb = clampTo(b + dy, t + minSize, H);
        return { nl, nt, nr - nl, nb - nt };
    }

    // Smallest width whose height (width / aspect) is also at least minSize.
    const double minW = aspect >= 1.0 ? minSize * aspect : minSize;

    // The fixed side (or corner) the rectangle grows from.
    const double ax = movesL ? r : l;
    const double ay = movesT ? b : t;
    const double roomX = movesL ? ax : W - ax;
    const double roomY = movesT ? ay : H - ay;

    double w = 0.0;
    double maxW = 0.0;
    if (movesX && movesY) {
        const double wantW = movesL ? ax - (l + dx) : (r + dx) - ax;
        const double wantH = movesT ? ay - (t + dy) : (b + dy) - ay;
        w = std::max(wantW, wantH * aspect);
        maxW = std::min(roomX, roomY * aspect);
    } else if (movesX) {
        w = movesL ? ax - (l + dx) : (r + dx) - ax;
        maxW = std::min(roomX, H * aspect);
    } else {
        w = (movesT ? ay - (t + dy) : (b + dy) - ay) * aspect;
        maxW = std::min(W, roomY * aspect);
    }
    w = clampTo(w, minW, maxW);
    const double h = w / aspect;

    CropRect out{ 0.0, 0.0, w, h };
    if (movesX) out.x = movesL ? ax - w : ax;
    else out.x = clampTo(l + start.width / 2 - w / 2, 0.0, W - w); // keep the horizontal centre
    if (movesY) out.y = movesT ? ay - h : ay;
    else out.y = clampTo(t + start.height / 2 - h / 2, 0.0, H - h); // keep the vertical centre
    return out;
}

CropRect fitAspect(const CropRect& crop, double aspect)
{
    if (aspect <= 0.0 || crop.width <= 0.0 || crop.height <= 0.0) return crop;
    double w = crop.width;
    double h = w / aspect;
    if (h > crop.height) {
        h = crop.height;
        w = h * aspect;
    }
    return { crop.x + (crop.width - w) / 2, crop.y + (crop.height - h) / 2, w, h };
}

CropOp toCropOp(const CropRect& crop, int frameWidth, int frameHeight)
{
    // Round the edges rather than the size, so a rectangle touching the frame edge stays there.
    const int x0 = std::clamp(roundToInt(crop.x), 0, std::max(0, frameWidth - 1));
    const int y0 = std::clamp(roundToInt(crop.y), 0, std::max(0, frameHeight - 1));
    const int x1 = std::clamp(roundToInt(crop.x + crop.width), x0 + 1, std::max(x0 + 1, frameWidth));
    const int y1 = std::clamp(roundToInt(crop.y + crop.height), y0 + 1, std::max(y0 + 1, frameHeight));
    return { x0, y0, x1 - x0, y1 - y0 };
}

std::optional<Snap> snapRange(double start, double end, const std::vector<double>& targets, double tolerance)
{
    std::optional<Snap> best;
    for (const double target : targets) {
        for (const double edge : { start, end }) {
            const double shift = target - edge;
            if (std::abs(shift) <= tolerance && (!best || std::abs(shift) < std::abs(best->shift))) best = Snap{ shift, target };
        }
    }
    return best;
}

std::vector<double> thumbnailTimes(double durationSec, int count)
{
    std::vector<double> times;
    if (!(durationSec > 0.0) || count <= 0) return times;
    times.reserve(static_cast<std::size_t>(count));
    const double slot = durationSec / count;
    for (int i = 0; i < count; ++i) times.push_back((i + 0.5) * slot);
    return times;
}

} // namespace regif
