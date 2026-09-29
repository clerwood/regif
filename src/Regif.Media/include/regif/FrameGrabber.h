#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace regif {

// An upright, opaque BGRA image. Rows are tightly packed (stride = width * 4).
struct VideoFrameBgra {
    int width = 0;
    int height = 0;
    double timeSec = 0.0;
    std::vector<std::uint8_t> pixels;
};

// Decodes single frames from a video for the preview/scrubber. Keeps the file open
// (shared read access) so repeated grabs are fast. Not thread-safe: one grab at a time.
class FrameGrabber {
public:
    explicit FrameGrabber(const std::filesystem::path& file);
    ~FrameGrabber();

    FrameGrabber(FrameGrabber&&) noexcept;
    FrameGrabber& operator=(FrameGrabber&&) noexcept;
    FrameGrabber(const FrameGrabber&) = delete;
    FrameGrabber& operator=(const FrameGrabber&) = delete;

    // The frame on screen at `seconds`, scaled to fit within maxDimension and with any
    // container rotation applied. Throws MediaError.
    VideoFrameBgra grab(double seconds, int maxDimension = 1280);

    // Frames at each of `seconds` (ascending), for timeline thumbnails. Each frame is passed to
    // `onFrame` with its index as soon as it's decoded; return false from it to stop early.
    // GIFs are decoded in a single pass. Frames that can't be decoded are skipped.
    using FrameCallback = std::function<bool(std::size_t index, VideoFrameBgra&& frame)>;
    void grabSequence(const std::vector<double>& seconds, int maxDimension, const FrameCallback& onFrame);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace regif
