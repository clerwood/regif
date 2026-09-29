#pragma once

#include "MediaInfo.h"
#include "RenderPlan.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <stdexcept>

namespace regif {

// Called with a fraction in [0, 1]. May be invoked from a worker thread.
using ProgressCallback = std::function<void(double)>;

class CancellationToken {
public:
    void cancel() noexcept { m_cancelled.store(true, std::memory_order_relaxed); }
    void reset() noexcept { m_cancelled.store(false, std::memory_order_relaxed); }
    bool isCancelled() const noexcept { return m_cancelled.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> m_cancelled{ false };
};

class MediaError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class OperationCancelled : public std::runtime_error {
public:
    OperationCancelled() : std::runtime_error("Operation cancelled") {}
};

// Implemented by Regif.Media (FFmpeg) and by fakes in tests.
// Implementations must treat `input` as read-only.
class IMediaProcessor {
public:
    virtual ~IMediaProcessor() = default;

    virtual MediaInfo probe(const std::filesystem::path& file) = 0;

    virtual void render(const std::filesystem::path& input,
                        const MediaInfo& inputInfo,
                        const RenderPlan& plan,
                        const std::filesystem::path& output,
                        const ProgressCallback& progress,
                        const CancellationToken& cancel) = 0;
};

} // namespace regif
