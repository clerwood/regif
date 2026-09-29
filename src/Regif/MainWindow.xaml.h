#pragma once

#include "MainWindow.g.h"

#include <regif/EditorGeometry.h>
#include <regif/FrameGrabber.h>
#include <regif/Session.h>

namespace winrt::Regif::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow() = default;

    // XAML elements can't be touched in the constructor; set-up happens here instead.
    void InitializeComponent();

    // Command bar
    winrt::fire_and_forget OnOpenClick(winrt::Windows::Foundation::IInspectable sender, Microsoft::UI::Xaml::RoutedEventArgs args);
    winrt::fire_and_forget OnExportClick(winrt::Windows::Foundation::IInspectable sender, Microsoft::UI::Xaml::RoutedEventArgs args);
    void OnUndoClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnRedoClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnCancelClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);

    // Edit panel
    void OnCropClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnCropResetClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnTrimClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnCutClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnSpeedClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnConvertClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnOptimizeClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnUsePlayheadClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnHistorySelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                   Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);

    void OnCropValueChanged(Microsoft::UI::Xaml::Controls::NumberBox const& sender,
                            Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& args);
    void OnCropAspectChanged(winrt::Windows::Foundation::IInspectable const& sender,
                             Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
    void OnCropExpanding(Microsoft::UI::Xaml::Controls::Expander const& sender,
                         Microsoft::UI::Xaml::Controls::ExpanderExpandingEventArgs const& args);
    void OnCropCollapsed(Microsoft::UI::Xaml::Controls::Expander const& sender,
                         Microsoft::UI::Xaml::Controls::ExpanderCollapsedEventArgs const& args);
    void OnTrimValueChanged(Microsoft::UI::Xaml::Controls::NumberBox const& sender,
                            Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& args);

    // Preview and crop overlay
    void OnPlayClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnCropSurfaceSizeChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                  Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
    void OnCropPointerPressed(winrt::Windows::Foundation::IInspectable const& sender,
                              Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnCropPointerMoved(winrt::Windows::Foundation::IInspectable const& sender,
                            Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnCropPointerReleased(winrt::Windows::Foundation::IInspectable const& sender,
                               Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnCropPointerCaptureLost(winrt::Windows::Foundation::IInspectable const& sender,
                                  Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);

    // Timeline
    void OnTimelineSizeChanged(winrt::Windows::Foundation::IInspectable const& sender,
                               Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
    void OnTimelinePointerPressed(winrt::Windows::Foundation::IInspectable const& sender,
                                  Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnTimelinePointerMoved(winrt::Windows::Foundation::IInspectable const& sender,
                                Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnTimelinePointerReleased(winrt::Windows::Foundation::IInspectable const& sender,
                                   Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnTimelinePointerCaptureLost(winrt::Windows::Foundation::IInspectable const& sender,
                                      Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnTimelineKeyDown(winrt::Windows::Foundation::IInspectable const& sender,
                           Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
    void OnDragOver(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::DragEventArgs const& args);
    winrt::fire_and_forget OnDrop(winrt::Windows::Foundation::IInspectable sender, Microsoft::UI::Xaml::DragEventArgs args);

private:
    winrt::fire_and_forget OpenFile(std::filesystem::path path);
    winrt::fire_and_forget ApplyOperation(::regif::Operation op);
    winrt::fire_and_forget ShowCurrentStage();
    winrt::fire_and_forget RequestFrame(double seconds);

    winrt::fire_and_forget LoadThumbnails();

    void ShowFrame(const ::regif::VideoFrameBgra& frame);
    void SetPlayhead(double seconds, bool showFrame = true);

    // Crop overlay
    struct PreviewLayout {
        double scale = 0.0; // screen pixels per source pixel
        double left = 0.0;  // where the frame sits inside CropSurface
        double top = 0.0;
        explicit operator bool() const { return scale > 0.0; }
    };
    PreviewLayout CropLayout();
    double LockedAspect(); // width / height, 0 for Free
    void ResetCrop();
    void CreateCropShapes();
    void LayoutCropOverlay();
    void SyncCropBoxes();

    // Timeline
    enum class TimelineDrag { None, Seek, TrimStart, TrimEnd };
    double TimelineDuration();
    double TimelineX(double seconds);
    double TimelineSeconds(double x);
    double FrameStep();
    int WantedThumbnailCount();
    void AddThumbnail(std::size_t index, int count, const ::regif::VideoFrameBgra& frame);
    void LayoutTimeline();
    void SetTrimPoint(bool start, double seconds);
    void ResetInputs();
    void RefreshUi();
    void SetBusy(bool busy, winrt::hstring const& message = {});
    void ReportProgress(double fraction);
    void ShowStatus(Microsoft::UI::Xaml::Controls::InfoBarSeverity severity, winrt::hstring const& title,
                    winrt::hstring const& message);
    void ShowError(winrt::hstring const& title, std::exception_ptr error);
    void InitializeWithWindow(winrt::Windows::Foundation::IInspectable const& picker);
    HWND WindowHandle();

    std::unique_ptr<::regif::Session> m_session;
    std::shared_ptr<::regif::CancellationToken> m_cancel = std::make_shared<::regif::CancellationToken>();
    std::shared_ptr<::regif::FrameGrabber> m_grabber;

    Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap m_frameBitmap{ nullptr };
    Microsoft::UI::Xaml::Media::Imaging::BitmapImage m_gifBitmap{ nullptr };

    bool m_busy = false;
    bool m_closed = false;
    bool m_updatingUi = false;
    bool m_frameInFlight = false;
    std::optional<double> m_pendingFrameTime;
    unsigned m_previewGeneration = 0; // bumped whenever the previewed stage changes
    unsigned m_stillToken = 0;        // bumped when the GIF animation takes over from a still frame
    double m_playheadSec = 0.0;

    // Crop overlay: the rectangle in source pixels (the boxes show it rounded).
    ::regif::CropRect m_crop;
    ::regif::CropRect m_cropDragStart;
    ::regif::CropHandle m_cropDrag = ::regif::CropHandle::None;
    winrt::Windows::Foundation::Point m_cropDragOrigin{};
    bool m_cropEditing = true; // the Crop section is expanded
    bool m_syncingCrop = false;
    std::vector<Microsoft::UI::Xaml::Shapes::Rectangle> m_cropShades; // top, bottom, left, right
    Microsoft::UI::Xaml::Shapes::Rectangle m_cropBorder{ nullptr };
    std::vector<Microsoft::UI::Xaml::Shapes::Rectangle> m_cropHandles; // in kCropHandles order

    // Timeline
    TimelineDrag m_timelineDrag = TimelineDrag::None;
    double m_timelineGrabOffset = 0.0;
    std::vector<Microsoft::UI::Xaml::Controls::Image> m_thumbnails; // one slot per still, empty until decoded
    std::shared_ptr<std::atomic_bool> m_thumbnailCancel;
    Microsoft::UI::Dispatching::DispatcherQueueTimer m_thumbnailTimer{ nullptr }; // debounces resizes
};

} // namespace winrt::Regif::implementation

namespace winrt::Regif::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

} // namespace winrt::Regif::factory_implementation
