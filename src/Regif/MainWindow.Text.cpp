// Text overlays: the Text panel, the FFmpeg-drawn text on the preview, and the text tracks on
// the timeline. Clips are always read from the session (in the current stage's coordinates);
// m_textVisuals only caches each clip's rendering.

#include "pch.h"

#include "MainWindow.xaml.h"
#include "FontCatalog.h"
#include "UiHelpers.h"

#include <array>
#include <cstring>
#include <format>
#include <set>
#include <thread>
#include <tuple>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using winrt::Windows::Foundation::IInspectable;

namespace imaging = winrt::Microsoft::UI::Xaml::Media::Imaging;
namespace media = winrt::Microsoft::UI::Xaml::Media;
namespace shapes = winrt::Microsoft::UI::Xaml::Shapes;

namespace winrt::Regif::implementation {
namespace {

winrt::Windows::UI::Color ToColor(const ::regif::Rgba& c)
{
    return { c.a, c.r, c.g, c.b };
}

::regif::Rgba ToRgba(const winrt::Windows::UI::Color& c)
{
    return { c.R, c.G, c.B, c.A };
}

// TextBox uses '\r' for line breaks; the text layer uses '\n'.
std::string FromTextBox(winrt::hstring const& text)
{
    std::string utf8 = winrt::to_string(text);
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        if (utf8[i] == '\r') {
            out += '\n';
            if (i + 1 < utf8.size() && utf8[i + 1] == '\n') ++i;
        } else {
            out += utf8[i];
        }
    }
    return out;
}

winrt::hstring ToTextBox(const std::string& text)
{
    std::string out = text;
    std::replace(out.begin(), out.end(), '\n', '\r');
    return winrt::to_hstring(out);
}

std::string FirstLine(const std::string& text)
{
    const auto end = text.find('\n');
    return end == std::string::npos ? text : text.substr(0, end) + " \xE2\x80\xA6";
}

std::string FontFile(const std::string& family, bool bold, bool italic)
{
    return winrt::to_string(FontCatalog::instance().file(winrt::to_hstring(family), bold, italic));
}

imaging::WriteableBitmap ToBitmap(const ::regif::VideoFrameBgra& frame)
{
    imaging::WriteableBitmap bitmap(frame.width, frame.height);
    auto buffer = bitmap.PixelBuffer();
    std::memcpy(buffer.data(), frame.pixels.data(), std::min<std::size_t>(buffer.Capacity(), frame.pixels.size()));
    bitmap.Invalidate();
    return bitmap;
}

} // namespace

void MainWindow::InitializeText()
{
    // Enumerating fonts takes a moment; do it off the UI thread before the panel needs it.
    std::thread([] { FontCatalog::instance(); }).detach();

    m_textSelection = shapes::Rectangle();
    m_textSelection.Stroke(TrimHandleStart().Background()); // the accent brush
    m_textSelection.StrokeThickness(2);
    media::DoubleCollection dashes;
    dashes.Append(3.0);
    dashes.Append(2.0);
    m_textSelection.StrokeDashArray(dashes);
    m_textSelection.IsHitTestVisible(false);
}

::regif::TextStyle MainWindow::DefaultTextStyle()
{
    if (auto selected = SelectedClip()) return selected->style; // new text copies the selected text's style

    ::regif::TextStyle style;
    const auto& info = m_session->current().info;
    style.fontFamily = winrt::to_string(FontCatalog::instance().fallback(L"Segoe UI"));
    style.bold = true;
    style.fontFile = FontFile(style.fontFamily, style.bold, style.italic);
    style.fontSize = std::max(12.0, std::round(info.height * 0.08));
    style.strokeWidth = std::max(1.0, std::round(style.fontSize / 16));
    style.boxPadding = std::max(2.0, std::round(style.fontSize / 4));
    style.shadowX = style.shadowY = std::max(1.0, std::round(style.fontSize / 20));
    return style;
}

std::optional<::regif::TextClip> MainWindow::SelectedClip()
{
    if (!m_session || m_selectedClip == 0) return std::nullopt;
    const ::regif::TextLayer layer = m_session->textLayer();
    if (const ::regif::TextClip* clip = layer.findClip(m_selectedClip)) return *clip;
    return std::nullopt;
}

void MainWindow::SelectClip(std::uint64_t clipId)
{
    m_selectedClip = clipId;
    if (clipId != 0 && m_session) {
        for (const auto& track : m_session->textLayer().tracks)
            for (const auto& clip : track.clips)
                if (clip.id == clipId) m_selectedTrack = track.id;
        TextExpander().IsExpanded(true);
    }
    RefreshTextPanel();
    LayoutTextOverlay();
    LayoutLanes();
}

void MainWindow::EditSelectedClip(const std::function<void(::regif::TextClip&)>& edit)
{
    auto clip = SelectedClip();
    if (!clip || m_busy) return;
    edit(*clip);
    m_session->updateTextClip(*clip);

    if (!m_updatingText) { // keep the panel's times in step with timeline drags
        m_updatingText = true;
        TextStart().Value(clip->startSec);
        TextEnd().Value(clip->endSec);
        m_updatingText = false;
    }
    RefreshTextVisuals();
    LayoutTextOverlay();
    LayoutLanes();
}

void MainWindow::AddTextAt(std::uint64_t trackId, double seconds)
{
    if (!m_session || m_busy) return;
    const auto& info = m_session->current().info;
    const double duration = TimelineDuration();
    if (duration <= 0.0) {
        ShowStatus(InfoBarSeverity::Warning, L"Can't add text", L"This file's duration is unknown.");
        return;
    }
    const auto layer = m_session->textLayer();
    const bool knownTrack = std::any_of(layer.tracks.begin(), layer.tracks.end(),
                                        [&](const ::regif::TextTrack& t) { return t.id == trackId; });
    if (!knownTrack) trackId = m_session->addTextTrack(std::format("Track {}", layer.tracks.size() + 1));

    const auto selected = SelectedClip();
    ::regif::TextClip clip;
    clip.text = "Text";
    clip.style = DefaultTextStyle();
    const double length = std::min(2.0, duration);
    clip.startSec = std::clamp(seconds, 0.0, std::max(0.0, duration - length));
    clip.endSec = std::min(duration, clip.startSec + length);
    // Where the selected text is (subtitles stay in place), else centred low in the frame.
    clip.x = selected ? selected->x : info.width / 2.0;
    clip.y = selected ? selected->y : std::round(info.height * 0.78);
    if (!selected) clip.style.align = ::regif::TextAlign::Center;

    const std::uint64_t id = m_session->addTextClip(trackId, clip);
    m_selectedTrack = trackId;
    if (!::regif::isVisibleAt(clip, m_playheadSec)) SetPlayhead(clip.startSec);
    RefreshTextVisuals();
    SelectClip(id);
    LayoutTimeline(); // a new track makes it taller
    TextContent().Focus(FocusState::Programmatic);
    TextContent().SelectAll();
}

// ---------------------------------------------------------------------------------------
// Panel

void MainWindow::RefreshTextPanel()
{
    m_updatingText = true;
    auto items = TextTrackBox().Items();
    items.Clear();
    int selectedIndex = -1;
    ::regif::TextLayer layer;
    if (m_session) layer = m_session->textLayer();
    for (std::size_t i = 0; i < layer.tracks.size(); ++i) {
        items.Append(winrt::box_value(winrt::to_hstring(layer.tracks[i].name)));
        if (layer.tracks[i].id == m_selectedTrack) selectedIndex = static_cast<int>(i);
    }
    if (selectedIndex < 0 && !layer.tracks.empty()) {
        selectedIndex = static_cast<int>(layer.tracks.size()) - 1;
        m_selectedTrack = layer.tracks.back().id;
    }
    if (selectedIndex < 0) m_selectedTrack = 0;
    TextTrackBox().SelectedIndex(selectedIndex);
    RemoveTrackButton().IsEnabled(selectedIndex >= 0);

    const ::regif::TextClip* clip = m_selectedClip ? layer.findClip(m_selectedClip) : nullptr;
    if (!clip) m_selectedClip = 0; // gone at this stage (e.g. trimmed away)
    TextClipPanel().Visibility(VisibleIf(clip != nullptr));
    if (!clip) {
        m_updatingText = false;
        return;
    }

    const auto& s = clip->style;
    if (FromTextBox(TextContent().Text()) != clip->text) TextContent().Text(ToTextBox(clip->text));
    TextStart().Value(clip->startSec);
    TextEnd().Value(clip->endSec);

    const auto& families = FontCatalog::instance().families();
    if (TextFont().Items().Size() == 0)
        for (const auto& family : families) TextFont().Items().Append(winrt::box_value(winrt::hstring(family.name)));
    const std::wstring family = winrt::to_hstring(s.fontFamily).c_str();
    const auto found = std::find_if(families.begin(), families.end(), [&](const auto& f) { return f.name == family; });
    TextFont().SelectedIndex(found == families.end() ? -1 : static_cast<int>(found - families.begin()));

    TextSize().Value(s.fontSize);
    TextBold().IsChecked(s.bold);
    TextItalic().IsChecked(s.italic);
    TextAlignLeft().IsChecked(s.align == ::regif::TextAlign::Left);
    TextAlignCenter().IsChecked(s.align == ::regif::TextAlign::Center);
    TextAlignRight().IsChecked(s.align == ::regif::TextAlign::Right);

    const std::array<std::tuple<ColorPicker, Border, ::regif::Rgba>, 4> colors{ {
        { FillPicker(), FillSwatch(), s.fill },
        { StrokePicker(), StrokeSwatch(), s.stroke },
        { BoxPicker(), BoxSwatch(), s.boxColor },
        { ShadowPicker(), ShadowSwatch(), s.shadowColor },
    } };
    for (const auto& [picker, swatch, color] : colors) {
        picker.Color(ToColor(color));
        swatch.Background(media::SolidColorBrush(ToColor(color)));
    }
    StrokeWidth().Value(s.strokeWidth);
    BoxEnabled().IsChecked(s.box);
    BoxPadding().Value(s.boxPadding);
    ShadowEnabled().IsChecked(s.shadow);
    ShadowX().Value(s.shadowX);
    ShadowY().Value(s.shadowY);
    m_updatingText = false;
}

void MainWindow::OnTextTrackChanged(IInspectable const&, SelectionChangedEventArgs const&)
{
    if (m_updatingText || !m_session) return;
    const int index = TextTrackBox().SelectedIndex();
    const auto layer = m_session->textLayer();
    if (index < 0 || static_cast<std::size_t>(index) >= layer.tracks.size()) return;
    m_selectedTrack = layer.tracks[static_cast<std::size_t>(index)].id;
    const auto& clips = layer.tracks[static_cast<std::size_t>(index)].clips;
    const bool clipOnTrack = std::any_of(clips.begin(), clips.end(), [&](const auto& c) { return c.id == m_selectedClip; });
    SelectClip(clipOnTrack ? m_selectedClip : 0);
}

void MainWindow::OnAddTrackClick(IInspectable const&, RoutedEventArgs const&)
{
    if (!m_session || m_busy) return;
    m_selectedTrack = m_session->addTextTrack(std::format("Track {}", m_session->textLayer().tracks.size() + 1));
    SelectClip(0);
    LayoutTimeline();
}

winrt::fire_and_forget MainWindow::OnRemoveTrackClick(IInspectable, RoutedEventArgs)
{
    if (!m_session || m_busy || m_selectedTrack == 0) co_return;
    auto lifetime = get_strong();
    const std::uint64_t trackId = m_selectedTrack;
    std::size_t clipCount = 0;
    for (const auto& track : m_session->textLayer().tracks)
        if (track.id == trackId) clipCount = track.clips.size();

    if (clipCount > 0) {
        ContentDialog dialog;
        dialog.XamlRoot(Content().XamlRoot());
        dialog.Title(winrt::box_value(L"Remove this track?"));
        dialog.Content(winrt::box_value(winrt::hstring(
            std::format(L"Its {} text clip{} will be removed too. This can't be undone.", clipCount, clipCount == 1 ? L"" : L"s"))));
        dialog.PrimaryButtonText(L"Remove");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Close);
        if (co_await dialog.ShowAsync() != ContentDialogResult::Primary || m_closed || !m_session) co_return;
    }
    try {
        m_session->removeTextTrack(trackId);
    } catch (std::invalid_argument const&) {
        co_return; // already gone
    }
    m_selectedTrack = 0;
    RefreshTextVisuals();
    SelectClip(0);
    LayoutTimeline();
}

void MainWindow::OnAddTextClick(IInspectable const&, RoutedEventArgs const&)
{
    AddTextAt(m_selectedTrack, m_playheadSec);
}

void MainWindow::OnDeleteTextClick(IInspectable const&, RoutedEventArgs const&)
{
    if (!m_session || m_busy || m_selectedClip == 0) return;
    try {
        m_session->removeTextClip(m_selectedClip);
    } catch (std::invalid_argument const&) {
    }
    RefreshTextVisuals();
    SelectClip(0);
}

void MainWindow::OnTextContentChanged(IInspectable const&, TextChangedEventArgs const&)
{
    if (m_updatingText) return;
    const std::string text = FromTextBox(TextContent().Text());
    EditSelectedClip([&](::regif::TextClip& clip) { clip.text = text; });
}

void MainWindow::OnTextNumberChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&)
{
    if (m_updatingText) return;
    const double duration = TimelineDuration();
    const double step = FrameStep();
    EditSelectedClip([&](::regif::TextClip& clip) {
        clip.startSec = std::clamp(Value(TextStart(), clip.startSec), 0.0, std::max(0.0, duration - step));
        clip.endSec = std::clamp(Value(TextEnd(), clip.endSec), std::min(duration, clip.startSec + step), duration);
        auto& s = clip.style;
        s.fontSize = std::max(1.0, Value(TextSize(), s.fontSize));
        s.strokeWidth = std::max(0.0, Value(StrokeWidth(), s.strokeWidth));
        s.boxPadding = std::max(0.0, Value(BoxPadding(), s.boxPadding));
        s.shadowX = Value(ShadowX(), s.shadowX);
        s.shadowY = Value(ShadowY(), s.shadowY);
    });
}

void MainWindow::OnTextFontChanged(IInspectable const&, SelectionChangedEventArgs const&)
{
    if (m_updatingText) return;
    const int index = TextFont().SelectedIndex();
    const auto& families = FontCatalog::instance().families();
    if (index < 0 || static_cast<std::size_t>(index) >= families.size()) return;
    const std::string family = winrt::to_string(families[static_cast<std::size_t>(index)].name);
    EditSelectedClip([&](::regif::TextClip& clip) {
        clip.style.fontFamily = family;
        clip.style.fontFile = FontFile(family, clip.style.bold, clip.style.italic);
    });
}

void MainWindow::OnTextToggleClick(IInspectable const&, RoutedEventArgs const&)
{
    if (m_updatingText) return;
    const bool bold = TextBold().IsChecked() && TextBold().IsChecked().Value();
    const bool italic = TextItalic().IsChecked() && TextItalic().IsChecked().Value();
    const bool box = Checked(BoxEnabled());
    const bool shadow = Checked(ShadowEnabled());
    EditSelectedClip([&](::regif::TextClip& clip) {
        auto& s = clip.style;
        s.bold = bold;
        s.italic = italic;
        s.fontFile = FontFile(s.fontFamily, bold, italic);
        s.box = box;
        s.shadow = shadow;
    });
}

void MainWindow::OnTextAlignClick(IInspectable const& sender, RoutedEventArgs const&)
{
    if (m_updatingText) return;
    const auto align = sender == TextAlignLeft()    ? ::regif::TextAlign::Left
                       : sender == TextAlignRight() ? ::regif::TextAlign::Right
                                                    : ::regif::TextAlign::Center;
    m_updatingText = true; // behave like radio buttons
    TextAlignLeft().IsChecked(align == ::regif::TextAlign::Left);
    TextAlignCenter().IsChecked(align == ::regif::TextAlign::Center);
    TextAlignRight().IsChecked(align == ::regif::TextAlign::Right);
    m_updatingText = false;

    EditSelectedClip([&](::regif::TextClip& clip) {
        // Move the anchor so the text stays where it is on screen.
        const auto it = m_textVisuals.find(clip.id);
        if (it != m_textVisuals.end() && it->second.rendered.image.width > 0) {
            const double width = it->second.rendered.image.width;
            const double left = clip.x + it->second.rendered.offsetX;
            clip.x = align == ::regif::TextAlign::Left     ? left
                     : align == ::regif::TextAlign::Center ? left + width / 2
                                                           : left + width;
        }
        clip.style.align = align;
    });
}

void MainWindow::OnTextColorChanged(ColorPicker const& sender, ColorChangedEventArgs const& args)
{
    const auto color = args.NewColor();
    const Border swatch = sender == FillPicker()     ? FillSwatch()
                          : sender == StrokePicker() ? StrokeSwatch()
                          : sender == BoxPicker()    ? BoxSwatch()
                                                     : ShadowSwatch();
    swatch.Background(media::SolidColorBrush(color));
    if (m_updatingText) return;
    const ::regif::Rgba rgba = ToRgba(color);
    EditSelectedClip([&](::regif::TextClip& clip) {
        auto& s = clip.style;
        if (sender == FillPicker()) s.fill = rgba;
        else if (sender == StrokePicker()) s.stroke = rgba;
        else if (sender == BoxPicker()) s.boxColor = rgba;
        else s.shadowColor = rgba;
    });
}

// ---------------------------------------------------------------------------------------
// Rendering and the preview

// Starts a render for every clip whose text or style no longer matches its rendering.
void MainWindow::RefreshTextVisuals()
{
    std::set<std::uint64_t> alive;
    if (m_session) {
        for (const auto& track : m_session->textLayer().tracks) {
            for (const auto& clip : track.clips) {
                alive.insert(clip.id);
                auto [it, added] = m_textVisuals.try_emplace(clip.id);
                TextVisual& visual = it->second;
                if (added || visual.wantedText != clip.text || !(visual.wantedStyle == clip.style)) {
                    visual.wantedText = clip.text;
                    visual.wantedStyle = clip.style;
                    RenderTextVisual(clip.id);
                }
            }
        }
    }
    std::erase_if(m_textVisuals, [&](const auto& entry) { return !alive.contains(entry.first); });
}

// One render per clip at a time; edits made meanwhile are picked up when it finishes.
winrt::fire_and_forget MainWindow::RenderTextVisual(std::uint64_t clipId)
{
    auto it = m_textVisuals.find(clipId);
    if (it == m_textVisuals.end() || it->second.inFlight) co_return;
    it->second.inFlight = true;
    auto lifetime = get_strong();
    auto queue = DispatcherQueue();

    while (true) {
        ::regif::TextClip job;
        job.text = it->second.wantedText;
        job.style = it->second.wantedStyle;

        ::regif::RenderedText result;
        std::exception_ptr error;
        co_await winrt::resume_background();
        try {
            result = ::regif::renderText(job);
        } catch (...) {
            error = std::current_exception();
        }
        if (!co_await ResumeOn{ queue } || m_closed) co_return;

        it = m_textVisuals.find(clipId); // may have been deleted meanwhile
        if (it == m_textVisuals.end()) co_return;
        TextVisual& visual = it->second;
        visual.text = job.text;
        visual.style = job.style;
        visual.rendered = std::move(result);
        visual.image = nullptr; // rebuilt from the new rendering on layout
        if (error && !visual.failed && clipId == m_selectedClip) ShowError(L"Couldn't draw this text", error);
        visual.failed = error != nullptr;
        if (visual.wantedText == job.text && visual.wantedStyle == job.style) {
            visual.inFlight = false;
            break;
        }
    }
    LayoutTextOverlay();
}

void MainWindow::LayoutTextOverlay()
{
    auto children = TextCanvas().Children();
    children.Clear();
    const PreviewLayout layout = CropLayout();
    if (!m_session || !layout || m_playing) return;

    const double s = layout.scale;
    bool selectionShown = false;
    for (const auto& track : m_session->textLayer().tracks) {
        for (const auto& clip : track.clips) {
            if (!::regif::isVisibleAt(clip, m_playheadSec)) continue;
            auto it = m_textVisuals.find(clip.id);
            if (it == m_textVisuals.end() || it->second.rendered.image.width <= 0) continue;
            TextVisual& visual = it->second;
            if (!visual.image) {
                visual.image = Image();
                visual.image.Source(ToBitmap(visual.rendered.image));
                visual.image.Stretch(media::Stretch::Fill);
                visual.image.IsHitTestVisible(false);
            }
            const double left = layout.left + (clip.x + visual.rendered.offsetX) * s;
            const double top = layout.top + (clip.y + visual.rendered.offsetY) * s;
            const double width = visual.rendered.image.width * s;
            const double height = visual.rendered.image.height * s;
            Place(visual.image, left, top, width, height);
            children.Append(visual.image);
            if (clip.id == m_selectedClip) {
                Place(m_textSelection, left - 4, top - 4, width + 8, height + 8);
                selectionShown = true;
            }
        }
    }
    if (selectionShown) children.Append(m_textSelection); // on top of every text
}

std::optional<::regif::TextClip> MainWindow::TextClipAt(double x, double y)
{
    const PreviewLayout layout = CropLayout();
    if (!m_session || !layout || m_playing) return std::nullopt;
    const auto layer = m_session->textLayer();
    for (auto track = layer.tracks.rbegin(); track != layer.tracks.rend(); ++track) { // topmost first
        for (auto clip = track->clips.rbegin(); clip != track->clips.rend(); ++clip) {
            if (!::regif::isVisibleAt(*clip, m_playheadSec)) continue;
            const auto it = m_textVisuals.find(clip->id);
            if (it == m_textVisuals.end() || it->second.rendered.image.width <= 0) continue;
            const auto& r = it->second.rendered;
            const double left = layout.left + (clip->x + r.offsetX) * layout.scale;
            const double top = layout.top + (clip->y + r.offsetY) * layout.scale;
            if (x >= left - 4 && x <= left + r.image.width * layout.scale + 4 && y >= top - 4 &&
                y <= top + r.image.height * layout.scale + 4)
                return *clip;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------
// Timeline lanes

double MainWindow::TimelineHeight()
{
    const std::size_t tracks = m_session ? m_session->textLayer().tracks.size() : 0;
    return tracks == 0 ? kTimelineHeight : kLaneTop + tracks * kLaneHeight;
}

int MainWindow::LaneAt(double y)
{
    if (!m_session || y < kLaneTop) return -1;
    const auto lane = static_cast<std::size_t>((y - kLaneTop) / kLaneHeight);
    return lane < m_session->textLayer().tracks.size() ? static_cast<int>(lane) : -1;
}

void MainWindow::LayoutLanes()
{
    auto children = LaneCanvas().Children();
    children.Clear();
    const double strip = TimelineSurface().ActualWidth() - 2 * kHandleWidth;
    if (!m_session || strip <= 0.0 || TimelineDuration() <= 0.0) return;

    // Brushes from elements the XAML already styles with theme resources.
    const media::Brush accent = TrimHandleStart().Background();
    const media::Brush laneBrush = FilmstripBackground().Background();
    const media::Brush onAccent = TrimHandleStart().Child().as<shapes::Rectangle>().Fill();
    const media::Brush text = Playhead().Fill();

    const auto layer = m_session->textLayer();
    for (std::size_t i = 0; i < layer.tracks.size(); ++i) {
        const auto& track = layer.tracks[i];
        const double y = kLaneTop + i * kLaneHeight;

        Border lane;
        lane.Background(laneBrush);
        lane.CornerRadius({ 4, 4, 4, 4 });
        if (track.id == m_selectedTrack) {
            lane.BorderBrush(accent);
            lane.BorderThickness({ 1, 1, 1, 1 });
        }
        Place(lane, kHandleWidth, y, strip, kLaneClipHeight);
        children.Append(lane);

        TextBlock name;
        name.Text(winrt::to_hstring(track.name));
        name.FontSize(11);
        name.Opacity(0.6);
        name.Foreground(text);
        Canvas::SetLeft(name, kHandleWidth + 6);
        Canvas::SetTop(name, y + 4);
        children.Append(name);

        for (const auto& clip : track.clips) {
            const double xs = TimelineX(clip.startSec), xe = TimelineX(clip.endSec);
            const bool selected = clip.id == m_selectedClip;
            Border block;
            block.Background(accent);
            block.Opacity(selected ? 1.0 : 0.6);
            block.CornerRadius({ 4, 4, 4, 4 });
            block.Padding({ 6, 0, 6, 0 });
            TextBlock label;
            label.Text(winrt::to_hstring(clip.text.empty() ? std::string("(empty)") : FirstLine(clip.text)));
            label.FontSize(12);
            label.Foreground(onAccent);
            label.VerticalAlignment(VerticalAlignment::Center);
            label.TextTrimming(TextTrimming::CharacterEllipsis);
            block.Child(label);
            Place(block, xs, y, std::max(4.0, xe - xs), kLaneClipHeight);
            children.Append(block);
        }
    }
}

} // namespace winrt::Regif::implementation
