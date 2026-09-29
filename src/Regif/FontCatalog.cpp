#include "pch.h"

#include "FontCatalog.h"

#include <dwrite.h>

#include <algorithm>
#include <cwctype>

namespace winrt::Regif::implementation {
namespace {

std::wstring FamilyName(IDWriteFontFamily* family)
{
    winrt::com_ptr<IDWriteLocalizedStrings> names;
    if (FAILED(family->GetFamilyNames(names.put()))) return {};
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
    UINT32 length = 0;
    if (FAILED(names->GetStringLength(index, &length))) return {};
    std::wstring name(length + 1, L'\0');
    if (FAILED(names->GetString(index, name.data(), length + 1))) return {};
    name.resize(length);
    return name;
}

// The file holding a real (not simulated) face of the family, if drawtext can load it.
std::wstring FaceFile(IDWriteFontFamily* family, DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style)
{
    winrt::com_ptr<IDWriteFont> font;
    if (FAILED(family->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, style, font.put()))) return {};
    if (font->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE) return {};
    winrt::com_ptr<IDWriteFontFace> face;
    if (FAILED(font->CreateFontFace(face.put())) || face->GetIndex() != 0) return {};

    UINT32 count = 0;
    if (FAILED(face->GetFiles(&count, nullptr)) || count != 1) return {};
    winrt::com_ptr<IDWriteFontFile> file;
    if (FAILED(face->GetFiles(&count, file.put()))) return {};
    const void* key = nullptr;
    UINT32 keySize = 0;
    winrt::com_ptr<IDWriteFontFileLoader> loader;
    if (FAILED(file->GetReferenceKey(&key, &keySize)) || FAILED(file->GetLoader(loader.put()))) return {};
    auto local = loader.try_as<IDWriteLocalFontFileLoader>();
    if (!local) return {}; // not a file on disk
    UINT32 length = 0;
    if (FAILED(local->GetFilePathLengthFromKey(key, keySize, &length))) return {};
    std::wstring path(length + 1, L'\0');
    if (FAILED(local->GetFilePathFromKey(key, keySize, path.data(), length + 1))) return {};
    path.resize(length);
    return path;
}

bool LessIgnoringCase(const std::wstring& a, const std::wstring& b)
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](wchar_t x, wchar_t y) { return std::towlower(x) < std::towlower(y); });
}

} // namespace

const FontCatalog& FontCatalog::instance()
{
    static const FontCatalog catalog; // thread-safe initialisation
    return catalog;
}

FontCatalog::FontCatalog()
{
    winrt::com_ptr<IDWriteFactory> factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(factory.put()))))
        return;
    winrt::com_ptr<IDWriteFontCollection> fonts;
    if (FAILED(factory->GetSystemFontCollection(fonts.put(), FALSE))) return;

    for (UINT32 i = 0; i < fonts->GetFontFamilyCount(); ++i) {
        winrt::com_ptr<IDWriteFontFamily> family;
        if (FAILED(fonts->GetFontFamily(i, family.put()))) continue;
        Family entry;
        entry.name = FamilyName(family.get());
        entry.regular = FaceFile(family.get(), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL);
        if (entry.name.empty() || entry.regular.empty()) continue;
        entry.bold = FaceFile(family.get(), DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL);
        entry.italic = FaceFile(family.get(), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC);
        entry.boldItalic = FaceFile(family.get(), DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_ITALIC);
        m_families.push_back(std::move(entry));
    }
    std::sort(m_families.begin(), m_families.end(),
              [](const Family& a, const Family& b) { return LessIgnoringCase(a.name, b.name); });
}

const FontCatalog::Family* FontCatalog::find(std::wstring_view name) const
{
    for (const Family& family : m_families)
        if (family.name == name) return &family;
    return nullptr;
}

std::wstring FontCatalog::file(std::wstring_view name, bool bold, bool italic) const
{
    const Family* family = find(name);
    if (!family) return {};
    // Exact face, then for bold italic whichever half exists, then regular.
    const std::wstring* exact = bold ? (italic ? &family->boldItalic : &family->bold)
                                     : (italic ? &family->italic : &family->regular);
    if (!exact->empty()) return *exact;
    if (bold && italic && !family->bold.empty()) return family->bold;
    if (bold && italic && !family->italic.empty()) return family->italic;
    return family->regular;
}

std::wstring FontCatalog::fallback(std::wstring_view preferred) const
{
    for (std::wstring_view name : { preferred, std::wstring_view(L"Segoe UI"), std::wstring_view(L"Arial") })
        if (find(name)) return std::wstring(name);
    return m_families.empty() ? std::wstring() : m_families.front().name;
}

} // namespace winrt::Regif::implementation
