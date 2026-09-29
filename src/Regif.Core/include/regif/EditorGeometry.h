#pragma once

#include "Operations.h"

#include <vector>

namespace regif {

// Geometry behind the interactive editors (the crop overlay and the timeline). Kept here
// rather than in the app so it's portable and unit-tested.

// A rectangle in displayed source pixels. Doubles so dragging stays smooth when the preview
// is scaled; toCropOp rounds to whole pixels.
struct CropRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

enum class CropHandle { None, Move, Left, Top, Right, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };

// The part of `crop` under point (px, py). Handles win within `grip` of an edge or corner;
// inside the rectangle is Move; elsewhere None. All values in the same coordinate space.
CropHandle hitTestCrop(const CropRect& crop, double px, double py, double grip);

// `start` resized or moved by (dx, dy) through `handle`, kept inside the frameWidth x
// frameHeight frame and at least minSize on each side. With aspect > 0 (width / height) the
// result keeps that ratio: corners pin the opposite corner, edges keep the other axis centred.
CropRect dragCrop(const CropRect& start, CropHandle handle, double dx, double dy, double frameWidth,
                  double frameHeight, double aspect, double minSize);

// The largest rectangle with the given aspect that fits inside `crop`, centred on it.
// aspect <= 0 returns `crop` unchanged.
CropRect fitAspect(const CropRect& crop, double aspect);

// Whole-pixel crop for the rectangle, clamped to the frame.
CropOp toCropOp(const CropRect& crop, int frameWidth, int frameHeight);

// Times (seconds) at the centre of `count` equal slots across the clip, for timeline stills.
std::vector<double> thumbnailTimes(double durationSec, int count);

} // namespace regif
