#include "pch.h"

#include "InteractiveSurface.h"
#if __has_include("InteractiveSurface.g.cpp")
#include "InteractiveSurface.g.cpp"
#endif

namespace winrt::Regif::implementation {

void InteractiveSurface::SetCursor(Microsoft::UI::Input::InputSystemCursorShape shape)
{
    if (m_shape == shape) return; // pointer moves call this constantly
    m_shape = shape;
    ProtectedCursor(Microsoft::UI::Input::InputSystemCursor::Create(shape));
}

} // namespace winrt::Regif::implementation
