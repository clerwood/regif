#pragma once

#include "MainWindow.g.h"

#include <regif/EditorGeometry.h>
#include <regif/FrameGrabber.h>
#include <regif/TextRenderer.h>
#include <regif/Session.h>

#include <chrono>
#include <functional>
#include <map>

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

    // Text panel (MainWindow.Text.cpp)
    void OnTextTrackChanged(winrt::Windows::Foundation::IInspectable const& sender,
                            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
    void OnAddTrackClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    winrt::fire_and_forget OnRemoveTrackClick(winrt::Windows::Foundation::IInspectable sender, Microsoft::UI::Xaml::RoutedEventArgs args);
    void OnAddTextClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnDeleteTextClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnTextContentChanged(winrt::Windows::Foundation::IInspectable const& sender,
                              Microsoft::UI::Xaml::Controls::TextChangedEventArgs const& args);
    void OnTextNumberChanged(Microsoft::UI::Xaml::Controls::NumberBox const& sender,
                             Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const& args);
    void OnTextFontChanged(winrt::Windows::Foundation::IInspectable const& sender,
                           Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
    void OnTextToggleClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnTextAlignClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnTextColorChanged(Microsoft::UI::Xaml::Controls::ColorPicker const& sender,
                            Microsoft::UI::Xaml::Controls::ColorChangedEventArgs const& args);

    // Preview: crop overlay and text
    void OnPlayClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnPreviewSurfaceSizeChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                     Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
    void OnPreviewPointerPressed(winrt::Windows::Foundation::IInspectable const& sender,
                                 Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnPreviewPointerMoved(winrt::Windows::Foundation::IInspectable const& sender,
                               Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnPreviewPointerReleased(winrt::Windows::Foundation::IInspectable const& sender,
                                  Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnPreviewPointerCaptureLost(winrt::Windows::Foundation::IInspectable const& sender,
                                     Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
    void OnPreviewDoubleTapped(winrt::Windows::Foundation::IInspectable const& sender,
                               Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const& args);

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
    void OnTimelineDoubleTapped(winrt::Windows::Foundation::IInspectable const& sender,
                                Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const& args);
    void OnRootPreviewKeyDown(winrt::Windows::Foundation::IInspectable const& sender,
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
    void SetPlayhead(double seconds, bool showFrame = true);  // a user action: stops playback
    void MovePlayhead(double seconds, bool showFrame = true); // also used by playback

    // Playback: the playhead moves in real time and frames are decoded as it goes.
    void StartPlayback();
    void StopPlayback();
    void OnPlaybackTick();
    bool IsTyping(); // keyboard focus is somewhere that takes text, so Space is a character

    // Crop overlay
    struct PreviewLayout {
        double scale = 0.0; // screen pixels per source pixel
        double left = 0.0;  // where the frame sits inside CropSurface
        double top = 0.0;
        explicit operator bool() const { return scale > 0.0; }
    };
    PreviewLayout CropLayout(); // the frame's placement inside PreviewSurface
    double LockedAspect(); // width / height, 0 for Free
    void ResetCrop();
    void CreateCropShapes();
    void LayoutCropOverlay();
    void SyncCropBoxes();

    // Text (MainWindow.Text.cpp). Clips are always read fresh from the session, in the current
    // stage's coordinates; the cache only holds FFmpeg's rendering of each clip's text and style.
    struct TextVisual {
        std::string text;                       // what `rendered` shows
        ::regif::TextStyle style;
        ::regif::RenderedText rendered;
        Microsoft::UI::Xaml::Controls::Image image{ nullptr };
        std::string wantedText;                 // what should be shown
        ::regif::TextStyle wantedStyle;
        bool inFlight = false;
        bool failed = false;
    };
    void InitializeText();
    ::regif::TextStyle DefaultTextStyle();
    std::optional<::regif::TextClip> SelectedClip();
    void SelectClip(std::uint64_t clipId);
    void EditSelectedClip(const std::function<void(::regif::TextClip&)>& edit);
    void AddTextAt(std::uint64_t trackId, double seconds);
    void RefreshTextPanel();
    void RefreshTextVisuals();
    winrt::fire_and_forget RenderTextVisual(std::uint64_t clipId);
    void LayoutTextOverlay();
    void LayoutLanes();
    void BeginInlineEdit(std::uint64_t clipId); // a caret in the text, on the preview
    void EndInlineEdit();
    void LayoutInlineEditor();
    std::optional<::regif::TextClip> TextClipAt(double x, double y); // preview surface point, topmost first
    int LaneAt(double y);
    double TimelineHeight();

    // Timeline
    enum class TimelineDrag { None, Seek, TrimStart, TrimEnd, ClipMove, ClipStart, ClipEnd };
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

    bool m_busy = false;
    bool m_closed = false;
    bool m_updatingUi = false;
    bool m_frameInFlight = false;
    std::optional<double> m_pendingFrameTime;
    unsigned m_previewGeneration = 0; // bumped whenever the previewed stage changes
    bool m_playing = false;
    double m_playbackOrigin = 0.0; // playhead position when playback started
    std::chrono::steady_clock::time_point m_playbackStartedAt;
    Microsoft::UI::Dispatching::DispatcherQueueTimer m_playbackTimer{ nullptr };
    double m_playheadSec = 0.0;

    // Crop overlay: the rectangle in source pixels (the boxes show it rounded).
    ::regif::CropRect m_crop;
    ::regif::CropRect m_cropDragStart;
    ::regif::CropHandle m_cropDrag = ::regif::CropHandle::None;
    winrt::Windows::Foundation::Point m_previewDragOrigin{}; // where a crop or text drag began
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
    ::regif::TextClip m_clipDragStart; // the dragged clip as it was when the drag began

    // Text
    std::map<std::uint64_t, TextVisual> m_textVisuals;
    std::uint64_t m_selectedTrack = 0;
    std::uint64_t m_selectedClip = 0;
    bool m_textDragging = false;
    bool m_updatingText = false; // filling the text panel from a clip
    Microsoft::UI::Xaml::Shapes::Rectangle m_textSelection{ nullptr };
    Microsoft::UI::Xaml::Controls::TextBox m_inlineEditor{ nullptr }; // made per edit, so its colours apply
    std::uint64_t m_inlineClip = 0;
};

} // namespace winrt::Regif::implementation

namespace winrt::Regif::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

} // namespace winrt::Regif::factory_implementation
