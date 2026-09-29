#pragma once

#include "MainWindow.g.h"

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

    // Preview
    void OnPlayClick(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnScrubValueChanged(winrt::Windows::Foundation::IInspectable const& sender,
                             Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
    void OnDragOver(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::DragEventArgs const& args);
    winrt::fire_and_forget OnDrop(winrt::Windows::Foundation::IInspectable sender, Microsoft::UI::Xaml::DragEventArgs args);

private:
    winrt::fire_and_forget OpenFile(std::filesystem::path path);
    winrt::fire_and_forget ApplyOperation(::regif::Operation op);
    winrt::fire_and_forget ShowCurrentStage();
    winrt::fire_and_forget RequestFrame(double seconds);

    void ShowFrame(const ::regif::VideoFrameBgra& frame);
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
};

} // namespace winrt::Regif::implementation

namespace winrt::Regif::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

} // namespace winrt::Regif::factory_implementation
