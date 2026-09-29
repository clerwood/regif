#pragma once

// Small helpers shared by the MainWindow implementation files.

#include <algorithm>
#include <cmath>
#include <coroutine>
#include <exception>
#include <string>
#include <string_view>

namespace winrt::Regif::implementation {

using winrt::Microsoft::UI::Xaml::FrameworkElement;
using winrt::Microsoft::UI::Xaml::UIElement;
using winrt::Microsoft::UI::Xaml::Visibility;
using winrt::Microsoft::UI::Xaml::Controls::Canvas;
using winrt::Microsoft::UI::Xaml::Controls::CheckBox;
using winrt::Microsoft::UI::Xaml::Controls::NumberBox;

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

inline std::wstring Wide(std::string_view utf8)
{
    return std::wstring(winrt::to_hstring(utf8));
}

inline std::wstring ErrorMessage(std::exception_ptr error)
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

inline double Value(NumberBox const& box, double fallback)
{
    const double value = box.Value();
    return std::isnan(value) ? fallback : value;
}

inline int IntValue(NumberBox const& box, int fallback)
{
    const double value = box.Value();
    return std::isnan(value) ? fallback : static_cast<int>(std::lround(value));
}

inline bool Checked(CheckBox const& box)
{
    const auto value = box.IsChecked();
    return value && value.Value();
}

inline Visibility VisibleIf(bool condition)
{
    return condition ? Visibility::Visible : Visibility::Collapsed;
}

inline void Place(UIElement const& element, double left, double top, double width, double height)
{
    Canvas::SetLeft(element, left);
    Canvas::SetTop(element, top);
    auto fe = element.as<FrameworkElement>();
    fe.Width(std::max(0.0, width));
    fe.Height(std::max(0.0, height));
}

// Timeline, in screen pixels. The filmstrip sits between the two trim handles' widths.
constexpr double kHandleWidth = 12.0;
constexpr double kStripTop = 6.0;
constexpr double kStripHeight = 56.0;
constexpr double kTimelineHeight = 68.0;
constexpr int kMaxThumbnails = 48;
constexpr double kLaneTop = kTimelineHeight + 4.0; // text tracks start below the filmstrip
constexpr double kLaneHeight = 28.0;             // per track, including the gap
constexpr double kLaneClipHeight = 24.0;

} // namespace winrt::Regif::implementation
