#pragma once

#include "InteractiveSurface.g.h"

namespace winrt::Regif::implementation {

struct InteractiveSurface : InteractiveSurfaceT<InteractiveSurface> {
    InteractiveSurface() = default;

    void SetCursor(Microsoft::UI::Input::InputSystemCursorShape shape);

private:
    std::optional<Microsoft::UI::Input::InputSystemCursorShape> m_shape;
};

} // namespace winrt::Regif::implementation

namespace winrt::Regif::factory_implementation {

struct InteractiveSurface : InteractiveSurfaceT<InteractiveSurface, implementation::InteractiveSurface> {};

} // namespace winrt::Regif::factory_implementation
