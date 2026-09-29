#include <regif/Session.h>

#include <algorithm>
#include <format>
#include <random>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace regif {
namespace {

bool startsWith(std::string_view text, std::string_view prefix)
{
    return text.substr(0, prefix.size()) == prefix;
}

// Probes `file` and fills in the size from the file system, so fakes and real
// processors agree on it.
MediaInfo probeWithSize(IMediaProcessor& processor, const fs::path& file)
{
    MediaInfo info = processor.probe(file);
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (!ec) info.fileSizeBytes = size;
    return info;
}

fs::path createUniqueDirectory(const fs::path& root)
{
    fs::create_directories(root);

    std::random_device device;
    std::mt19937 random(device());
    const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto name = std::format("{}{}-{:08x}", Session::kDirectoryPrefix, epoch, random());
        const fs::path dir = root / name;
        if (fs::create_directory(dir)) return dir;
    }
    throw std::runtime_error("Couldn't create a working folder in " + pathToUtf8(root));
}

void removeQuietly(const fs::path& file) noexcept
{
    std::error_code ec;
    fs::remove(file, ec); // may fail if a viewer still has it open; the session folder is removed later anyway
}

} // namespace

bool isSameFile(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (fs::exists(a, ec) && fs::exists(b, ec)) {
        const bool same = fs::equivalent(a, b, ec);
        return !ec && same;
    }
    const fs::path na = fs::absolute(a, ec).lexically_normal();
    const fs::path nb = fs::absolute(b, ec).lexically_normal();
    return na == nb;
}

std::string pathToUtf8(const fs::path& path)
{
    const std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

Session::Session(fs::path original, const fs::path& workRoot, IMediaProcessor& processor)
    : m_original(std::move(original))
    , m_processor(processor)
{
    // Probe before touching the disk so an unreadable file leaves nothing behind.
    Stage first;
    first.file = m_original;
    first.info = probeWithSize(m_processor, m_original);
    first.label = "Original";

    m_workDir = createUniqueDirectory(workRoot);
    m_stages.push_back(std::move(first));
}

Session::~Session()
{
    // Only ever delete a folder this class created.
    if (!m_workDir.empty() && startsWith(pathToUtf8(m_workDir.filename()), kDirectoryPrefix)) {
        std::error_code ec;
        fs::remove_all(m_workDir, ec);
    }
}

void Session::undo()
{
    if (canUndo()) --m_current;
}

void Session::redo()
{
    if (canRedo()) ++m_current;
}

void Session::select(std::size_t index)
{
    if (index >= m_stages.size()) throw std::out_of_range("No such stage");
    m_current = index;
}

const Stage& Session::apply(const Operation& op, const ProgressCallback& progress, const CancellationToken* cancel)
{
    const Stage& base = current();
    if (auto problem = validate(op, base.info)) throw std::invalid_argument(*problem);

    const RenderPlan plan = planOperation(op, base.info);
    const unsigned id = m_nextFileId++;
    const fs::path partial = m_workDir / std::format("stage-{:03}.partial{}", id, plan.fileExtension);
    const fs::path finished = m_workDir / std::format("stage-{:03}{}", id, plan.fileExtension);

    CancellationToken neverCancelled;
    const CancellationToken& token = cancel ? *cancel : neverCancelled;

    Stage next;
    try {
        m_processor.render(base.file, base.info, plan, partial, progress, token);
        if (token.isCancelled()) throw OperationCancelled();
        fs::rename(partial, finished);
        next.file = finished;
        next.info = probeWithSize(m_processor, finished);
    } catch (...) {
        removeQuietly(partial);
        removeQuietly(finished);
        throw;
    }
    next.operation = op;
    next.label = describe(op);

    // The render succeeded, so the redo branch can go now.
    discardRedoStages();
    m_stages.push_back(std::move(next));
    m_current = m_stages.size() - 1;
    if (progress) progress(1.0);
    return current();
}

void Session::discardRedoStages()
{
    while (m_stages.size() > m_current + 1) {
        const Stage& stage = m_stages.back();
        const bool ownedFile = stage.operation.has_value()
            && stage.file.parent_path() == m_workDir
            && !isSameFile(stage.file, m_original);
        if (ownedFile) removeQuietly(stage.file);
        m_stages.pop_back();
    }
}

TextLayer Session::textLayer() const
{
    TextLayer layer = m_text;
    for (std::size_t i = 1; i <= m_current; ++i)
        layer = mapLayerForward(layer, *m_stages[i].operation, m_stages[i - 1].info, m_stages[i].info);
    return layer;
}

TextClip Session::toOriginal(TextClip clip) const
{
    for (std::size_t i = m_current; i > 0; --i)
        clip = mapClipBackward(clip, *m_stages[i].operation, m_stages[i - 1].info, m_stages[i].info);
    return clip;
}

TextTrack& Session::findTrack(std::uint64_t trackId)
{
    const auto it = std::find_if(m_text.tracks.begin(), m_text.tracks.end(),
                                 [&](const TextTrack& t) { return t.id == trackId; });
    if (it == m_text.tracks.end()) throw std::invalid_argument("No such text track");
    return *it;
}

std::uint64_t Session::addTextTrack(std::string name)
{
    TextTrack track;
    track.id = m_nextTextId++;
    track.name = std::move(name);
    m_text.tracks.push_back(std::move(track));
    return m_text.tracks.back().id;
}

void Session::removeTextTrack(std::uint64_t trackId)
{
    findTrack(trackId); // throws for unknown ids
    std::erase_if(m_text.tracks, [&](const TextTrack& t) { return t.id == trackId; });
}

std::uint64_t Session::addTextClip(std::uint64_t trackId, TextClip clip)
{
    TextTrack& track = findTrack(trackId);
    clip.id = m_nextTextId++;
    track.clips.push_back(toOriginal(std::move(clip)));
    return track.clips.back().id;
}

void Session::updateTextClip(const TextClip& clip)
{
    for (TextTrack& track : m_text.tracks) {
        for (TextClip& existing : track.clips) {
            if (existing.id == clip.id) {
                existing = toOriginal(clip);
                return;
            }
        }
    }
    throw std::invalid_argument("No such text clip");
}

void Session::removeTextClip(std::uint64_t clipId)
{
    for (TextTrack& track : m_text.tracks) {
        if (std::erase_if(track.clips, [&](const TextClip& c) { return c.id == clipId; }) > 0) return;
    }
    throw std::invalid_argument("No such text clip");
}

bool Session::exportBurnsInText() const
{
    return !textLayer().empty();
}

std::string Session::exportExtension() const
{
    const TextLayer text = textLayer();
    if (!text.empty()) return planBurnIn(text, current().info).fileExtension;
    return pathToUtf8(current().file.extension());
}

void Session::exportCurrent(const fs::path& destination, const ProgressCallback& progress,
                            const CancellationToken* cancel) const
{
    if (destination.empty()) throw std::invalid_argument("Choose where to save the file.");
    if (isSameFile(destination, m_original))
        throw std::invalid_argument("regif never overwrites the original file. Choose a different name or folder.");

    const TextLayer text = textLayer();
    if (text.empty()) {
        if (!isSameFile(destination, current().file))
            fs::copy_file(current().file, destination, fs::copy_options::overwrite_existing);
        if (progress) progress(1.0);
        return;
    }
    if (isSameFile(destination, current().file))
        throw std::invalid_argument("Choose a different file to export to.");

    // Render inside the session folder, then copy, so a failed or cancelled export never
    // leaves a half-written file where the user asked for one.
    const RenderPlan plan = planBurnIn(text, current().info);
    const fs::path partial = m_workDir / ("export.partial" + plan.fileExtension);
    CancellationToken neverCancelled;
    const CancellationToken& token = cancel ? *cancel : neverCancelled;
    try {
        m_processor.render(current().file, current().info, plan, partial, progress, token);
        if (token.isCancelled()) throw OperationCancelled();
        fs::copy_file(partial, destination, fs::copy_options::overwrite_existing);
    } catch (...) {
        removeQuietly(partial);
        throw;
    }
    removeQuietly(partial);
    if (progress) progress(1.0);
}

void Session::cleanupStaleSessions(const fs::path& workRoot, std::chrono::hours maxAge)
{
    std::error_code ec;
    if (!fs::is_directory(workRoot, ec)) return;

    const auto now = fs::file_time_type::clock::now();
    for (fs::directory_iterator it(workRoot, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc)) continue;
        if (!startsWith(pathToUtf8(it->path().filename()), kDirectoryPrefix)) continue;

        const auto modified = it->last_write_time(entryEc);
        if (entryEc || now - modified < maxAge) continue;

        fs::remove_all(it->path(), entryEc);
    }
}

} // namespace regif
