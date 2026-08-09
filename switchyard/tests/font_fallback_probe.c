#define COBJMACROS

#include <windows.h>
#include <initguid.h>
#include <dwrite.h>
#include <dwrite_2.h>
#include <stdio.h>

static unsigned int failures;

static void expect_registry_value( HKEY root, const WCHAR *key_name, const WCHAR *value_name,
                                   const WCHAR *expected )
{
    WCHAR value[LF_FACESIZE];
    HKEY hkey;
    DWORD type, size = sizeof(value);
    LONG error;

    if ((error = RegOpenKeyExW( root, key_name, 0, KEY_QUERY_VALUE, &hkey )))
    {
        fwprintf( stderr, L"could not open %ls (%ld)\n", key_name, error );
        failures++;
        return;
    }
    error = RegQueryValueExW( hkey, value_name, NULL, &type, (BYTE *)value, &size );
    if (error || type != REG_SZ || size < sizeof(WCHAR) || size > sizeof(value) ||
        size % sizeof(WCHAR) || value[size / sizeof(WCHAR) - 1] || lstrcmpW( value, expected ))
    {
        fwprintf( stderr, L"unexpected %ls replacement (%ld)\n", value_name, error );
        failures++;
    }
    RegCloseKey( hkey );
}

static unsigned int count_system_link_entries( const WCHAR *family, const WCHAR *expected )
{
    static const WCHAR system_link_key[] =
        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink";
    WCHAR value[4096 / sizeof(WCHAR)];
    HKEY hkey;
    DWORD type, size = sizeof(value);
    unsigned int count = 0;
    size_t index = 0, length, expected_length = lstrlenW( expected );
    LONG error;

    if ((error = RegOpenKeyExW( HKEY_LOCAL_MACHINE, system_link_key, 0, KEY_QUERY_VALUE, &hkey )))
    {
        fwprintf( stderr, L"could not open SystemLink (%ld)\n", error );
        failures++;
        return 0;
    }
    error = RegQueryValueExW( hkey, family, NULL, &type, (BYTE *)value, &size );
    RegCloseKey( hkey );
    if (error || type != REG_MULTI_SZ || !size || size > sizeof(value) || size % sizeof(WCHAR))
    {
        fwprintf( stderr, L"could not read SystemLink for %ls (%ld)\n", family, error );
        failures++;
        return 0;
    }

    while (index < size / sizeof(WCHAR))
    {
        for (length = index; length < size / sizeof(WCHAR) && value[length]; ++length) ;
        if (length == size / sizeof(WCHAR))
        {
            fwprintf( stderr, L"malformed SystemLink value for %ls\n", family );
            failures++;
            return 0;
        }
        if (length - index == expected_length && !memcmp( value + index, expected,
                                                           expected_length * sizeof(WCHAR) ))
            count++;
        index = length + 1;
    }
    return count;
}

static void expect_system_link( const WCHAR *family )
{
    static const WCHAR expected[] = L"NotoEmoji-Static.ttf,Noto Emoji";
    unsigned int count = count_system_link_entries( family, expected );

    if (count != 1)
    {
        fwprintf( stderr, L"SystemLink for %ls contains the emoji fallback %u times\n", family, count );
        failures++;
    }
}

static void expect_gdi_emoji_link(void)
{
    static const WCHAR face_name[] = L"Arial";
    static const MAT2 identity = { {0,1}, {0,0}, {0,0}, {0,1} };
    GLYPHMETRICS metrics = {0};
    LOGFONTW logfont = {0};
    HGDIOBJ old_font;
    HDC hdc;
    HFONT font;
    DWORD result;

    if (!(hdc = CreateCompatibleDC( NULL )))
    {
        fputs( "could not create a GDI device context\n", stderr );
        failures++;
        return;
    }
    logfont.lfHeight = -32;
    logfont.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW( logfont.lfFaceName, face_name, ARRAYSIZE(logfont.lfFaceName) );
    if (!(font = CreateFontIndirectW( &logfont )) ||
        (old_font = SelectObject( hdc, font )) == HGDI_ERROR)
    {
        fputs( "could not select the GDI base font\n", stderr );
        failures++;
        if (font) DeleteObject( font );
        DeleteDC( hdc );
        return;
    }

    /* U+2705 is absent from both Noto Sans and Symbols2.  GGO_METRICS takes
     * the same linked-font path used by the GDI text renderer when supplied
     * with an explicit identity matrix. */
    result = GetGlyphOutlineW( hdc, 0x2705, GGO_METRICS, &metrics, 0, NULL, &identity );
    if (result == GDI_ERROR || !metrics.gmBlackBoxX || !metrics.gmBlackBoxY)
    {
        fprintf( stderr, "Arial GDI fallback lacks U+2705 (%#lx, %lu x %lu)\n",
                 (unsigned long)result, (unsigned long)metrics.gmBlackBoxX,
                 (unsigned long)metrics.gmBlackBoxY );
        failures++;
    }
    SelectObject( hdc, old_font );
    DeleteObject( font );
    DeleteDC( hdc );
}

static const WCHAR *analysis_source_text;

static HRESULT WINAPI analysis_source_QueryInterface( IDWriteTextAnalysisSource *iface, REFIID riid,
                                                       void **object )
{
    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IDWriteTextAnalysisSource ))
    {
        *object = iface;
        IDWriteTextAnalysisSource_AddRef( iface );
        return S_OK;
    }
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI analysis_source_AddRef( IDWriteTextAnalysisSource *iface )
{
    (void)iface;
    return 2;
}

static ULONG WINAPI analysis_source_Release( IDWriteTextAnalysisSource *iface )
{
    (void)iface;
    return 1;
}

static HRESULT WINAPI analysis_source_GetTextAtPosition( IDWriteTextAnalysisSource *iface, UINT32 position,
                                                          const WCHAR **text, UINT32 *length )
{
    UINT32 source_length = lstrlenW( analysis_source_text );

    (void)iface;
    if (position >= source_length)
    {
        *text = NULL;
        *length = 0;
    }
    else
    {
        *text = analysis_source_text + position;
        *length = source_length - position;
    }
    return S_OK;
}

static HRESULT WINAPI analysis_source_GetTextBeforePosition( IDWriteTextAnalysisSource *iface, UINT32 position,
                                                              const WCHAR **text, UINT32 *length )
{
    (void)iface;
    (void)position;
    *text = NULL;
    *length = 0;
    return S_OK;
}

static DWRITE_READING_DIRECTION WINAPI analysis_source_GetParagraphReadingDirection(
    IDWriteTextAnalysisSource *iface )
{
    (void)iface;
    return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
}

static HRESULT WINAPI analysis_source_GetLocaleName( IDWriteTextAnalysisSource *iface, UINT32 position,
                                                      UINT32 *length, const WCHAR **locale )
{
    (void)iface;
    (void)position;
    *locale = NULL;
    *length = 0;
    return S_OK;
}

static HRESULT WINAPI analysis_source_GetNumberSubstitution( IDWriteTextAnalysisSource *iface, UINT32 position,
                                                              UINT32 *length,
                                                              IDWriteNumberSubstitution **substitution )
{
    (void)iface;
    (void)position;
    *substitution = NULL;
    *length = 0;
    return S_OK;
}

static IDWriteTextAnalysisSourceVtbl analysis_source_vtbl =
{
    analysis_source_QueryInterface,
    analysis_source_AddRef,
    analysis_source_Release,
    analysis_source_GetTextAtPosition,
    analysis_source_GetTextBeforePosition,
    analysis_source_GetParagraphReadingDirection,
    analysis_source_GetLocaleName,
    analysis_source_GetNumberSubstitution,
};

static IDWriteTextAnalysisSource analysis_source = { &analysis_source_vtbl };

static void expect_directwrite_emoji_fallback(void)
{
    static const WCHAR emoji[] = { 0xd83e, 0xdd0d, 0 };
    static const WCHAR expected_name[] = L"Noto Emoji";
    IDWriteLocalizedStrings *names = NULL;
    IDWriteFontFamily *family = NULL;
    IDWriteFontFallback *fallback = NULL;
    IDWriteFactory2 *factory2 = NULL;
    IDWriteFactory *factory = NULL;
    IDWriteFont *font = NULL;
    WCHAR name[LF_FACESIZE];
    UINT32 name_index = 0, mapped_length = 0;
    BOOL exists = FALSE, has_character;
    FLOAT scale = 0.0f;
    HRESULT hr;

    analysis_source_text = emoji;
    hr = DWriteCreateFactory( DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&factory );
    if (SUCCEEDED(hr)) hr = IDWriteFactory_QueryInterface( factory, &IID_IDWriteFactory2, (void **)&factory2 );
    if (SUCCEEDED(hr)) hr = IDWriteFactory2_GetSystemFontFallback( factory2, &fallback );
    if (SUCCEEDED(hr))
        hr = IDWriteFontFallback_MapCharacters( fallback, &analysis_source, 0, ARRAYSIZE(emoji) - 1,
                NULL, L"Arial", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, &mapped_length, &font, &scale );
    if (FAILED(hr) || !font || mapped_length != ARRAYSIZE(emoji) - 1 || scale != 1.0f)
    {
        fprintf( stderr, "DirectWrite did not map U+1F90D to a fallback (%#lx, %u, %f)\n",
                 (unsigned long)hr, (unsigned int)mapped_length, scale );
        failures++;
        goto done;
    }
    hr = IDWriteFont_GetFontFamily( font, &family );
    if (SUCCEEDED(hr)) hr = IDWriteFontFamily_GetFamilyNames( family, &names );
    if (SUCCEEDED(hr)) hr = IDWriteLocalizedStrings_FindLocaleName( names, L"en-us", &name_index, &exists );
    if (SUCCEEDED(hr) && exists)
        hr = IDWriteLocalizedStrings_GetString( names, name_index, name, ARRAYSIZE(name) );
    if (FAILED(hr) || !exists || lstrcmpW( name, expected_name ))
    {
        fprintf( stderr, "DirectWrite mapped U+1F90D to the wrong family (%#lx)\n", (unsigned long)hr );
        failures++;
    }
    else if (FAILED(IDWriteFont_HasCharacter( font, 0x1f90d, &has_character )) || !has_character)
    {
        fputs( "DirectWrite fallback face lacks U+1F90D\n", stderr );
        failures++;
    }

done:
    if (names) IDWriteLocalizedStrings_Release( names );
    if (family) IDWriteFontFamily_Release( family );
    if (font) IDWriteFont_Release( font );
    if (fallback) IDWriteFontFallback_Release( fallback );
    if (factory2) IDWriteFactory2_Release( factory2 );
    if (factory) IDWriteFactory_Release( factory );
}

static void expect_emoji_family( IDWriteFontCollection *collection )
{
    static const WCHAR name[] = L"Noto Emoji";
    static const UINT32 codepoints[] = { 0x1f600, 0x1f90d, 0x1fae0 };
    IDWriteFontFamily *family = NULL;
    IDWriteFont *font = NULL;
    IDWriteFontFace *face = NULL;
    UINT16 glyphs[ARRAYSIZE(codepoints)];
    UINT32 index, i;
    BOOL exists;
    HRESULT hr;

    hr = IDWriteFontCollection_FindFamilyName( collection, name, &index, &exists );
    if (FAILED(hr) || !exists)
    {
        fwprintf( stderr, L"font family %ls was not found (%#lx)\n", name, (unsigned long)hr );
        failures++;
        return;
    }
    hr = IDWriteFontCollection_GetFontFamily( collection, index, &family );
    if (SUCCEEDED(hr))
        hr = IDWriteFontFamily_GetFirstMatchingFont( family, DWRITE_FONT_WEIGHT_NORMAL,
                                                      DWRITE_FONT_STRETCH_NORMAL,
                                                      DWRITE_FONT_STYLE_NORMAL, &font );
    if (SUCCEEDED(hr)) hr = IDWriteFont_CreateFontFace( font, &face );
    if (FAILED(hr))
    {
        fwprintf( stderr, L"could not create %ls face (%#lx)\n", name, (unsigned long)hr );
        failures++;
        goto done;
    }
    hr = IDWriteFontFace_GetGlyphIndices( face, codepoints, ARRAYSIZE(codepoints), glyphs );
    if (FAILED(hr))
    {
        fwprintf( stderr, L"could not map %ls glyphs (%#lx)\n", name, (unsigned long)hr );
        failures++;
    }
    else for (i = 0; i < ARRAYSIZE(glyphs); ++i)
    {
        if (glyphs[i]) continue;
        fwprintf( stderr, L"font family %ls lacks U+%04lx\n", name, (unsigned long)codepoints[i] );
        failures++;
    }

done:
    if (face) IDWriteFontFace_Release( face );
    if (font) IDWriteFont_Release( font );
    if (family) IDWriteFontFamily_Release( family );
}

int main(void)
{
    static const WCHAR replacements[] = L"Software\\Wine\\Fonts\\Replacements";
    static const WCHAR * const link_families[] =
    {
        L"Noto Sans", L"Noto Sans Symbols", L"Noto Sans Symbols 2",
        L"Noto Sans CJK JP", L"Noto Sans CJK KR", L"Noto Sans CJK SC",
        L"Noto Sans CJK TC", L"Noto Sans CJK HK",
    };
    IDWriteFactory *factory = NULL;
    IDWriteFontCollection *collection = NULL;
    HRESULT hr;
    unsigned int i;

    expect_registry_value( HKEY_CURRENT_USER, replacements, L"Segoe UI Emoji", L"Noto Sans Symbols 2" );
    expect_registry_value( HKEY_CURRENT_USER, replacements, L"Segoe UI Symbol", L"Noto Sans Symbols 2" );
    for (i = 0; i < ARRAYSIZE(link_families); ++i) expect_system_link( link_families[i] );
    expect_gdi_emoji_link();

    hr = DWriteCreateFactory( DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&factory );
    if (SUCCEEDED(hr)) hr = IDWriteFactory_GetSystemFontCollection( factory, &collection, TRUE );
    if (FAILED(hr))
    {
        fprintf( stderr, "could not create the DirectWrite system collection (%#lx)\n", (unsigned long)hr );
        failures++;
    }
    else expect_emoji_family( collection );
    expect_directwrite_emoji_fallback();

    if (collection) IDWriteFontCollection_Release( collection );
    if (factory) IDWriteFactory_Release( factory );
    if (failures) return 1;
    puts( "font fallback probe passed" );
    return 0;
}
