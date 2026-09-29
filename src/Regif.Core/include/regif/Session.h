#pragma once

#include "MediaInfo.h"
#include "MediaProcessor.h"
#include "Operations.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace regif {

struct Stage {
    std::filesystem::path file;          // the original file for stage 0, a working file afterwards
    MediaInfo info;
    std::optional<Operation> operation;  // empty for the original
    std::string label;
};

// A staged editing session over one source file.
//
// Every applied operation renders a new file into the session's working directory and
// that file becomes the baseline for the next operation. Undo/redo move between stages
// without re-rendering. The original file is only ever read, and exportCurrent() refuses
// to write over it. The working directory is deleted when the session is destroyed.
//
// Not thread-safe: callers must not run two operations on one session at the same time.
class Session {
public:
    Session(std::filesystem::path original, const std::filesystem::path& workRoot, IMediaProcessor& processor);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    const std::filesystem::path& originalPath() const noexcept { return m_original; }
    const std::filesystem::path& workDirectory() const noexcept { return m_workDir; }
    const std::vector<Stage>& stages() const noexcept { return m_stages; }
    std::size_t currentIndex() const noexcept { return m_current; }
    const Stage& current() const noexcept { return m_stages[m_current]; }

    bool canUndo() const noexcept { return m_current > 0; }
    bool canRedo() const noexcept { return m_current + 1 < m_stages.size(); }
    void undo();
    void redo();
    void select(std::size_t index);

    // Renders `op` against the current stage and makes the result current. Stages after
    // the current one (the redo branch) are discarded only once the render succeeds.
    // Throws std::invalid_argument for invalid operations, OperationCancelled, MediaError.
    const Stage& apply(const Operation& op,
                       const ProgressCallback& progress = {},
                       const CancellationToken* cancel = nullptr);

    // Copies the current stage to `destination`. Throws if `destination` is the original.
    void exportCurrent(const std::filesystem::path& destination) const;

    // Removes leftover session directories (e.g. after a crash) older than maxAge.
    static void cleanupStaleSessions(const std::filesystem::path& workRoot, std::chrono::hours maxAge);

    static constexpr std::string_view kDirectoryPrefix = "session-";

private:
    void discardRedoStages();

    std::filesystem::path m_original;
    std::filesystem::path m_workDir;
    IMediaProcessor& m_processor;
    std::vector<Stage> m_stages;
    std::size_t m_current = 0;
    unsigned m_nextFileId = 1;
};

// True when both paths exist and refer to the same file (handles case, links, 8.3 names).
bool isSameFile(const std::filesystem::path& a, const std::filesystem::path& b);

std::string pathToUtf8(const std::filesystem::path& path);

} // namespace regif
