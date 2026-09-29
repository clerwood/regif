#pragma once

#include <string>
#include <vector>

namespace winrt::Regif::implementation {

// Installed font families and the font files FFmpeg's drawtext can load for them. drawtext
// takes a file, not a family name, and always loads the first face of a file, so families
// whose regular face isn't the first face of its own file are left out.
class FontCatalog {
public:
    struct Family {
        std::wstring name;
        std::wstring regular, bold, italic, boldItalic; // empty when the family has no such face
    };

    // Enumerates the system font collection on first use (tens of milliseconds).
    static const FontCatalog& instance();

    const std::vector<Family>& families() const { return m_families; }
    const Family* find(std::wstring_view name) const;

    // The best file for the family: the exact face when there is one, else the nearest real
    // face (FreeType can't embolden or slant). Empty for unknown families.
    std::wstring file(std::wstring_view family, bool bold, bool italic) const;

    // A family that exists, preferring `preferred`, then common UI fonts.
    std::wstring fallback(std::wstring_view preferred) const;

private:
    FontCatalog();
    std::vector<Family> m_families;
};

} // namespace winrt::Regif::implementation
