#include "pch.h"

#include "MainWindow.xaml.h"
#include "UiHelpers.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <regif/FfmpegProcessor.h>
#include <regif/MediaInfo.h>

#include <winrt/Windows.Globalization.NumberFormatting.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <stdexcept>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using winrt::Windows::Foundation::IInspectable;

namespace fs = std::filesystem;
namespace imaging = winrt::Microsoft::UI::Xaml::Media::Imaging;
namespace pickers = winrt::Windows::Storage::Pickers;
namespace streams = winrt::Windows::Storage::Streams;
namespace datatransfer = winrt::Windows::ApplicationModel::DataTransfer;
namespace media = winrt::Microsoft::UI::Xaml::Media;
namespace shapes = winrt::Microsoft::UI::Xaml::Shapes;
namespace xinput = winrt::Microsoft::UI::Xaml::Input;
using winrt::Microsoft::UI::Input::InputSystemCursorShape;
using ::regif::CropHandle;

namespace winrt::Regif::implementation {
namespace {

constexpr std::array kOpenExtensions{ L".gif", L".mp4", L".mov", L".m4v", L".mkv", L".webm",
                                      L".avi", L".wmv", L".mpg", L".mpeg", L".ts", L".flv" };

::regif::IMediaProcessor& Processor()
{
    static ::regif::FfmpegProcessor processor; // stateless; outlives every session
    return processor;
}

fs::path SessionRoot()
{
    PWSTR raw = nullptr;
    fs::path root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) root = raw;
    CoTaskMemFree(raw);
    if (root.empty()) root = fs::temp_directory_path();
    return root / L"regif" / L"sessions";
}

fs::path ExecutableDirectory()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return fs::path(buffer).parent_path();
}

winrt::fire_and_forget CleanupStaleSessionsAsync()
{
    co_await winrt::resume_background();
    try {
        ::regif::Session::cleanupStaleSessions(SessionRoot(), std::chrono::hours(24 * 7));
    } catch (...) {
        // Best effort: leftovers are retried on the next start.
    }
}

std::vector<std::uint8_t> ReadAllBytes(const fs::path& file)
{
    const auto size = fs::file_size(file);
    if (size > 0xFFFFFFFFull) throw std::runtime_error("This GIF is too large to preview.");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Couldn't read " + ::regif::pathToUtf8(file));
    return bytes;
}

::regif::Dither DitherFromIndex(int index)
{
    switch (index) {
    case 0: return ::regif::Dither::None;
    case 1: return ::regif::Dither::Bayer;
    case 2: return ::regif::Dither::FloydSteinberg;
    default: return ::regif::Dither::Sierra2_4a;
    }
}

void UseDecimalFormat(NumberBox const& box, double increment)
{
    using namespace winrt::Windows::Globalization::NumberFormatting;
    IncrementNumberRounder rounder;
    rounder.Increment(increment);
    rounder.RoundingAlgorithm(RoundingAlgorithm::RoundHalfUp);
    DecimalFormatter formatter;
    formatter.IntegerDigits(1);
    formatter.FractionDigits(2);
    formatter.NumberRounder(rounder);
    box.NumberFormatter(formatter);
}

// Crop overlay, in screen pixels.
constexpr double kCropGrip = 10.0;       // how close to an edge counts as grabbing it
constexpr double kCropHandleSize = 10.0;
constexpr double kCropMinSize = 12.0;
constexpr std::array kCropHandles{ CropHandle::TopLeft,     CropHandle::Top,    CropHandle::TopRight,
                                   CropHandle::Right,       CropHandle::BottomRight, CropHandle::Bottom,
                                   CropHandle::BottomLeft,  CropHandle::Left };

InputSystemCursorShape CursorFor(CropHandle handle)
{
    switch (handle) {
    case CropHandle::Move: return InputSystemCursorShape::SizeAll;
    case CropHandle::Left:
    case CropHandle::Right: return InputSystemCursorShape::SizeWestEast;
    case CropHandle::Top:
    case CropHandle::Bottom: return InputSystemCursorShape::SizeNorthSouth;
    case CropHandle::TopLeft:
    case CropHandle::BottomRight: return InputSystemCursorShape::SizeNorthwestSoutheast;
    case CropHandle::TopRight:
    case CropHandle::BottomLeft: return InputSystemCursorShape::SizeNortheastSouthwest;
    default: return InputSystemCursorShape::Arrow;
    }
}


} // namespace

void MainWindow::InitializeComponent()
{
    MainWindowT::InitializeComponent();

    const double scale = GetDpiForWindow(WindowHandle()) / 96.0;
    AppWindow().Resize({ static_cast<int32_t>(1320 * scale), static_cast<int32_t>(860 * scale) });
    const fs::path icon = ExecutableDirectory() / L"Assets" / L"regif.ico";
    if (std::error_code ec; fs::exists(icon, ec)) AppWindow().SetIcon(icon.wstring());

    for (NumberBox box : { TrimStart(), TrimEnd(), CutStart(), CutEnd() }) UseDecimalFormat(box, 0.01);
    UseDecimalFormat(SpeedFactor(), 0.05);
    CreateCropShapes();
    InitializeText();

    m_thumbnailTimer = DispatcherQueue().CreateTimer();
    m_thumbnailTimer.Interval(std::chrono::milliseconds(250));
    m_thumbnailTimer.IsRepeating(false);
    m_thumbnailTimer.Tick([this](auto&&, auto&&) {
        if (!m_closed && WantedThumbnailCount() != static_cast<int>(m_thumbnails.size())) LoadThumbnails();
    });

    Closed([this](IInspectable const&, WindowEventArgs const&) {
        m_closed = true;
        m_cancel->cancel();
        ++m_previewGeneration;
        m_grabber.reset();
        m_thumbnailTimer.Stop();
        if (m_thumbnailCancel) m_thumbnailCancel->store(true);
        // Deleting the session removes its working folder. A busy session is still in use by a
        // background render; it goes away with the window once that render stops.
        if (!m_busy) m_session.reset();
    });

    CleanupStaleSessionsAsync();
    RefreshUi();
}

// ---------------------------------------------------------------------------------------
// Opening, exporting and history

winrt::fire_and_forget MainWindow::OnOpenClick(IInspectable, RoutedEventArgs)
{
    if (m_busy) co_return;
    auto lifetime = get_strong();
    auto queue = DispatcherQueue();

    pickers::FileOpenPicker picker;
    InitializeWithWindow(picker);
    picker.ViewMode(pickers::PickerViewMode::Thumbnail);
    picker.SuggestedStartLocation(pickers::PickerLocationId::VideosLibrary);
    for (const wchar_t* extension : kOpenExtensions) picker.FileTypeFilter().Append(extension);

    winrt::Windows::Storage::StorageFile file{ nullptr };
    try {
        file = co_await picker.PickSingleFileAsync();
    } catch (winrt::hresult_error const&) {
        file = nullptr;
    }
    if (!co_await ResumeOn{ queue } || !file || m_closed) co_return;
    OpenFile(fs::path(std::wstring(file.Path())));
}

winrt::fire_and_forget MainWindow::OnDrop(IInspectable, DragEventArgs args)
{
    auto lifetime = get_strong();
    auto queue = DispatcherQueue();
    auto view = args.DataView();
    if (m_busy || !view.Contains(datatransfer::StandardDataFormats::StorageItems())) co_return;

    auto deferral = args.GetDeferral();
    winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Storage::IStorageItem> items{ nullptr };
    try {
        items = co_await view.GetStorageItemsAsync();
    } catch (winrt::hresult_error const&) {
        items = nullptr;
    }
    deferral.Complete();
    if (!co_await ResumeOn{ queue } || !items || items.Size() == 0 || m_closed) co_return;
    OpenFile(fs::path(std::wstring(items.GetAt(0).Path())));
}

void MainWindow::OnDragOver(IInspectable const&, DragEventArgs const& args)
{
    if (m_busy || !args.DataView().Contains(datatransfer::StandardDataFormats::StorageItems())) return;
    args.AcceptedOperation(datatransfer::DataPackageOperation::Copy);
    args.DragUIOverride().Caption(L"Open in regif");
}

winrt::fire_and_forget MainWindow::OpenFile(fs::path path)
{
    if (m_busy) co_return;
    auto lifetime = get_strong();
    auto queue = DispatcherQueue();
    SetBusy(true, winrt::hstring(L"Opening " + path.filename().wstring()));

    std::unique_ptr<::regif::Session> session;
    std::exception_ptr error;
    co_await winrt::resume_background();
    try {
        session = std::make_unique<::regif::Session>(path, SessionRoot(), Processor());
    } catch (...) {
        error = std::current_exception();
    }
    if (!co_await ResumeOn{ queue } || m_closed) co_return;

    SetBusy(false);
    if (error) {
        ShowError(L"Couldn't open this file", error);
        co_return;
    }

    ++m_previewGeneration;
    m_grabber.reset();
    if (m_thumbnailCancel) m_thumbnailCancel->store(true); // releases the old stage file sooner
    m_session = std::move(session); // the previous session's working folder is deleted here
    Title(winrt::hstring(path.filename().wstring() + L" \u2013 regif"));
    StatusBar().IsOpen(false);
    m_playheadSec = 0.0;
    m_textVisuals.clear();
    m_selectedTrack = 0;
    m_selectedClip = 0;
    ResetInputs();
    ShowCurrentStage();
}

winrt::fire_and_forget MainWindow::OnExportClick(IInspectable, RoutedEventArgs)
{
    if (!m_session || m_busy) co_return;
    auto lifetime = get_strong();
    auto queue = DispatcherQueue();

    const ::regif::Stage& stage = m_session->current();
    const bool isGif = stage.info.kind == ::regif::MediaKind::Gif;
    const bool withText = m_session->exportBurnsInText(); // renders instead of copying
    std::wstring extension = Wide(m_session->exportExtension());
    if (extension.empty()) extension = isGif ? L".gif" : L".mkv";

    pickers::FileSavePicker picker;
    InitializeWithWindow(picker);
    picker.SuggestedStartLocation(isGif ? pickers::PickerLocationId::PicturesLibrary
                                        : pickers::PickerLocationId::VideosLibrary);
    picker.FileTypeChoices().Insert(isGif ? L"GIF image" : L"Video",
                                    winrt::single_threaded_vector<winrt::hstring>({ winrt::hstring(extension) }));
    picker.SuggestedFileName(winrt::hstring(m_session->originalPath().stem().wstring() + L"-edited"));

    winrt::Windows::Storage::StorageFile file{ nullptr };
    try {
        file = co_await picker.PickSaveFileAsync();
    } catch (winrt::hresult_error const&) {
        file = nullptr;
    }
    if (!co_await ResumeOn{ queue } || !file || m_closed || !m_session || m_busy) co_return;

    const fs::path destination(std::wstring(file.Path()));
    SetBusy(true, withText ? L"Exporting with text" : L"Exporting");
    ::regif::Session* session = m_session.get();
    auto cancel = m_cancel;
    cancel->reset();
    ::regif::ProgressCallback progress = [queue, weak = get_weak()](double fraction) {
        queue.TryEnqueue([weak, fraction] {
            if (auto self = weak.get()) self->ReportProgress(fraction);
        });
    };
    bool cancelled = false;
    std::exception_ptr error;
    co_await winrt::resume_background();
    try {
        session->exportCurrent(destination, progress, cancel.get());
    } catch (::regif::OperationCancelled const&) {
        cancelled = true;
    } catch (...) {
        error = std::current_exception();
    }
    if (!co_await ResumeOn{ queue } || m_closed) co_return;

    SetBusy(false);
    if (cancelled) {
        ShowStatus(InfoBarSeverity::Informational, L"Export cancelled", L"Nothing was written.");
    } else if (error) {
        ShowError(L"Couldn't export", error);
    } else {
        ShowStatus(InfoBarSeverity::Success, L"Exported", winrt::hstring(destination.wstring()));
    }
}

void MainWindow::OnUndoClick(IInspectable const&, RoutedEventArgs const&)
{
    if (!m_session || m_busy || !m_session->canUndo()) return;
    m_session->undo();
    ResetInputs();
    ShowCurrentStage();
}

void MainWindow::OnRedoClick(IInspectable const&, RoutedEventArgs const&)
{
    if (!m_session || m_busy || !m_session->canRedo()) return;
    m_session->redo();
    ResetInputs();
    ShowCurrentStage();
}

void MainWindow::OnCancelClick(IInspectable const&, RoutedEventArgs const&)
{
    m_cancel->cancel();
    ShowStatus(InfoBarSeverity::Informational, L"Cancelling", L"");
}

void MainWindow::OnHistorySelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
{
    if (m_updatingUi || !m_session || m_busy) return;
    const int32_t index = HistoryList().SelectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) == m_session->currentIndex()) return;
    m_session->select(static_cast<std::size_t>(index));
    ResetInputs();
    ShowCurrentStage();
}

// ---------------------------------------------------------------------------------------
// Operations

void MainWindow::OnCropClick(IInspectable const&, RoutedEventArgs const&)
{
    ApplyOperation(::regif::CropOp{ IntValue(CropX(), 0), IntValue(CropY(), 0), IntValue(CropWidth(), 0),
                                    IntValue(CropHeight(), 0) });
}

void MainWindow::OnCropResetClick(IInspectable const&, RoutedEventArgs const&)
{
    ResetCrop();
    LayoutCropOverlay();
}

void MainWindow::OnTrimClick(IInspectable const&, RoutedEventArgs const&)
{
    ApplyOperation(::regif::TrimOp{ Value(TrimStart(), 0.0), Value(TrimEnd(), 0.0) });
}

void MainWindow::OnCutClick(IInspectable const&, RoutedEventArgs const&)
{
    ApplyOperation(::regif::CutOp{ Value(CutStart(), 0.0), Value(CutEnd(), 0.0) });
}

void MainWindow::OnSpeedClick(IInspectable const&, RoutedEventArgs const&)
{
    ApplyOperation(::regif::SpeedOp{ Value(SpeedFactor(), 1.0) });
}

void MainWindow::OnConvertClick(IInspectable const&, RoutedEventArgs const&)
{
    ::regif::ConvertToGifOp op;
    op.fps = Value(ConvertFps(), 15.0);
    op.width = IntValue(ConvertWidth(), 0);
    op.gif.maxColors = IntValue(ConvertColors(), 256);
    op.gif.dither = DitherFromIndex(ConvertDither().SelectedIndex());
    op.gif.perFramePalette = Checked(ConvertPerFrame());
    op.loop = Checked(ConvertLoop());
    ApplyOperation(op);
}

void MainWindow::OnOptimizeClick(IInspectable const&, RoutedEventArgs const&)
{
    ::regif::OptimizeGifOp op;
    op.fps = Value(OptimizeFps(), 0.0);
    op.width = IntValue(OptimizeWidth(), 0);
    op.gif.maxColors = IntValue(OptimizeColors(), 128);
    op.gif.dither = DitherFromIndex(OptimizeDither().SelectedIndex());
    ApplyOperation(op);
}

void MainWindow::OnUsePlayheadClick(IInspectable const& sender, RoutedEventArgs const&)
{
    const auto tag = winrt::unbox_value_or<winrt::hstring>(sender.as<FrameworkElement>().Tag(), L"");
    const double t = m_playheadSec;
    if (tag == L"TrimStart") TrimStart().Value(t);
    else if (tag == L"TrimEnd") TrimEnd().Value(t);
    else if (tag == L"CutStart") CutStart().Value(t);
    else if (tag == L"CutEnd") CutEnd().Value(t);
}

winrt::fire_and_forget MainWindow::ApplyOperation(::regif::Operation op)
{
    if (!m_session || m_busy) co_return;
    if (auto problem = ::regif::validate(op, m_session->current().info)) {
        ShowStatus(InfoBarSeverity::Warning, L"Can't apply this change", winrt::to_hstring(*problem));
        co_return;
    }

    auto lifetime = get_strong();
    auto queue = DispatcherQueue();
    const std::wstring label = Wide(::regif::describe(op));
    SetBusy(true, winrt::hstring(label));

    ::regif::Session* session = m_session.get();
    auto cancel = m_cancel;
    cancel->reset();
    ::regif::ProgressCallback progress = [queue, weak = get_weak()](double fraction) {
        queue.TryEnqueue([weak, fraction] {
            if (auto self = weak.get()) self->ReportProgress(fraction);
        });
    };

    bool cancelled = false;
    std::exception_ptr error;
    co_await winrt::resume_background();
    try {
        session->apply(op, progress, cancel.get());
    } catch (::regif::OperationCancelled const&) {
        cancelled = true;
    } catch (...) {
        error = std::current_exception();
    }
    if (!co_await ResumeOn{ queue } || m_closed) co_return;

    SetBusy(false);
    if (cancelled) {
        ShowStatus(InfoBarSeverity::Informational, L"Cancelled", L"Nothing was changed.");
    } else if (error) {
        ShowError(winrt::hstring(L"Couldn't " + label), error);
    } else {
        ShowStatus(InfoBarSeverity::Success, winrt::hstring(label),
                   winrt::to_hstring(::regif::summarize(m_session->current().info)));
        ResetInputs();
        ShowCurrentStage();
    }
}

// ---------------------------------------------------------------------------------------
// Preview

winrt::fire_and_forget MainWindow::ShowCurrentStage()
{
    auto lifetime = get_strong();
    auto queue = DispatcherQueue();
    const unsigned generation = ++m_previewGeneration;
    m_grabber.reset();
    m_gifBitmap = nullptr;
    PreviewImage().Source(nullptr);
    m_playing = false;
    RefreshUi();
    LoadThumbnails();
    RefreshTextVisuals(); // stages can move, retime or rescale text
    RefreshTextPanel();
    if (!m_session) co_return;

    const ::regif::Stage stage = m_session->current();
    const bool isGif = stage.info.kind == ::regif::MediaKind::Gif;

    std::shared_ptr<::regif::FrameGrabber> grabber;
    std::vector<std::uint8_t> bytes;
    std::exception_ptr error;
    co_await winrt::resume_background();
    try {
        grabber = std::make_shared<::regif::FrameGrabber>(stage.file);
        if (isGif) bytes = ReadAllBytes(stage.file); // read into memory so the file stays unlocked
    } catch (...) {
        error = std::current_exception();
    }
    if (!co_await ResumeOn{ queue } || m_closed || generation != m_previewGeneration) co_return;
    if (error) {
        ShowError(L"Couldn't show a preview", error);
        co_return;
    }

    m_grabber = grabber;
    if (!isGif) {
        RequestFrame(m_playheadSec);
        co_return;
    }

    imaging::BitmapImage bitmap;
    try {
        streams::Buffer buffer(static_cast<uint32_t>(bytes.size()));
        if (!bytes.empty()) std::memcpy(buffer.data(), bytes.data(), bytes.size());
        buffer.Length(static_cast<uint32_t>(bytes.size()));
        streams::InMemoryRandomAccessStream stream;
        co_await stream.WriteAsync(buffer);
        stream.Seek(0);
        if (!co_await ResumeOn{ queue }) co_return;
        co_await bitmap.SetSourceAsync(stream); // animated GIFs play automatically
    } catch (winrt::hresult_error const&) {
        error = std::current_exception();
    }
    if (!co_await ResumeOn{ queue } || m_closed || generation != m_previewGeneration) co_return;
    if (error) {
        ShowError(L"Couldn't show a preview", error);
        co_return;
    }
    m_gifBitmap = bitmap;
    PreviewImage().Source(bitmap);
}

// Decodes at most one preview frame at a time; requests that arrive meanwhile are
// coalesced so dragging the slider never queues up stale work.
winrt::fire_and_forget MainWindow::RequestFrame(double seconds)
{
    m_pendingFrameTime = seconds;
    if (m_frameInFlight) co_return;
    m_frameInFlight = true;

    auto lifetime = get_strong();
    auto queue = DispatcherQueue();
    while (m_pendingFrameTime && m_grabber && !m_closed) {
        const double target = *m_pendingFrameTime;
        m_pendingFrameTime.reset();
        const unsigned generation = m_previewGeneration;
        const unsigned stillToken = m_stillToken;
        auto grabber = m_grabber;

        std::optional<::regif::VideoFrameBgra> frame;
        co_await winrt::resume_background();
        try {
            frame = grabber->grab(target);
        } catch (...) {
            // A missing preview frame isn't worth interrupting the user for.
        }
        if (!co_await ResumeOn{ queue }) co_return;
        if (frame && !m_closed && generation == m_previewGeneration && stillToken == m_stillToken) ShowFrame(*frame);
    }
    m_frameInFlight = false;
}

void MainWindow::ShowFrame(const ::regif::VideoFrameBgra& frame)
{
    if (frame.width <= 0 || frame.height <= 0) return;
    if (!m_frameBitmap || m_frameBitmap.PixelWidth() != frame.width || m_frameBitmap.PixelHeight() != frame.height)
        m_frameBitmap = imaging::WriteableBitmap(frame.width, frame.height);

    auto buffer = m_frameBitmap.PixelBuffer();
    std::memcpy(buffer.data(), frame.pixels.data(), std::min<std::size_t>(buffer.Capacity(), frame.pixels.size()));
    m_frameBitmap.Invalidate();
    PreviewImage().Source(m_frameBitmap);
}

void MainWindow::OnPlayClick(IInspectable const&, RoutedEventArgs const&)
{
    if (!m_gifBitmap) return;
    ++m_stillToken; // drop any still frame that's still being decoded
    m_pendingFrameTime.reset();
    PreviewImage().Source(m_gifBitmap);
    m_gifBitmap.Play();
    m_playing = true; // text is shown for one moment, so hide it while the GIF plays
    LayoutTextOverlay();
}

void MainWindow::SetPlayhead(double seconds, bool showFrame)
{
    if (!m_session) return;
    m_playheadSec = std::clamp(seconds, 0.0, TimelineDuration());
    PositionText().Text(winrt::hstring(std::format(L"{:.2f} / {:.2f} s", m_playheadSec, TimelineDuration())));
    Canvas::SetLeft(Playhead(), TimelineX(m_playheadSec) - 1.0);
    m_playing = false;
    LayoutTextOverlay();
    if (showFrame) RequestFrame(m_playheadSec);
}

// ---------------------------------------------------------------------------------------
// Crop overlay

void MainWindow::CreateCropShapes()
{
    const media::SolidColorBrush shade(winrt::Windows::UI::Color{ 0x99, 0, 0, 0 });
    const media::SolidColorBrush white(winrt::Windows::UI::Color{ 0xFF, 0xFF, 0xFF, 0xFF });
    auto children = CropCanvas().Children();
    for (int i = 0; i < 4; ++i) {
        shapes::Rectangle r;
        r.Fill(shade);
        r.IsHitTestVisible(false);
        children.Append(r);
        m_cropShades.push_back(r);
    }
    m_cropBorder = shapes::Rectangle();
    m_cropBorder.Stroke(white);
    m_cropBorder.StrokeThickness(1.5);
    m_cropBorder.IsHitTestVisible(false);
    children.Append(m_cropBorder);
    for (std::size_t i = 0; i < kCropHandles.size(); ++i) {
        shapes::Rectangle handle;
        handle.Fill(white);
        handle.Stroke(shade);
        handle.StrokeThickness(1);
        handle.RadiusX(2);
        handle.RadiusY(2);
        handle.IsHitTestVisible(false);
        children.Append(handle);
        m_cropHandles.push_back(handle);
    }
}

// Where the frame is drawn inside PreviewSurface. The surface has the image's margin, and the
// image is Stretch="Uniform", so the frame is scaled to fit and centred.
MainWindow::PreviewLayout MainWindow::CropLayout()
{
    if (!m_session) return {};
    const auto& info = m_session->current().info;
    const double width = PreviewSurface().ActualWidth();
    const double height = PreviewSurface().ActualHeight();
    if (info.width <= 0 || info.height <= 0 || width <= 0 || height <= 0) return {};
    const double scale = std::min(width / info.width, height / info.height);
    return { scale, (width - info.width * scale) / 2, (height - info.height * scale) / 2 };
}

double MainWindow::LockedAspect()
{
    switch (CropAspect().SelectedIndex()) {
    case 1: {
        if (!m_session) return 0.0;
        const auto& info = m_session->current().info;
        return info.height > 0 ? static_cast<double>(info.width) / info.height : 0.0;
    }
    case 2: return 1.0;
    case 3: return 4.0 / 3.0;
    case 4: return 3.0 / 4.0;
    case 5: return 16.0 / 9.0;
    case 6: return 9.0 / 16.0;
    default: return 0.0;
    }
}

void MainWindow::ResetCrop()
{
    if (!m_session) return;
    const auto& info = m_session->current().info;
    m_crop = { 0.0, 0.0, static_cast<double>(info.width), static_cast<double>(info.height) };
    m_crop = ::regif::fitAspect(m_crop, LockedAspect());
    SyncCropBoxes();
}

void MainWindow::SyncCropBoxes()
{
    if (!m_session) return;
    const auto& info = m_session->current().info;
    const ::regif::CropOp op = ::regif::toCropOp(m_crop, info.width, info.height);
    m_syncingCrop = true;
    CropX().Value(op.x);
    CropY().Value(op.y);
    CropWidth().Value(op.width);
    CropHeight().Value(op.height);
    m_syncingCrop = false;
}

void MainWindow::LayoutCropOverlay()
{
    const bool visible = m_session && m_cropEditing && m_session->current().info.width > 0 &&
                         m_session->current().info.height > 0;
    PreviewSurface().Visibility(VisibleIf(m_session != nullptr));
    CropCanvas().Visibility(VisibleIf(visible));
    const PreviewLayout layout = visible ? CropLayout() : PreviewLayout{};
    if (!layout) return; // laid out again from SizeChanged once the surface has a size

    const auto& info = m_session->current().info;
    const double s = layout.scale;
    const double fl = layout.left, ft = layout.top, fw = info.width * s, fh = info.height * s;
    const double x = fl + m_crop.x * s, y = ft + m_crop.y * s, w = m_crop.width * s, h = m_crop.height * s;

    Place(m_cropShades[0], fl, ft, fw, y - ft);                    // above
    Place(m_cropShades[1], fl, y + h, fw, ft + fh - (y + h));      // below
    Place(m_cropShades[2], fl, y, x - fl, h);                      // left
    Place(m_cropShades[3], x + w, y, fl + fw - (x + w), h);        // right
    Place(m_cropBorder, x, y, w, h);

    const std::array<std::pair<double, double>, 8> centres{ { { x, y }, { x + w / 2, y }, { x + w, y },
                                                              { x + w, y + h / 2 }, { x + w, y + h },
                                                              { x + w / 2, y + h }, { x, y + h }, { x, y + h / 2 } } };
    for (std::size_t i = 0; i < centres.size(); ++i) {
        Place(m_cropHandles[i], centres[i].first - kCropHandleSize / 2, centres[i].second - kCropHandleSize / 2,
              kCropHandleSize, kCropHandleSize);
    }
}

void MainWindow::OnCropValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&)
{
    if (m_syncingCrop || !m_session) return;
    const auto& info = m_session->current().info;
    m_crop = { Value(CropX(), 0.0), Value(CropY(), 0.0), Value(CropWidth(), info.width), Value(CropHeight(), info.height) };
    LayoutCropOverlay();
}

void MainWindow::OnCropAspectChanged(IInspectable const&, SelectionChangedEventArgs const&)
{
    if (!m_session) return;
    const double aspect = LockedAspect();
    if (aspect <= 0.0) return;
    m_crop = ::regif::fitAspect(m_crop, aspect);
    SyncCropBoxes();
    LayoutCropOverlay();
}

void MainWindow::OnCropExpanding(Expander const&, ExpanderExpandingEventArgs const&)
{
    m_cropEditing = true;
    LayoutCropOverlay();
}

void MainWindow::OnCropCollapsed(Expander const&, ExpanderCollapsedEventArgs const&)
{
    m_cropEditing = false;
    LayoutCropOverlay();
}

void MainWindow::OnPreviewSurfaceSizeChanged(IInspectable const&, SizeChangedEventArgs const&)
{
    LayoutCropOverlay();
    LayoutTextOverlay();
}

// One surface serves the crop rectangle and the text. Crop edges and corners win, then text
// (so it can be dragged inside the crop rectangle), then moving the crop rectangle.
void MainWindow::OnPreviewPointerPressed(IInspectable const&, xinput::PointerRoutedEventArgs const& args)
{
    const PreviewLayout layout = CropLayout();
    if (m_busy || !layout) return;
    const auto p = args.GetCurrentPoint(PreviewSurface()).Position();
    const CropHandle handle =
        m_cropEditing ? ::regif::hitTestCrop(m_crop, (p.X - layout.left) / layout.scale, (p.Y - layout.top) / layout.scale,
                                             kCropGrip / layout.scale)
                      : CropHandle::None;
    const bool resizing = handle != CropHandle::None && handle != CropHandle::Move;
    const std::optional<::regif::TextClip> text = resizing ? std::nullopt : TextClipAt(p.X, p.Y);

    m_previewDragOrigin = p;
    if (text) {
        SelectClip(text->id);
        m_textDragging = true;
        m_clipDragStart = *text;
    } else if (handle != CropHandle::None) {
        m_cropDrag = handle;
        m_cropDragStart = m_crop;
    } else {
        SelectClip(0); // clicking the picture itself deselects text
        return;
    }
    PreviewSurface().CapturePointer(args.Pointer());
    args.Handled(true);
}

void MainWindow::OnPreviewPointerMoved(IInspectable const&, xinput::PointerRoutedEventArgs const& args)
{
    const PreviewLayout layout = CropLayout();
    if (!layout) return;
    const auto p = args.GetCurrentPoint(PreviewSurface()).Position();
    const double dx = (p.X - m_previewDragOrigin.X) / layout.scale;
    const double dy = (p.Y - m_previewDragOrigin.Y) / layout.scale;

    if (m_textDragging) {
        EditSelectedClip([&](::regif::TextClip& clip) {
            clip.x = m_clipDragStart.x + dx;
            clip.y = m_clipDragStart.y + dy;
        });
        args.Handled(true);
        return;
    }
    if (m_cropDrag == CropHandle::None) {
        CropHandle hover = m_cropEditing ? ::regif::hitTestCrop(m_crop, (p.X - layout.left) / layout.scale,
                                                                (p.Y - layout.top) / layout.scale, kCropGrip / layout.scale)
                                         : CropHandle::None;
        if ((hover == CropHandle::None || hover == CropHandle::Move) && TextClipAt(p.X, p.Y)) hover = CropHandle::Move;
        PreviewSurface().SetCursor(CursorFor(m_busy ? CropHandle::None : hover));
        return;
    }

    const auto& info = m_session->current().info;
    m_crop = ::regif::dragCrop(m_cropDragStart, m_cropDrag, dx, dy, info.width, info.height, LockedAspect(),
                               kCropMinSize / layout.scale);
    SyncCropBoxes();
    LayoutCropOverlay();
    args.Handled(true);
}

void MainWindow::OnPreviewPointerReleased(IInspectable const&, xinput::PointerRoutedEventArgs const& args)
{
    if (m_cropDrag == CropHandle::None && !m_textDragging) return;
    m_cropDrag = CropHandle::None;
    m_textDragging = false;
    PreviewSurface().ReleasePointerCapture(args.Pointer());
    args.Handled(true);
}

void MainWindow::OnPreviewPointerCaptureLost(IInspectable const&, xinput::PointerRoutedEventArgs const&)
{
    m_cropDrag = CropHandle::None;
    m_textDragging = false;
}

// ---------------------------------------------------------------------------------------
// Timeline

double MainWindow::TimelineDuration()
{
    return m_session ? std::max(0.0, m_session->current().info.durationSec) : 0.0;
}

double MainWindow::TimelineX(double seconds)
{
    const double strip = std::max(0.0, TimelineSurface().ActualWidth() - 2 * kHandleWidth);
    const double duration = TimelineDuration();
    return kHandleWidth + (duration > 0.0 ? std::clamp(seconds / duration, 0.0, 1.0) * strip : 0.0);
}

double MainWindow::TimelineSeconds(double x)
{
    const double strip = TimelineSurface().ActualWidth() - 2 * kHandleWidth;
    if (strip <= 0.0) return 0.0;
    return std::clamp((x - kHandleWidth) / strip, 0.0, 1.0) * TimelineDuration();
}

double MainWindow::FrameStep()
{
    const double fps = m_session ? m_session->current().info.frameRate : 0.0;
    return fps > 0.0 ? 1.0 / fps : 0.05;
}

int MainWindow::WantedThumbnailCount()
{
    if (!m_session) return 0;
    const auto& info = m_session->current().info;
    const double strip = TimelineSurface().ActualWidth() - 2 * kHandleWidth;
    if (strip <= 0.0 || info.width <= 0 || info.height <= 0 || info.durationSec <= 0.0) return 0;
    const double thumbWidth = std::max(24.0, kStripHeight * info.width / info.height);
    return std::clamp(static_cast<int>(std::ceil(strip / thumbWidth)), 1, kMaxThumbnails);
}

// Decodes the filmstrip for the current stage on a background thread with its own grabber,
// so it never waits behind (or delays) the preview frame. Stills appear as they're decoded.
winrt::fire_and_forget MainWindow::LoadThumbnails()
{
    if (m_thumbnailCancel) m_thumbnailCancel->store(true);
    m_thumbnailCancel.reset();
    FilmstripCanvas().Children().Clear();
    m_thumbnails.clear();

    const int count = WantedThumbnailCount();
    if (!m_session || count <= 0) co_return;
    const ::regif::MediaInfo info = m_session->current().info;
    const fs::path file = m_session->current().file;
    m_thumbnails.assign(static_cast<std::size_t>(count), Image{ nullptr });

    // Big enough to cover a slot (the stills are UniformToFill) at the display's scale.
    const double slot = (TimelineSurface().ActualWidth() - 2 * kHandleWidth) / count;
    const double cover = std::max(slot / info.width, kStripHeight / info.height);
    const double raster = TimelineSurface().XamlRoot() ? TimelineSurface().XamlRoot().RasterizationScale() : 1.0;
    const int maxDimension = static_cast<int>(std::ceil(std::max(info.width, info.height) * cover * raster));
    const std::vector<double> times = ::regif::thumbnailTimes(info.durationSec, count);

    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_thumbnailCancel = cancel;
    auto lifetime = get_strong();
    auto queue = DispatcherQueue();
    auto weak = get_weak();

    co_await winrt::resume_background();
    try {
        ::regif::FrameGrabber grabber(file);
        grabber.grabSequence(times, maxDimension, [&](std::size_t index, ::regif::VideoFrameBgra&& frame) {
            if (cancel->load()) return false;
            auto still = std::make_shared<::regif::VideoFrameBgra>(std::move(frame));
            queue.TryEnqueue([weak, cancel, index, count, still] {
                auto self = weak.get();
                if (self && !cancel->load() && !self->m_closed) self->AddThumbnail(index, count, *still);
            });
            return !cancel->load();
        });
    } catch (...) {
        // The filmstrip is decoration; the timeline works without it.
    }
}

void MainWindow::AddThumbnail(std::size_t index, int count, const ::regif::VideoFrameBgra& frame)
{
    if (count != static_cast<int>(m_thumbnails.size()) || index >= m_thumbnails.size() || frame.width <= 0 ||
        frame.height <= 0)
        return;
    imaging::WriteableBitmap bitmap(frame.width, frame.height);
    auto buffer = bitmap.PixelBuffer();
    std::memcpy(buffer.data(), frame.pixels.data(), std::min<std::size_t>(buffer.Capacity(), frame.pixels.size()));
    bitmap.Invalidate();

    Image image;
    image.Source(bitmap);
    image.Stretch(media::Stretch::UniformToFill);
    image.IsHitTestVisible(false);
    FilmstripCanvas().Children().Append(image);
    m_thumbnails[index] = image;
    LayoutTimeline();
}

void MainWindow::LayoutTimeline()
{
    TimelineSurface().Height(TimelineHeight());
    const double strip = std::max(0.0, TimelineSurface().ActualWidth() - 2 * kHandleWidth);
    const double duration = TimelineDuration();
    const bool active = m_session && duration > 0.0 && strip > 0.0;
    LayoutLanes();

    Place(FilmstripBackground(), kHandleWidth, kStripTop, strip, kStripHeight);
    Place(FilmstripCanvas(), kHandleWidth, kStripTop, strip, kStripHeight);
    media::RectangleGeometry clip;
    clip.Rect({ 0, 0, static_cast<float>(strip), static_cast<float>(kStripHeight) });
    FilmstripCanvas().Clip(clip);
    if (!m_thumbnails.empty()) {
        const double slot = strip / static_cast<double>(m_thumbnails.size());
        for (std::size_t i = 0; i < m_thumbnails.size(); ++i) {
            // One pixel of overlap hides seams from rounding.
            if (m_thumbnails[i]) Place(m_thumbnails[i], static_cast<double>(i) * slot, 0, std::ceil(slot) + 1, kStripHeight);
        }
    }

    for (UIElement e : { TrimShadeStart().as<UIElement>(), TrimShadeEnd().as<UIElement>(), TrimFrame().as<UIElement>(),
                         TrimHandleStart().as<UIElement>(), TrimHandleEnd().as<UIElement>(), Playhead().as<UIElement>() })
        e.Visibility(VisibleIf(active));
    if (!active) return;

    const double start = std::clamp(Value(TrimStart(), 0.0), 0.0, duration);
    const double end = std::clamp(Value(TrimEnd(), duration), start, duration);
    const double xs = TimelineX(start), xe = TimelineX(end);
    Place(TrimShadeStart(), kHandleWidth, kStripTop, xs - kHandleWidth, kStripHeight);
    Place(TrimShadeEnd(), xe, kStripTop, kHandleWidth + strip - xe, kStripHeight);
    Place(TrimFrame(), xs, kStripTop - 3, xe - xs, kStripHeight + 6);
    Place(TrimHandleStart(), xs - kHandleWidth, kStripTop - 3, kHandleWidth, kStripHeight + 6);
    Place(TrimHandleEnd(), xe, kStripTop - 3, kHandleWidth, kStripHeight + 6);
    Place(Playhead(), TimelineX(m_playheadSec) - 1.0, 0, 2, TimelineHeight());
}

void MainWindow::SetTrimPoint(bool start, double seconds)
{
    const double duration = TimelineDuration();
    const double step = FrameStep();
    const double trimStart = Value(TrimStart(), 0.0);
    const double trimEnd = Value(TrimEnd(), duration);
    if (start) {
        seconds = std::clamp(seconds, 0.0, std::max(0.0, trimEnd - step));
        TrimStart().Value(seconds);
    } else {
        seconds = std::clamp(seconds, std::min(duration, trimStart + step), duration);
        TrimEnd().Value(seconds);
    }
    TrimExpander().IsExpanded(true); // so the Trim button is at hand
    SetPlayhead(seconds);            // show the frame at the trim point
}

void MainWindow::OnTrimValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&)
{
    LayoutTimeline();
}

void MainWindow::OnTimelineSizeChanged(IInspectable const&, SizeChangedEventArgs const&)
{
    LayoutTimeline();
    if (WantedThumbnailCount() != static_cast<int>(m_thumbnails.size())) {
        m_thumbnailTimer.Stop();
        m_thumbnailTimer.Start();
    }
}

void MainWindow::OnTimelinePointerPressed(IInspectable const&, xinput::PointerRoutedEventArgs const& args)
{
    if (!m_session || m_busy || TimelineDuration() <= 0.0) return;
    TimelineSurface().Focus(FocusState::Pointer);
    const auto point = args.GetCurrentPoint(TimelineSurface()).Position();
    const double x = point.X;

    if (const int lane = LaneAt(point.Y); lane >= 0) {
        const ::regif::TextLayer layer = m_session->textLayer();
        const ::regif::TextTrack& track = layer.tracks[static_cast<std::size_t>(lane)];
        m_selectedTrack = track.id;
        m_timelineGrabOffset = x;
        m_timelineDrag = TimelineDrag::Seek;
        for (auto it = track.clips.rbegin(); it != track.clips.rend(); ++it) { // the last one draws on top
            const double cs = TimelineX(it->startSec), ce = TimelineX(it->endSec);
            if (x < cs - 4 || x > ce + 4) continue;
            const double edge = std::min(6.0, (ce - cs) / 3);
            m_timelineDrag = x <= cs + edge ? TimelineDrag::ClipStart
                           : x >= ce - edge ? TimelineDrag::ClipEnd
                                            : TimelineDrag::ClipMove;
            m_clipDragStart = *it;
            SelectClip(it->id);
            if (!::regif::isVisibleAt(*it, m_playheadSec)) SetPlayhead(it->startSec);
            break;
        }
        if (m_timelineDrag == TimelineDrag::Seek) {
            SelectClip(0);
            SetPlayhead(TimelineSeconds(x));
        }
        TimelineSurface().CapturePointer(args.Pointer());
        args.Handled(true);
        return;
    }

    const double xs = TimelineX(Value(TrimStart(), 0.0));
    const double xe = TimelineX(Value(TrimEnd(), TimelineDuration()));
    const bool onStart = x >= xs - kHandleWidth - 2 && x <= xs + 3;
    const bool onEnd = x >= xe - 3 && x <= xe + kHandleWidth + 2;

    if (onStart && (!onEnd || x <= (xs + xe) / 2)) {
        m_timelineDrag = TimelineDrag::TrimStart;
        m_timelineGrabOffset = x - xs;
    } else if (onEnd) {
        m_timelineDrag = TimelineDrag::TrimEnd;
        m_timelineGrabOffset = x - xe;
    } else {
        m_timelineDrag = TimelineDrag::Seek;
        SetPlayhead(TimelineSeconds(x));
    }
    TimelineSurface().CapturePointer(args.Pointer());
    args.Handled(true);
}

void MainWindow::OnTimelinePointerMoved(IInspectable const&, xinput::PointerRoutedEventArgs const& args)
{
    if (!m_session) return;
    const auto point = args.GetCurrentPoint(TimelineSurface()).Position();
    const double x = point.X;
    const double duration = TimelineDuration();
    const double strip = TimelineSurface().ActualWidth() - 2 * kHandleWidth;
    const double shift = strip > 0.0 ? (x - m_timelineGrabOffset) / strip * duration : 0.0; // for clip drags
    const ::regif::TextClip& from = m_clipDragStart;
    switch (m_timelineDrag) {
    case TimelineDrag::None: {
        bool onHandle = false;
        if (const int lane = LaneAt(point.Y); lane >= 0) {
            const ::regif::TextLayer layer = m_session->textLayer();
            for (const auto& clip : layer.tracks[static_cast<std::size_t>(lane)].clips) {
                const double cs = TimelineX(clip.startSec), ce = TimelineX(clip.endSec);
                const double edge = std::min(6.0, (ce - cs) / 3);
                onHandle |= (x >= cs - 4 && x <= cs + edge) || (x >= ce - edge && x <= ce + 4);
            }
        } else {
            const double xs = TimelineX(Value(TrimStart(), 0.0));
            const double xe = TimelineX(Value(TrimEnd(), duration));
            onHandle = (x >= xs - kHandleWidth - 2 && x <= xs + 3) || (x >= xe - 3 && x <= xe + kHandleWidth + 2);
        }
        TimelineSurface().SetCursor(onHandle && !m_busy ? InputSystemCursorShape::SizeWestEast
                                                        : InputSystemCursorShape::Arrow);
        return;
    }
    case TimelineDrag::ClipMove: {
        const double length = from.endSec - from.startSec;
        const double start = std::clamp(from.startSec + shift, 0.0, std::max(0.0, duration - length));
        EditSelectedClip([&](::regif::TextClip& clip) {
            clip.startSec = start;
            clip.endSec = start + length;
        });
        SetPlayhead(start);
        break;
    }
    case TimelineDrag::ClipStart: {
        const double start = std::clamp(from.startSec + shift, 0.0, std::max(0.0, from.endSec - FrameStep()));
        EditSelectedClip([&](::regif::TextClip& clip) { clip.startSec = start; });
        SetPlayhead(start);
        break;
    }
    case TimelineDrag::ClipEnd: {
        const double end = std::clamp(from.endSec + shift, std::min(duration, from.startSec + FrameStep()), duration);
        EditSelectedClip([&](::regif::TextClip& clip) { clip.endSec = end; });
        SetPlayhead(std::max(from.startSec, end - FrameStep())); // the last frame it shows on
        break;
    }
    case TimelineDrag::Seek: SetPlayhead(TimelineSeconds(x)); break;
    case TimelineDrag::TrimStart: SetTrimPoint(true, TimelineSeconds(x - m_timelineGrabOffset)); break;
    case TimelineDrag::TrimEnd: SetTrimPoint(false, TimelineSeconds(x - m_timelineGrabOffset)); break;
    }
    args.Handled(true);
}

void MainWindow::OnTimelinePointerReleased(IInspectable const&, xinput::PointerRoutedEventArgs const& args)
{
    if (m_timelineDrag == TimelineDrag::None) return;
    m_timelineDrag = TimelineDrag::None;
    TimelineSurface().ReleasePointerCapture(args.Pointer());
    args.Handled(true);
}

void MainWindow::OnTimelinePointerCaptureLost(IInspectable const&, xinput::PointerRoutedEventArgs const&)
{
    m_timelineDrag = TimelineDrag::None;
}

void MainWindow::OnTimelineDoubleTapped(IInspectable const&, xinput::DoubleTappedRoutedEventArgs const& args)
{
    if (!m_session || m_busy) return;
    const auto point = args.GetPosition(TimelineSurface());
    const int lane = LaneAt(point.Y);
    if (lane < 0) return;
    const ::regif::TextLayer layer = m_session->textLayer();
    const ::regif::TextTrack& track = layer.tracks[static_cast<std::size_t>(lane)];
    const double t = TimelineSeconds(point.X);
    for (const auto& clip : track.clips) {
        if (::regif::isVisibleAt(clip, t)) { // on a clip: edit its text
            SelectClip(clip.id);
            TextContent().Focus(FocusState::Programmatic);
            TextContent().SelectAll();
            args.Handled(true);
            return;
        }
    }
    AddTextAt(track.id, t);
    args.Handled(true);
}

void MainWindow::OnTimelineKeyDown(IInspectable const&, xinput::KeyRoutedEventArgs const& args)
{
    if (!m_session || m_busy || TimelineDuration() <= 0.0) return;
    using winrt::Windows::System::VirtualKey;
    switch (args.Key()) {
    case VirtualKey::Left: SetPlayhead(m_playheadSec - FrameStep()); break;
    case VirtualKey::Right: SetPlayhead(m_playheadSec + FrameStep()); break;
    case VirtualKey::PageUp: SetPlayhead(m_playheadSec - 1.0); break;
    case VirtualKey::PageDown: SetPlayhead(m_playheadSec + 1.0); break;
    case VirtualKey::Home: SetPlayhead(0.0); break;
    case VirtualKey::End: SetPlayhead(TimelineDuration()); break;
    case VirtualKey::I: SetTrimPoint(true, m_playheadSec); break;
    case VirtualKey::O: SetTrimPoint(false, m_playheadSec); break;
    default: return;
    }
    args.Handled(true);
}

// ---------------------------------------------------------------------------------------
// UI state

void MainWindow::ResetInputs()
{
    if (!m_session) return;
    const auto& info = m_session->current().info;
    const double duration = std::max(0.0, info.durationSec);
    ResetCrop();
    TrimStart().Value(0);
    TrimEnd().Value(duration);
    CutStart().Value(0);
    CutEnd().Value(std::min(1.0, duration / 2));
    ConvertWidth().Value(std::min(480, info.width));
    OptimizeFps().Value(0);
    OptimizeWidth().Value(0);
}

void MainWindow::RefreshUi()
{
    m_updatingUi = true;
    const bool hasSession = m_session != nullptr;
    const bool idle = !m_busy;

    OpenButton().IsEnabled(idle);
    ExportButton().IsEnabled(idle && hasSession);
    UndoButton().IsEnabled(idle && hasSession && m_session->canUndo());
    RedoButton().IsEnabled(idle && hasSession && m_session->canRedo());
    CancelButton().Visibility(VisibleIf(m_busy));

    EmptyState().Visibility(VisibleIf(!hasSession));
    EditPanel().Visibility(VisibleIf(hasSession));
    EditPanel().IsHitTestVisible(idle);
    EditPanel().Opacity(idle ? 1.0 : 0.6);
    HistoryHeader().Visibility(VisibleIf(hasSession));
    HistoryList().IsEnabled(idle);
    ScrubPanel().Visibility(VisibleIf(hasSession));
    PreviewSurface().IsHitTestVisible(idle);

    auto items = HistoryList().Items();
    items.Clear();
    if (!hasSession) {
        StageTitle().Text(L"No file open");
        StageDetails().Text(L"");
        LayoutCropOverlay();
        LayoutTextOverlay();
        LayoutTimeline();
        m_updatingUi = false;
        return;
    }

    const auto& stages = m_session->stages();
    const std::size_t current = m_session->currentIndex();
    const auto& info = stages[current].info;
    const bool isGif = info.kind == ::regif::MediaKind::Gif;

    SpeedExpander().Visibility(VisibleIf(isGif));
    OptimizeExpander().Visibility(VisibleIf(isGif));
    ConvertExpander().Visibility(VisibleIf(!isGif));
    PlayButton().Visibility(VisibleIf(isGif));

    StageTitle().Text(winrt::hstring(m_session->originalPath().filename().wstring()));
    std::wstring details = Wide(::regif::summarize(info));
    details += std::format(L"\nStage {} of {}", current + 1, stages.size());
    if (current > 0 && !isGif) details += L"\nVideo stages are kept lossless (Matroska, FFV1).";
    StageDetails().Text(winrt::hstring(details));

    m_playheadSec = std::clamp(m_playheadSec, 0.0, std::max(0.0, info.durationSec));
    TimelineSurface().IsEnabled(info.durationSec > 0.0);
    PositionText().Text(winrt::hstring(std::format(L"{:.2f} / {:.2f} s", m_playheadSec, info.durationSec)));

    for (std::size_t i = 0; i < stages.size(); ++i)
        items.Append(winrt::box_value(winrt::hstring(std::format(L"{}. {}", i + 1, Wide(stages[i].label)))));
    HistoryList().SelectedIndex(static_cast<int32_t>(current));

    LayoutCropOverlay();
    LayoutTextOverlay();
    LayoutTimeline();
    m_updatingUi = false;
}

void MainWindow::SetBusy(bool busy, winrt::hstring const& message)
{
    m_busy = busy;
    BusyProgress().IsIndeterminate(true);
    BusyProgress().Value(0);
    BusyProgress().Visibility(VisibleIf(busy));
    if (busy) ShowStatus(InfoBarSeverity::Informational, message, L"");
    RefreshUi();
}

void MainWindow::ReportProgress(double fraction)
{
    if (!m_busy || m_closed) return;
    BusyProgress().IsIndeterminate(false);
    BusyProgress().Value(std::clamp(fraction, 0.0, 1.0) * 100.0);
}

void MainWindow::ShowStatus(InfoBarSeverity severity, winrt::hstring const& title, winrt::hstring const& message)
{
    StatusBar().Severity(severity);
    StatusBar().Title(title);
    StatusBar().Message(message);
    StatusBar().IsOpen(true);
}

void MainWindow::ShowError(winrt::hstring const& title, std::exception_ptr error)
{
    ShowStatus(InfoBarSeverity::Error, title, winrt::hstring(ErrorMessage(error)));
}

// ---------------------------------------------------------------------------------------
// Win32 interop

HWND MainWindow::WindowHandle()
{
    HWND hwnd = nullptr;
    auto native = this->m_inner.as<::IWindowNative>();
    winrt::check_hresult(native->get_WindowHandle(&hwnd));
    return hwnd;
}

void MainWindow::InitializeWithWindow(IInspectable const& picker)
{
    // WinRT pickers in a desktop app need an owner window.
    auto initialize = picker.as<::IInitializeWithWindow>();
    winrt::check_hresult(initialize->Initialize(WindowHandle()));
}

} // namespace winrt::Regif::implementation
