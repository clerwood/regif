#include "pch.h"

#include "MainWindow.xaml.h"
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

namespace winrt::Regif::implementation {
namespace {

// Resumes a coroutine on the UI thread. Yields false when the window is shutting down, in
// which case the caller must stop without touching XAML objects.
struct ResumeOn {
    winrt::Microsoft::UI::Dispatching::DispatcherQueue queue;
    bool enqueued = true;

    bool await_ready() const { return queue.HasThreadAccess(); }
    bool await_suspend(std::coroutine_handle<> handle)
    {
        // Once enqueued the coroutine may resume (and this awaiter vanish) at any moment.
        if (queue.TryEnqueue([handle] { handle.resume(); })) return true;
        enqueued = false;
        return false;
    }
    bool await_resume() const noexcept { return enqueued; }
};

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

std::wstring Wide(std::string_view utf8)
{
    return std::wstring(winrt::to_hstring(utf8));
}

std::wstring ErrorMessage(std::exception_ptr error)
{
    try {
        std::rethrow_exception(error);
    } catch (winrt::hresult_error const& e) {
        return std::wstring(e.message());
    } catch (std::exception const& e) {
        return Wide(e.what());
    } catch (...) {
        return L"Something unexpected went wrong.";
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

double Value(NumberBox const& box, double fallback)
{
    const double value = box.Value();
    return std::isnan(value) ? fallback : value;
}

int IntValue(NumberBox const& box, int fallback)
{
    const double value = box.Value();
    return std::isnan(value) ? fallback : static_cast<int>(std::lround(value));
}

bool Checked(CheckBox const& box)
{
    const auto value = box.IsChecked();
    return value && value.Value();
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

Visibility VisibleIf(bool condition)
{
    return condition ? Visibility::Visible : Visibility::Collapsed;
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

    Closed([this](IInspectable const&, WindowEventArgs const&) {
        m_closed = true;
        m_cancel->cancel();
        ++m_previewGeneration;
        m_grabber.reset();
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
    m_session = std::move(session); // the previous session's working folder is deleted here
    Title(winrt::hstring(path.filename().wstring() + L" \u2013 regif"));
    StatusBar().IsOpen(false);
    ScrubSlider().Value(0);
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
    std::wstring extension = stage.file.extension().wstring();
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
    SetBusy(true, L"Exporting");
    ::regif::Session* session = m_session.get();
    std::exception_ptr error;
    co_await winrt::resume_background();
    try {
        session->exportCurrent(destination);
    } catch (...) {
        error = std::current_exception();
    }
    if (!co_await ResumeOn{ queue } || m_closed) co_return;

    SetBusy(false);
    if (error) {
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
    if (!m_session) return;
    const auto& info = m_session->current().info;
    CropX().Value(0);
    CropY().Value(0);
    CropWidth().Value(info.width);
    CropHeight().Value(info.height);
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
    const double t = ScrubSlider().Value();
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
    RefreshUi();
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
        RequestFrame(ScrubSlider().Value());
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
}

void MainWindow::OnScrubValueChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& args)
{
    if (!m_session) return;
    PositionText().Text(winrt::hstring(
        std::format(L"{:.2f} / {:.2f} s", args.NewValue(), m_session->current().info.durationSec)));
    if (m_updatingUi) return;
    RequestFrame(args.NewValue());
}

// ---------------------------------------------------------------------------------------
// UI state

void MainWindow::ResetInputs()
{
    if (!m_session) return;
    const auto& info = m_session->current().info;
    const double duration = std::max(0.0, info.durationSec);
    CropX().Value(0);
    CropY().Value(0);
    CropWidth().Value(info.width);
    CropHeight().Value(info.height);
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

    auto items = HistoryList().Items();
    items.Clear();
    if (!hasSession) {
        StageTitle().Text(L"No file open");
        StageDetails().Text(L"");
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

    ScrubSlider().Maximum(std::max(0.0, info.durationSec));
    ScrubSlider().IsEnabled(info.durationSec > 0.0);
    PositionText().Text(winrt::hstring(std::format(L"{:.2f} / {:.2f} s", ScrubSlider().Value(), info.durationSec)));

    for (std::size_t i = 0; i < stages.size(); ++i)
        items.Append(winrt::box_value(winrt::hstring(std::format(L"{}. {}", i + 1, Wide(stages[i].label)))));
    HistoryList().SelectedIndex(static_cast<int32_t>(current));

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
