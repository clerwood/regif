#include "TestFramework.h"

#include <regif/Session.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace regif;

namespace {

// A throwaway directory under the system temp folder.
class TempDir {
public:
    TempDir()
    {
        std::random_device device;
        m_path = fs::temp_directory_path() / ("regif-tests-" + std::to_string(device()));
        fs::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

std::string readAll(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path& file, const std::string& text)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string serialize(const MediaInfo& info)
{
    std::ostringstream out;
    out << (info.kind == MediaKind::Gif ? "gif" : "video") << ' ' << info.width << ' ' << info.height << ' '
        << info.durationSec << ' ' << info.frameRate << '\n';
    return out.str();
}

// Stands in for FFmpeg: "media" files are text files holding a serialized MediaInfo.
// Rendering copies the input info, applies the crop size if the graph has one, and
// appends the filter graph so tests can see which stage each file came from.
class FakeProcessor : public IMediaProcessor {
public:
    enum class Failure { None, Error, CancelMidway };
    Failure failNext = Failure::None;
    std::vector<fs::path> renderedInputs;

    MediaInfo probe(const fs::path& file) override
    {
        std::ifstream in(file);
        std::string kind;
        MediaInfo info;
        if (!(in >> kind >> info.width >> info.height >> info.durationSec >> info.frameRate))
            throw MediaError("not a fake media file: " + pathToUtf8(file));
        info.kind = kind == "gif" ? MediaKind::Gif : MediaKind::Video;
        return info;
    }

    void render(const fs::path& input, const MediaInfo& inputInfo, const RenderPlan& plan, const fs::path& output,
                const ProgressCallback& progress, const CancellationToken& cancel) override
    {
        renderedInputs.push_back(input);
        MediaInfo out = inputInfo;
        out.kind = plan.outputKind;
        out.rotationDegrees = 0;
        int w = 0, h = 0;
        const auto crop = plan.filterGraph.find("crop=w=");
        if (crop != std::string::npos && std::sscanf(plan.filterGraph.c_str() + crop, "crop=w=%d:h=%d", &w, &h) == 2) {
            out.width = w;
            out.height = h;
        }
        writeAll(output, serialize(out) + plan.filterGraph + "\n");
        if (progress) progress(0.5);

        const Failure failure = failNext;
        failNext = Failure::None;
        if (failure == Failure::Error) throw MediaError("simulated decoder failure");
        if (failure == Failure::CancelMidway) {
            const_cast<CancellationToken&>(cancel).cancel();
            throw OperationCancelled();
        }
    }
};

MediaInfo sampleVideo()
{
    MediaInfo info;
    info.kind = MediaKind::Video;
    info.width = 640;
    info.height = 360;
    info.durationSec = 8.0;
    info.frameRate = 30.0;
    return info;
}

std::vector<fs::path> filesIn(const fs::path& dir)
{
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) files.push_back(entry.path());
    return files;
}

bool anyPartialFiles(const fs::path& dir)
{
    for (const auto& file : filesIn(dir))
        if (pathToUtf8(file.filename()).find(".partial") != std::string::npos) return true;
    return false;
}

struct Fixture {
    TempDir temp;
    fs::path original = temp.path() / "input clip.mp4";
    fs::path workRoot = temp.path() / "work";
    std::string originalBytes;
    FakeProcessor processor;

    Fixture()
    {
        originalBytes = serialize(sampleVideo()) + "original bytes\n";
        writeAll(original, originalBytes);
    }
};

} // namespace

TEST_CASE("a new session starts at the original without copying it")
{
    Fixture f;
    Session session(f.original, f.workRoot, f.processor);
    CHECK_EQ(session.stages().size(), std::size_t(1));
    CHECK_EQ(session.currentIndex(), std::size_t(0));
    CHECK(session.current().file == f.original);
    CHECK(!session.current().operation.has_value());
    CHECK_EQ(session.current().info.width, 640);
    CHECK_EQ(session.current().info.fileSizeBytes, std::uintmax_t(f.originalBytes.size()));
    CHECK(!session.canUndo());
    CHECK(!session.canRedo());
    CHECK(fs::is_directory(session.workDirectory()));
    CHECK(filesIn(session.workDirectory()).empty());
    CHECK_CONTAINS(pathToUtf8(session.workDirectory().filename()), "session-");
}

TEST_CASE("an unreadable file doesn't leave a working folder behind")
{
    Fixture f;
    writeAll(f.original, "garbage");
    CHECK_THROWS_AS(Session(f.original, f.workRoot, f.processor), MediaError);
    CHECK(!fs::exists(f.workRoot) || fs::is_empty(f.workRoot));
}

TEST_CASE("each applied change becomes the baseline for the next")
{
    Fixture f;
    Session session(f.original, f.workRoot, f.processor);

    session.apply(CropOp{ 0, 0, 320, 180 });
    CHECK_EQ(session.currentIndex(), std::size_t(1));
    CHECK_EQ(session.current().info.width, 320);
    CHECK_EQ(session.current().label, std::string("Crop to 320x180 at (0, 0)"));
    CHECK(session.current().file.parent_path() == session.workDirectory());
    CHECK_EQ(session.current().file.extension().string(), std::string(".mkv"));

    session.apply(ConvertToGifOp{});
    CHECK_EQ(session.stages().size(), std::size_t(3));
    CHECK(session.current().info.kind == MediaKind::Gif);
    CHECK_EQ(session.current().file.extension().string(), std::string(".gif"));

    session.apply(SpeedOp{ 2.0 });
    CHECK_EQ(session.stages().size(), std::size_t(4));

    // Every render read the previous stage, never the original after the first one.
    CHECK_EQ(f.processor.renderedInputs.size(), std::size_t(3));
    CHECK(f.processor.renderedInputs[0] == f.original);
    CHECK(f.processor.renderedInputs[1] == session.stages()[1].file);
    CHECK(f.processor.renderedInputs[2] == session.stages()[2].file);
}

TEST_CASE("the original file is never modified")
{
    Fixture f;
    {
        Session session(f.original, f.workRoot, f.processor);
        session.apply(CropOp{ 0, 0, 100, 100 });
        session.apply(TrimOp{ 1, 2 });
        session.undo();
        session.undo();
        session.apply(CutOp{ 1, 2 });
        CHECK_THROWS_AS(session.exportCurrent(f.original), std::invalid_argument);
    }
    CHECK_EQ(readAll(f.original), f.originalBytes);
}

TEST_CASE("invalid operations are rejected before rendering")
{
    Fixture f;
    Session session(f.original, f.workRoot, f.processor);
    CHECK_THROWS_AS(session.apply(SpeedOp{ 2.0 }), std::invalid_argument);           // video
    CHECK_THROWS_AS(session.apply(CropOp{ 0, 0, 5000, 10 }), std::invalid_argument); // too big
    CHECK(f.processor.renderedInputs.empty());
    CHECK_EQ(session.stages().size(), std::size_t(1));
}

TEST_CASE("undo, redo and select move between stages without rendering")
{
    Fixture f;
    Session session(f.original, f.workRoot, f.processor);
    session.apply(CropOp{ 0, 0, 320, 180 });
    session.apply(CropOp{ 0, 0, 160, 90 });

    session.undo();
    CHECK_EQ(session.currentIndex(), std::size_t(1));
    CHECK(session.canRedo());
    session.undo();
    CHECK_EQ(session.currentIndex(), std::size_t(0));
    CHECK(!session.canUndo());
    session.undo(); // no-op at the start
    CHECK_EQ(session.currentIndex(), std::size_t(0));
    session.redo();
    session.redo();
    session.redo(); // no-op at the end
    CHECK_EQ(session.currentIndex(), std::size_t(2));
    session.select(1);
    CHECK_EQ(session.current().info.width, 320);
    CHECK_THROWS_AS(session.select(7), std::out_of_range);
    CHECK_EQ(f.processor.renderedInputs.size(), std::size_t(2));
}

TEST_CASE("applying after undo replaces the redo stages and deletes their files")
{
    Fixture f;
    Session session(f.original, f.workRoot, f.processor);
    session.apply(CropOp{ 0, 0, 320, 180 });
    session.apply(CropOp{ 0, 0, 160, 90 });
    session.apply(TrimOp{ 1, 2 });
    const fs::path dropped1 = session.stages()[2].file;
    const fs::path dropped2 = session.stages()[3].file;

    session.select(1);
    session.apply(TrimOp{ 3, 4 });

    CHECK_EQ(session.stages().size(), std::size_t(3));
    CHECK_EQ(session.currentIndex(), std::size_t(2));
    CHECK(!fs::exists(dropped1));
    CHECK(!fs::exists(dropped2));
    CHECK(fs::exists(session.stages()[1].file));
    CHECK(fs::exists(session.current().file));
    CHECK(session.current().file != dropped1); // file names are never reused
    CHECK(fs::exists(f.original));
}

TEST_CASE("undoing to the original and applying keeps the original")
{
    Fixture f;
    Session session(f.original, f.workRoot, f.processor);
    session.apply(CropOp{ 0, 0, 320, 180 });
    session.undo();
    session.apply(CropOp{ 0, 0, 200, 100 });
    CHECK_EQ(session.stages().size(), std::size_t(2));
    CHECK(session.stages()[0].file == f.original);
    CHECK_EQ(readAll(f.original), f.originalBytes);
}

TEST_CASE("a failed render leaves the session unchanged")
{
    Fixture f;
    Session session(f.original, f.workRoot, f.processor);
    session.apply(CropOp{ 0, 0, 320, 180 });
    session.apply(CropOp{ 0, 0, 160, 90 });
    session.undo();

    f.processor.failNext = FakeProcessor::Failure::Error;
    CHECK_THROWS_AS(session.apply(TrimOp{ 1, 2 }), MediaError);

    CHECK_EQ(session.stages().size(), std::size_t(3)); // redo branch kept
    CHECK_EQ(session.currentIndex(), std::size_t(1));
    CHECK(session.canRedo());
    CHECK(fs::exists(session.stages()[2].file));
    CHECK(!anyPartialFiles(session.workDirectory()));
}

TEST_CASE("a cancelled render leaves the session unchanged")
{
    Fixture f;
    Session session(f.original, f.workRoot, f.processor);
    CancellationToken token;
    f.processor.failNext = FakeProcessor::Failure::CancelMidway;
    CHECK_THROWS_AS(session.apply(TrimOp{ 1, 2 }, {}, &token), OperationCancelled);
    CHECK(token.isCancelled());
    CHECK_EQ(session.stages().size(), std::size_t(1));
    CHECK(filesIn(session.workDirectory()).empty());

    token.reset();
    session.apply(TrimOp{ 1, 2 }, {}, &token);
    CHECK_EQ(session.stages().size(), std::size_t(2));
}

TEST_CASE("progress ends at 100 percent")
{
    Fixture f;
    Session session(f.original, f.workRoot, f.processor);
    std::vector<double> reports;
    session.apply(TrimOp{ 1, 2 }, [&](double p) { reports.push_back(p); });
    CHECK(!reports.empty());
    CHECK_EQ(reports.back(), 1.0);
}

TEST_CASE("export copies the current stage")
{
    Fixture f;
    Session session(f.original, f.workRoot, f.processor);
    session.apply(ConvertToGifOp{});
    const fs::path destination = f.temp.path() / "out.gif";
    session.exportCurrent(destination);
    CHECK_EQ(readAll(destination), readAll(session.current().file));

    // Overwriting an earlier export is fine.
    session.apply(OptimizeGifOp{});
    session.exportCurrent(destination);
    CHECK_EQ(readAll(destination), readAll(session.current().file));

    // Exporting the untouched original to a new name works too.
    session.select(0);
    const fs::path copy = f.temp.path() / "copy.mp4";
    session.exportCurrent(copy);
    CHECK_EQ(readAll(copy), f.originalBytes);
}

TEST_CASE("export refuses to overwrite the original, however it's spelled")
{
    Fixture f;
    Session session(f.original, f.workRoot, f.processor);
    session.apply(CropOp{ 0, 0, 10, 10 });
    CHECK_THROWS_AS(session.exportCurrent(f.original), std::invalid_argument);
    CHECK_THROWS_AS(session.exportCurrent(f.temp.path() / "work" / ".." / "input clip.mp4"), std::invalid_argument);
    CHECK_THROWS_AS(session.exportCurrent(fs::path()), std::invalid_argument);
    CHECK_EQ(readAll(f.original), f.originalBytes);
}

TEST_CASE("the working folder is removed with the session")
{
    Fixture f;
    fs::path workDir;
    {
        Session session(f.original, f.workRoot, f.processor);
        session.apply(CropOp{ 0, 0, 10, 10 });
        workDir = session.workDirectory();
        CHECK(fs::exists(workDir));
    }
    CHECK(!fs::exists(workDir));
    CHECK(fs::exists(f.original));
}

TEST_CASE("two sessions on the same file get separate folders")
{
    Fixture f;
    Session a(f.original, f.workRoot, f.processor);
    Session b(f.original, f.workRoot, f.processor);
    CHECK(a.workDirectory() != b.workDirectory());
}

TEST_CASE("stale session folders are cleaned up, other folders are not")
{
    Fixture f;
    const fs::path stale = f.workRoot / "session-1-deadbeef";
    const fs::path fresh = f.workRoot / "session-2-cafef00d";
    const fs::path unrelated = f.workRoot / "keep-me";
    fs::create_directories(stale);
    fs::create_directories(fresh);
    fs::create_directories(unrelated);
    writeAll(stale / "stage-001.gif", "x");

    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(24 * 30);
    fs::last_write_time(stale, old);
    fs::last_write_time(unrelated, old);

    Session::cleanupStaleSessions(f.workRoot, std::chrono::hours(24 * 7));
    CHECK(!fs::exists(stale));
    CHECK(fs::exists(fresh));
    CHECK(fs::exists(unrelated));

    Session::cleanupStaleSessions(f.temp.path() / "does-not-exist", std::chrono::hours(1)); // no throw
}

namespace {

bool closeTo(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}

} // namespace

TEST_CASE("text lives in the original's coordinates across edits, undo and redo")
{
    Fixture f;
    Session session(f.original, f.workRoot, f.processor);
    const auto track = session.addTextTrack("Subtitles");
    session.apply(CropOp{ 40, 20, 320, 180 });
    session.apply(TrimOp{ 2, 6 });

    TextClip c;
    c.text = "Hi";
    c.startSec = 1;
    c.endSec = 2;
    c.x = 160;
    c.y = 90;
    const auto id = session.addTextClip(track, c);

    // From the stage it was added on, it's where it was put.
    TextLayer layer = session.textLayer();
    const TextClip* seen = layer.findClip(id);
    CHECK(seen && closeTo(seen->x, 160) && closeTo(seen->startSec, 1));

    // From the original: before the crop and the trim.
    session.select(0);
    layer = session.textLayer();
    seen = layer.findClip(id);
    CHECK(seen && closeTo(seen->x, 200) && closeTo(seen->y, 110) && closeTo(seen->startSec, 3) && closeTo(seen->endSec, 4));

    // Edited there, the change shows at the later stage too.
    TextClip moved = *seen;
    moved.x = 240;
    session.updateTextClip(moved);
    session.select(2);
    layer = session.textLayer();
    seen = layer.findClip(id);
    CHECK(seen && closeTo(seen->x, 200) && closeTo(seen->startSec, 1));

    CHECK_THROWS_AS(session.updateTextClip(TextClip{}), std::invalid_argument);
    CHECK_THROWS_AS(session.addTextClip(999, c), std::invalid_argument);
    CHECK_THROWS_AS(session.removeTextTrack(999), std::invalid_argument);

    session.removeTextClip(id);
    CHECK(session.textLayer().empty());
    CHECK_EQ(session.textLayer().tracks.size(), std::size_t(1));
    session.removeTextTrack(track);
    CHECK(session.textLayer().tracks.empty());
}

TEST_CASE("export burns text in, and copies when there's none")
{
    Fixture f;
    Session session(f.original, f.workRoot, f.processor);
    CHECK(!session.exportBurnsInText());
    CHECK_EQ(session.exportExtension(), std::string(".mp4"));

    const auto track = session.addTextTrack("Titles");
    TextClip c;
    c.text = "Title";
    c.startSec = 0;
    c.endSec = 1;
    session.addTextClip(track, c);
    CHECK(session.exportBurnsInText());
    CHECK_EQ(session.exportExtension(), std::string(".mkv"));

    const fs::path out = f.temp.path() / "titled.mkv";
    std::vector<double> progress;
    session.exportCurrent(out, [&](double p) { progress.push_back(p); });
    CHECK_CONTAINS(readAll(out), "drawtext=");
    CHECK(!progress.empty() && progress.back() == 1.0);
    CHECK(!anyPartialFiles(session.workDirectory()));
    CHECK_EQ(readAll(f.original), f.originalBytes);
    CHECK_THROWS_AS(session.exportCurrent(f.original), std::invalid_argument);

    // A failed render leaves nothing behind.
    f.processor.failNext = FakeProcessor::Failure::Error;
    const fs::path failed = f.temp.path() / "failed.mkv";
    CHECK_THROWS_AS(session.exportCurrent(failed), MediaError);
    CHECK(!fs::exists(failed));
    CHECK(!anyPartialFiles(session.workDirectory()));
}

TEST_CASE("isSameFile and pathToUtf8")
{
    Fixture f;
    CHECK(isSameFile(f.original, f.original));
    CHECK(isSameFile(f.original, f.temp.path() / "." / "input clip.mp4"));
    CHECK(!isSameFile(f.original, f.temp.path() / "other.mp4"));
    CHECK_EQ(pathToUtf8(fs::path(u8"caf\u00e9.gif")), std::string("caf\xc3\xa9.gif"));
}
