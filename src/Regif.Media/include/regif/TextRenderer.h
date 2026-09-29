#pragma once

#include <regif/FrameGrabber.h>
#include <regif/TextLayer.h>

namespace regif {

// One text clip drawn by FFmpeg's drawtext, with the same filter export uses, so the preview
// shows exactly what will be burned in.
struct RenderedText {
    VideoFrameBgra image; // premultiplied BGRA, cropped to the drawn pixels; empty when nothing is drawn
    double offsetX = 0.0; // the image's top-left corner relative to the clip's anchor, in pixels
    double offsetY = 0.0;
};

// Thread-safe. Throws MediaError, for example when the font file can't be loaded.
RenderedText renderText(const TextClip& clip);

} // namespace regif
