#include "Renderer.h"
#include "IconHelper.h"
#include "SettingsDialog.h"
#include <d2d1helper.h>
#include <cmath>
#include <vector>
#include <shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <shlwapi.h>
#include <commoncontrols.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

static D2D1_COLOR_F ColorF(float r, float g, float b, float a = 1.0f)
{
    return D2D1::ColorF(r, g, b, a);
}

static const D2D1_COLOR_F COL_WIDGET_BG     = ColorF(0.96f, 0.96f, 0.96f, 0.85f);
static const D2D1_COLOR_F COL_WIDGET_BORDER  = ColorF(0.0f, 0.0f, 0.0f, 0.08f);
static D2D1_COLOR_F GlassTintColor(const GroupData& group, float alpha)
{
    int rgb = group.glassColor;
    float r = (float)((rgb >> 16) & 0xFF) / 255.0f;
    float g = (float)((rgb >> 8) & 0xFF) / 255.0f;
    float b = (float)(rgb & 0xFF) / 255.0f;
    return ColorF(r, g, b, alpha);
}

static const D2D1_COLOR_F COL_WIDGET_TEXT    = ColorF(1.0f, 1.0f, 1.0f, 1.0f);
static const D2D1_COLOR_F COL_POPUP_BG       = ColorF(0.97f, 0.97f, 0.97f, 0.90f);
static const D2D1_COLOR_F COL_POPUP_BORDER   = ColorF(0.0f, 0.0f, 0.0f, 0.10f);
static const D2D1_COLOR_F COL_POPUP_TEXT     = ColorF(1.0f, 1.0f, 1.0f, 1.0f);
static const D2D1_COLOR_F COL_POPUP_SUBTEXT  = ColorF(1.0f, 1.0f, 1.0f, 0.75f);
static const D2D1_COLOR_F COL_HOVER          = ColorF(0.0f, 0.47f, 0.84f, 0.15f);
static const D2D1_COLOR_F COL_ICON_TINT      = ColorF(0.22f, 0.55f, 0.87f, 0.90f);
static const D2D1_COLOR_F COL_PLUS_TEXT      = ColorF(1.0f, 1.0f, 1.0f, 0.85f);
static const D2D1_COLOR_F COL_TEXT_SHADOW    = ColorF(0.0f, 0.0f, 0.0f, 0.45f);

bool WidgetRenderer::Initialize()
{
    m_ownerThread = GetCurrentThreadId();
    return CreateFactories();
}

void WidgetRenderer::Shutdown()
{
    CacheGuard g(this);
    ClearIconCache();
    ClearTextFormats();
    ClearBgCache();
    if (m_menuGlyphFmt) { m_menuGlyphFmt->Release(); m_menuGlyphFmt = nullptr; }
    if (m_menuChevronFmt) { m_menuChevronFmt->Release(); m_menuChevronFmt = nullptr; }
    if (m_d2dFactory)   { m_d2dFactory->Release();   m_d2dFactory = nullptr; }
    if (m_dwriteFactory) { m_dwriteFactory->Release(); m_dwriteFactory = nullptr; }
    if (m_wicFactory)    { m_wicFactory->Release();    m_wicFactory = nullptr; }
    m_ownerThread = 0;
}

WidgetRenderer::WidgetRenderer()
{
    InitializeCriticalSection(&m_cacheCs);
    m_cacheCsInit = true;
}

WidgetRenderer::~WidgetRenderer()
{
    if (m_cacheCsInit)
    {
        DeleteCriticalSection(&m_cacheCs);
        m_cacheCsInit = false;
    }
}

void WidgetRenderer::AssertUiThread() const
{
    if (m_ownerThread != 0 && m_ownerThread != GetCurrentThreadId())
        OutputDebugStringW(L"WidgetRenderer: called from non-UI thread!\n");
}

void WidgetRenderer::LockCache()
{
    AssertUiThread();
    if (m_cacheCsInit) EnterCriticalSection(&m_cacheCs);
}

void WidgetRenderer::UnlockCache()
{
    if (m_cacheCsInit) LeaveCriticalSection(&m_cacheCs);
}

bool WidgetRenderer::CreateFactories()
{
    HRESULT hr;

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2dFactory);
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), (IUnknown**)&m_dwriteFactory);
    if (FAILED(hr)) return false;

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&m_wicFactory));
    if (FAILED(hr)) return false;

    return true;
}

void WidgetRenderer::DrawRoundedRect(ID2D1RenderTarget* rt, D2D1_RECT_F rect,
    float radius, D2D1_COLOR_F fill, D2D1_COLOR_F border, float borderWidth)
{
    ID2D1RoundedRectangleGeometry* rrg = nullptr;
    m_d2dFactory->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(rect, radius, radius), &rrg);

    if (rrg)
    {
        ID2D1SolidColorBrush* fillBrush = nullptr;
        rt->CreateSolidColorBrush(fill, &fillBrush);
        if (fillBrush)
        {
            rt->FillGeometry(rrg, fillBrush);
            fillBrush->Release();
        }

        if (borderWidth > 0.0f)
        {
            ID2D1SolidColorBrush* borderBrush = nullptr;
            rt->CreateSolidColorBrush(border, &borderBrush);
            if (borderBrush)
            {
                rt->DrawGeometry(rrg, borderBrush, borderWidth);
                borderBrush->Release();
            }
        }

        rrg->Release();
    }
}

void WidgetRenderer::ClearTextFormats()
{
    CacheGuard g(this);
    for (auto& kv : m_textFormats)
    {
        if (kv.second.trim) kv.second.trim->Release();
        if (kv.second.fmt) kv.second.fmt->Release();
    }
    m_textFormats.clear();
}

const WidgetRenderer::CachedTextFormat* WidgetRenderer::GetTextFormat(
    float fontSize, bool bold, DWRITE_TEXT_ALIGNMENT align, bool ellipsis)
{
    CacheGuard g(this);
    if (!m_dwriteFactory) return nullptr;
    uint64_t key = ((uint64_t)(int)(fontSize * 16.0f) << 32) |
        ((uint64_t)(bold ? 1 : 0) << 8) |
        ((uint64_t)(int)align << 1) |
        (uint64_t)(ellipsis ? 1 : 0);

    auto it = m_textFormats.find(key);
    if (it != m_textFormats.end())
        return &it->second;

    // NOTE: семейство обязано быть текстовым: пробуем сначала
    // Segoe UI Variable Text (штатный шрифт меню Win11), затем Segoe UI.
    // L"Segoe UI Semibold" — имя начертания, а не семейства, ломает поиск.
    WCHAR locale[LOCALE_NAME_MAX_LENGTH] = L"en-US";
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);

    CachedTextFormat cf;
    HRESULT hrFmt = m_dwriteFactory->CreateTextFormat(
            L"Segoe UI Variable Text", nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSize, locale, &cf.fmt);
    if (FAILED(hrFmt) || !cf.fmt)
    {
        if (FAILED(m_dwriteFactory->CreateTextFormat(
                L"Segoe UI", nullptr,
                bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                fontSize, locale, &cf.fmt)) || !cf.fmt)
            return nullptr;
    }

    cf.fmt->SetTextAlignment(align);
    cf.fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    cf.fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    if (ellipsis)
    {
        DWRITE_TRIMMING trimming = {};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        if (FAILED(m_dwriteFactory->CreateEllipsisTrimmingSign(cf.fmt, &cf.trim)) || !cf.trim)
        {
            cf.fmt->Release();
            return nullptr;
        }
        cf.fmt->SetTrimming(&trimming, cf.trim);
    }

    auto inserted = m_textFormats.emplace(key, cf);
    return &inserted.first->second;
}

void WidgetRenderer::ClearIconCache()
{
    CacheGuard g(this);
    for (auto& kv : m_iconCache)
        if (kv.second) kv.second->Release();
    m_iconCache.clear();
}

static HICON ShellIconForPath(const std::wstring& path)
{
    if (path.empty()) return nullptr;
    // .url: IconFile дёргаем напрямую — shell по SYSICONINDEX иногда отдаёт
    // генерический глобус вместо иконки из IconFile.
    PCWSTR ext = PathFindExtensionW(path.c_str());
    if (ext && _wcsicmp(ext, L".url") == 0)
    {
        WCHAR iconFile[MAX_PATH] = {};
        GetPrivateProfileStringW(L"InternetShortcut", L"IconFile", L"",
            iconFile, MAX_PATH, path.c_str());
        if (iconFile[0])
        {
            WCHAR expanded[MAX_PATH] = {};
            if (ExpandEnvironmentStringsW(iconFile, expanded, MAX_PATH) != 0 &&
                expanded[0])
            {
                wcscpy_s(iconFile, expanded);
            }
            if (GetFileAttributesW(iconFile) != INVALID_FILE_ATTRIBUTES)
            {
                int iconIndex = (int)GetPrivateProfileIntW(
                    L"InternetShortcut", L"IconIndex", 0, path.c_str());
                HICON hDirect = nullptr;
                if (PrivateExtractIconsW(iconFile, iconIndex, 48, 48,
                        &hDirect, nullptr, 1, 0) == 1 && hDirect)
                    return hDirect;
            }
        }
    }
    // Высокое разрешение (48px) из системного image list: даунскейл до
    // 28-32px даёт чёткую картинку, апскейл из 16px — мыло.
    SHFILEINFOW sfi = {};
    if (SUCCEEDED(SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
            SHGFI_SYSICONINDEX)))
    {
        IImageList* pList = nullptr;
        if (SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&pList))) && pList)
        {
            HICON hIcon = nullptr;
            if (SUCCEEDED(pList->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hIcon)) && hIcon)
            {
                pList->Release();
                return hIcon;
            }
            pList->Release();
        }
    }
    // Фолбэк — обычная иконка
    SHFILEINFOW sfi2 = {};
    SHGetFileInfoW(path.c_str(), 0, &sfi2, sizeof(sfi2),
        SHGFI_ICON | SHGFI_LARGEICON);
    return sfi2.hIcon;
}

// По расширению: это картинка (превью вместо иконки)?
static bool IsImagePath(const std::wstring& path)
{
    if (path.empty()) return false;
    PCWSTR ext = PathFindExtensionW(path.c_str());
    if (!ext || !ext[1]) return false;
    static const wchar_t* kExts[] = { L".jpg", L".jpeg", L".jpe", L".png",
        L".gif", L".bmp", L".dib", L".webp", L".tif", L".tiff", L".ico",
        L".heic", L".heif", L".avif", nullptr };
    for (int i = 0; kExts[i]; i++)
        if (_wcsicmp(ext, kExts[i]) == 0) return true;
    return false;
}

IWICBitmap* WidgetRenderer::ConvertToCache(const std::wstring& key, IWICBitmap* src)
{
    CacheGuard g(this);
    if (!src || !m_wicFactory) return nullptr;
    IWICFormatConverter* conv = nullptr;
    m_wicFactory->CreateFormatConverter(&conv);
    if (!conv) return nullptr;
    IWICBitmap* result = nullptr;
    if (SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
    {
        UINT bw = 0, bh = 0;
        conv->GetSize(&bw, &bh);
        IWICBitmap* cached = nullptr;
        if (bw > 0 && bh > 0 && SUCCEEDED(m_wicFactory->CreateBitmap(bw, bh,
                GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &cached)))
        {
            // Копируем сконвертированные пиксели в кэшируемый битмап
            WICRect rc = { 0, 0, (INT)bw, (INT)bh };
            IWICBitmapLock* lDst = nullptr;
            if (SUCCEEDED(cached->Lock(&rc, WICBitmapLockWrite, &lDst)))
            {
                BYTE* dst = nullptr;
                UINT dstSize = 0, stride = 0;
                lDst->GetDataPointer(&dstSize, &dst);
                lDst->GetStride(&stride);
                if (FAILED(conv->CopyPixels(&rc, stride, dstSize, dst)))
                {
                    cached->Release();
                    cached = nullptr;
                }
                lDst->Release();
            }
            else
            {
                cached->Release();
                cached = nullptr;
            }
            if (cached)
            {
                if (!key.empty())
                {
                    if (m_iconCache.size() > 256)
                        ClearIconCache(); // защита от разрастания
                    m_iconCache[key] = cached; // владение у кэша
                }
                result = cached; // при пустом ключе — владение у вызывателя
            }
        }
    }
    conv->Release();
    return result;
}

ID2D1Bitmap* WidgetRenderer::TryThumbnailBitmap(ID2D1RenderTarget* rt,
    const std::wstring& cacheKey, const std::wstring& filePath)
{
    if (!rt || !m_wicFactory || filePath.empty()) return nullptr;
    if (GetFileAttributesW(filePath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return nullptr;

    IShellItemImageFactory* psif = nullptr;
    if (FAILED(SHCreateItemFromParsingName(filePath.c_str(), nullptr,
            IID_PPV_ARGS(&psif))) || !psif)
        return nullptr;

    HBITMAP hThumb = nullptr;
    SIZE thumbSize = { 64, 64 };
    HRESULT hr = psif->GetImage(thumbSize, SIIGBF_RESIZETOFIT, &hThumb);
    psif->Release();
    if (FAILED(hr) || !hThumb) return nullptr;

    IWICBitmap* src = nullptr;
    m_wicFactory->CreateBitmapFromHBITMAP(hThumb, nullptr,
        WICBitmapUsePremultipliedAlpha, &src);
    DeleteObject(hThumb);
    if (!src) return nullptr;

    IWICBitmap* wic = ConvertToCache(cacheKey, src);
    src->Release();
    if (!wic) return nullptr;

    bool ownWic = cacheKey.empty();
    ID2D1Bitmap* d2dBmp = nullptr;
    rt->CreateBitmapFromWicBitmap(wic, &d2dBmp);
    if (ownWic) wic->Release();
    return d2dBmp; // вызывающий делает Release (может быть nullptr)
}

ID2D1Bitmap* WidgetRenderer::GetIconBitmap(ID2D1RenderTarget* rt, const ShortcutInfo& si)
{
    CacheGuard g(this);
    if (!rt || !m_wicFactory) return nullptr;
    if (m_deferIcons) return nullptr; // быстрый старт: иконки догрузятся позже

    IWICBitmap* wic = nullptr;
    if (!si.lnkPath.empty())
    {
        if (m_iconCache.size() > 256)
            ClearIconCache(); // защита от разрастания
        auto it = m_iconCache.find(si.lnkPath);
        if (it != m_iconCache.end())
            wic = it->second;
    }

    if (!wic)
    {
        // Картинки: настоящее превью (как в Проводнике), а не иконка.
        // Проверяем и сам файл, и цель ярлыка (ярлык на фото).
        std::wstring imgPath;
        if (IsImagePath(si.lnkPath))
            imgPath = si.lnkPath;
        else if (IsImagePath(si.targetPath))
            imgPath = si.targetPath;
        if (!imgPath.empty())
        {
            ID2D1Bitmap* thumb = TryThumbnailBitmap(rt,
                si.lnkPath.empty() ? imgPath : si.lnkPath, imgPath);
            if (thumb) return thumb; // вызывающий делает Release
            // Нет превью — падаем ниже на обычную иконку.
        }

        // Цель-файл (exe) даёт лучшую иконку; для не-файловых целей
        // (steam://, http) берём иконку самого файла ярлыка — shell
        // резолвит .lnk/.url (включая IconFile) в правильную иконку.
        // (SYSICONINDEX по URL может вернуть мусорный генерический индекс,
        // поэтому URL-цели даже не пробуем.)
        bool targetIsFile = !si.targetPath.empty() &&
            GetFileAttributesW(si.targetPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        HICON hIcon = nullptr;
        // 1. Явная иконка из самого ярлыка (как показывает Проводник).
        // Чинит случаи вида Discord: цель — бесцветный Update.exe,
        // а логотип лежит в IconLocation ярлыка.
        if (!si.lnkPath.empty())
            hIcon = IconFromLnkLocation(si.lnkPath);
        if (!hIcon && targetIsFile) hIcon = ShellIconForPath(si.targetPath);
        if (!hIcon) hIcon = ShellIconForPath(si.lnkPath);
        if (!hIcon) return nullptr;

        IWICBitmap* wicBmp = nullptr;
        m_wicFactory->CreateBitmapFromHICON(hIcon, &wicBmp);
        DestroyIcon(hIcon);
        if (!wicBmp) return nullptr;

        wic = ConvertToCache(si.lnkPath, wicBmp);
        wicBmp->Release();
        if (!wic) return nullptr;
        bool ownWic = si.lnkPath.empty(); // некэшированный — отпустить после создания D2D
        ID2D1Bitmap* d2dBmp = nullptr;
        rt->CreateBitmapFromWicBitmap(wic, &d2dBmp);
        if (ownWic) wic->Release();
        return d2dBmp; // вызывающий делает Release (может быть nullptr)
    }

    ID2D1Bitmap* d2dBmp = nullptr;
    rt->CreateBitmapFromWicBitmap(wic, &d2dBmp);
    return d2dBmp; // вызывающий делает Release
}

void WidgetRenderer::DrawText(ID2D1RenderTarget* rt, const std::wstring& text,
    D2D1_RECT_F rect, float fontSize, D2D1_COLOR_F color, bool bold,
    DWRITE_TEXT_ALIGNMENT align, bool ellipsis)
{
    if (text.empty()) return;

    const CachedTextFormat* ctf = GetTextFormat(fontSize, bold, align, ellipsis);
    if (!ctf || !ctf->fmt) return;

    // Тень нужна только светлому тексту на стекле (виджеты/попапы).
    // Тёмному тексту меню Win11 тень не рисуем — иначе грязь под буквами.
    float lum = 0.30f * color.r + 0.59f * color.g + 0.11f * color.b;
    if (lum > 0.5f)
    {
        // Тень под текстом: белый читается и на светлом, и на тёмном стекле
        ID2D1SolidColorBrush* shadowBrush = nullptr;
        rt->CreateSolidColorBrush(COL_TEXT_SHADOW, &shadowBrush);
        if (shadowBrush)
        {
            D2D1_RECT_F shadowRect = D2D1::RectF(rect.left, rect.top + 1.0f,
                rect.right, rect.bottom + 1.0f);
            rt->DrawText(text.c_str(), (UINT32)text.size(), ctf->fmt, shadowRect,
                shadowBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            shadowBrush->Release();
        }
    }

    ID2D1SolidColorBrush* brush = nullptr;
    rt->CreateSolidColorBrush(color, &brush);
    if (brush)
    {
        rt->DrawText(text.c_str(), (UINT32)text.size(), ctf->fmt, rect, brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
        brush->Release();
    }
}

void WidgetRenderer::DrawFolderIcon(ID2D1RenderTarget* rt, float cx, float cy, float size)
{
    float half = size / 2.0f;
    float x1 = cx - half;
    float y1 = cy - half;
    float x2 = cx + half;
    float y2 = cy + half;

    ID2D1SolidColorBrush* brush = nullptr;
    rt->CreateSolidColorBrush(COL_ICON_TINT, &brush);
    if (!brush) return;

    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
        D2D1::RectF(x1, y1 + size * 0.2f, x2, y2), size * 0.08f, size * 0.08f);
    rt->FillRoundedRectangle(rr, brush);

    ID2D1SolidColorBrush* tabBrush = nullptr;
    D2D1_COLOR_F tabColor = ColorF(0.30f, 0.65f, 0.95f, 0.90f);
    rt->CreateSolidColorBrush(tabColor, &tabBrush);
    if (tabBrush)
    {
        D2D1_RECT_F tab = D2D1::RectF(x1, y1, x1 + size * 0.35f, y1 + size * 0.25f);
        rt->FillRectangle(tab, tabBrush);
        tabBrush->Release();
    }

    brush->Release();
}

// Квадрат из центра битмапа: фото-превью без растягивания
// (для квадратных иконок — весь битмап, без изменений).
static D2D1_RECT_F CenterSquareSrc(ID2D1Bitmap* bmp)
{
    D2D1_SIZE_F sz = bmp->GetSize();
    if (sz.width <= 0.0f || sz.height <= 0.0f) return D2D1::RectF(0, 0, 1, 1);
    if (sz.width > sz.height)
    {
        float x = (sz.width - sz.height) / 2.0f;
        return D2D1::RectF(x, 0, x + sz.height, sz.height);
    }
    float y = (sz.height - sz.width) / 2.0f;
    return D2D1::RectF(0, y, sz.width, y + sz.width);
}

void WidgetRenderer::DrawMiniIcons(ID2D1RenderTarget* rt, const GroupData& group,
    float cx, float cy)
{
    int count = (int)group.shortcuts.size();
    // Сетка виджета: 2x2 по умолчанию, 3x3 опционально. Иконки 22px:
    // 3*22 + 2*4 = 74px — влезает в ячейку 76px без ресайза виджета.
    int grid = group.gridSize;
    if (grid < 2) grid = 2;
    if (grid > 3) grid = 3;
    int slots = (count <= 1) ? 1 : grid * grid;
    int cols = (slots <= 1) ? 1 : grid;
    int maxIcons = min(count, slots);

    float gridW = cols * ICON_SIZE + (cols - 1) * 4.0f;
    float startX = cx - gridW / 2.0f;
    float startY = cy - (cols * ICON_SIZE + (cols - 1) * 4.0f) / 2.0f;

    for (int i = 0; i < maxIcons; i++)
    {
        int row = i / cols;
        int col = i % cols;
        float ix = startX + col * (ICON_SIZE + 4.0f);
        float iy = startY + row * (ICON_SIZE + 4.0f);

        ID2D1Bitmap* iconBmp = GetIconBitmap(rt, group.shortcuts[i]);
        if (iconBmp)
        {
            D2D1_RECT_F src = CenterSquareSrc(iconBmp);
            rt->DrawBitmap(iconBmp,
                D2D1::RectF(ix, iy, ix + (float)ICON_SIZE, iy + (float)ICON_SIZE),
                1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
            iconBmp->Release();
        }
        else
        {
            DrawFolderIcon(rt, ix + ICON_SIZE / 2.0f, iy + ICON_SIZE / 2.0f, (float)ICON_SIZE);
        }
    }
}

HBITMAP WidgetRenderer::FinishRendering(ID2D1RenderTarget* rt, IWICBitmap* wicBitmap,
    HDC hdcScreen, int width, int height)
{
    if (rt) rt->EndDraw();

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvBits = nullptr;
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) return nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);

    if (hBitmap && pvBits && wicBitmap)
    {
        IWICBitmapLock* lock = nullptr;
        WICRect rcLock = { 0, 0, width, height };
        // Неинициализированный/битый HBITMAP при неуспешном Lock —
        // удаляем и возвращаем nullptr, а не мусор вызывающему.
        if (SUCCEEDED(wicBitmap->Lock(&rcLock, WICBitmapLockRead, &lock)) && lock)
        {
            UINT cbStride = 0;
            BYTE* pbData = nullptr;
            UINT cbBufferSize = 0;
            lock->GetStride(&cbStride);
            lock->GetDataPointer(&cbBufferSize, &pbData);

            if (pbData && cbStride >= (UINT)(width * 4))
            {
                BYTE* dst = (BYTE*)pvBits;
                for (int y = 0; y < height; y++)
                {
                    memcpy(dst + y * width * 4, pbData + y * cbStride, width * 4);
                }
            }
            else
            {
                DeleteObject(hBitmap);
                hBitmap = nullptr;
            }
            lock->Release();
        }
        else
        {
            if (lock) lock->Release();
            DeleteObject(hBitmap);
            hBitmap = nullptr;
        }
    }
    else if (hBitmap)
    {
        DeleteObject(hBitmap);
        hBitmap = nullptr;
    }

    DeleteDC(hdcMem);
    return hBitmap;
}

void WidgetRenderer::ClearBgCache()
{
    CacheGuard g(this);
    for (auto& kv : m_bgCache)
        if (kv.second) kv.second->Release();
    m_bgCache.clear();
}

void WidgetRenderer::EnsureMenuGlyphFmt()
{
    CacheGuard g(this);
    if (!m_menuGlyphFmt && m_dwriteFactory)
    {
        m_dwriteFactory->CreateTextFormat(L"Segoe MDL2 Assets", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-US", &m_menuGlyphFmt);
        if (m_menuGlyphFmt)
        {
            m_menuGlyphFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_menuGlyphFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
}

void WidgetRenderer::EnsureMenuChevronFmt()
{
    CacheGuard g(this);
    if (!m_menuChevronFmt && m_dwriteFactory)
    {
        m_dwriteFactory->CreateTextFormat(L"Segoe MDL2 Assets", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-US", &m_menuChevronFmt);
        if (m_menuChevronFmt)
        {
            m_menuChevronFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_menuChevronFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
}

HBITMAP WidgetRenderer::RenderFrostedBackground(int x, int y, int w, int h, bool dark)
{
    if (!m_wicFactory || w <= 0 || h <= 0) return nullptr;
    IWICBitmap* blurBmp = GetBlurredBackground(x, y, w, h, 6, false);
    if (!blurBmp) return nullptr;

    // Растягиваем половинный блюр до полного размера.
    IWICBitmapScaler* scaler = nullptr;
    m_wicFactory->CreateBitmapScaler(&scaler);
    if (!scaler) return nullptr;
    HRESULT hr = scaler->Initialize(blurBmp, (UINT)w, (UINT)h,
        WICBitmapInterpolationModeLinear);
    if (FAILED(hr)) { scaler->Release(); return nullptr; }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hbmp || !bits) { if (hbmp) DeleteObject(hbmp); scaler->Release(); return nullptr; }

    // Та же вуаль, что у контекстного меню: тёмная #2C2C2E / светлая #F9F9F9.
    const int vr = dark ? 44 : 249;
    const int vg = dark ? 44 : 249;
    const int vb = dark ? 46 : 249;

    WICRect rcAll = { 0, 0, w, h };
    BYTE* dst = (BYTE*)bits;
    const UINT stride = (UINT)w * 4;
    hr = scaler->CopyPixels(&rcAll, stride, (UINT)h * stride, dst);
    scaler->Release();
    if (FAILED(hr)) { DeleteObject(hbmp); return nullptr; }

    // out = вуаль*0.82 + блюр*0.18 (как слои в RenderMenu).
    for (int i = 0; i < w * h; i++, dst += 4)
    {
        dst[0] = (BYTE)((vb * 82 + dst[0] * 18 + 50) / 100);
        dst[1] = (BYTE)((vg * 82 + dst[1] * 18 + 50) / 100);
        dst[2] = (BYTE)((vr * 82 + dst[2] * 18 + 50) / 100);
        dst[3] = 255;
    }
    return hbmp;
}

IWICBitmap* WidgetRenderer::GetBlurredBackground(int x, int y, int w, int h,
    int blurRadius, bool allowStale)
{
    CacheGuard g(this);
    BgKey key{ x, y, w, h, blurRadius };
    auto it = m_bgCache.find(key);
    if (it != m_bgCache.end())
        return it->second; // попадание в кэш: ховер/скролл без нового захвата

    if (allowStale && !m_bgCache.empty())
        return m_bgCache.rbegin()->second; // живой ресайз: тянем старый фон

    IWICBitmap* fresh = CaptureDesktopBlur(x, y, w, h, blurRadius);
    if (!fresh)
    {
        if (!m_bgCache.empty())
            return m_bgCache.rbegin()->second; // старый кэш лучше, чем ничего
        return nullptr;
    }
    if (m_bgCache.size() >= 16)
        ClearBgCache(); // защита от разрастания (смена экранов и т.п.)
    m_bgCache[key] = fresh;
    return fresh;
}

IWICBitmap* WidgetRenderer::CaptureDesktopBlur(int x, int y, int w, int h, int blurRadius)
{
    if (w <= 0 || h <= 0) return nullptr;
    // Захват сразу в половинном разрешении: в 4 раза меньше пикселей для
    // блюра + более гладкое стекло; при отрисовке растянется с билинейной
    // фильтрацией.
    int w2 = w / 2;
    int h2 = h / 2;
    if (w2 < 1) w2 = 1;
    if (h2 < 1) h2 = 1;

    HDC hdcScreen = GetDC(nullptr);

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hCapture = CreateCompatibleBitmap(hdcScreen, w2, h2);
    HGDIOBJ hOld = SelectObject(hdcMem, hCapture);
    // HALFTONE обязателен: без него даунскейл выкидывает пиксели и субпиксельные
    // цвета (ClearType окон позади) превращаются в фиолетовые/зелёные пятна.
    SetStretchBltMode(hdcMem, HALFTONE);
    SetBrushOrgEx(hdcMem, 0, 0, nullptr);
    if (m_captureGuard) m_captureGuard(true, x, y, w, h); // спрятать наши окна из кадра
    StretchBlt(hdcMem, 0, 0, w2, h2, hdcScreen, x, y, w, h, SRCCOPY);
    if (m_captureGuard) m_captureGuard(false, 0, 0, 0, 0);
    SelectObject(hdcMem, hOld);

    IWICBitmap* srcBitmap = nullptr;
    m_wicFactory->CreateBitmapFromHBITMAP(hCapture, nullptr, WICBitmapUsePremultipliedAlpha, &srcBitmap);
    DeleteObject(hCapture);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    if (!srcBitmap) return nullptr;

    // Работаем в половинном разрешении, радиус масштабируем соответственно
    int bw = w2, bh = h2;
    int br = blurRadius / 2;
    if (br < 1) br = 1;
    if (br > 8) br = 8;

    IWICBitmap* blurred = nullptr;
    m_wicFactory->CreateBitmap(bw, bh, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &blurred);
    if (!blurred) { srcBitmap->Release(); return nullptr; }

    IWICBitmapLock* lockSrc = nullptr;
    WICRect rcSrc = { 0, 0, bw, bh };
    srcBitmap->Lock(&rcSrc, WICBitmapLockRead, &lockSrc);

    IWICBitmapLock* lockDst = nullptr;
    blurred->Lock(&rcSrc, WICBitmapLockWrite, &lockDst);

    if (lockSrc && lockDst)
    {
        BYTE* srcData = nullptr;
        BYTE* dstData = nullptr;
        UINT srcSize = 0, dstSize = 0;
        UINT srcStride = 0, dstStride = 0;
        lockSrc->GetDataPointer(&srcSize, &srcData);
        lockDst->GetDataPointer(&dstSize, &dstData);
        lockSrc->GetStride(&srcStride);
        lockDst->GetStride(&dstStride);

        // Separable box blur: horizontal pass into dst, then vertical pass
        // back into dst via a temp row buffer. O(w*h*r) instead of O(w*h*r^2).
        int r = br;
        std::vector<BYTE> tmp((size_t)bw * bh * 4);

        for (int py = 0; py < bh; py++)
        {
            for (int px = 0; px < bw; px++)
            {
                int sumB = 0, sumG = 0, sumR = 0, count = 0;
                for (int kx = -r; kx <= r; kx++)
                {
                    int sx = px + kx;
                    if (sx < 0) sx = 0;
                    if (sx >= bw) sx = bw - 1;
                    BYTE* pixel = srcData + (size_t)py * srcStride + (size_t)sx * 4;
                    sumB += pixel[0];
                    sumG += pixel[1];
                    sumR += pixel[2];
                    count++;
                }
                BYTE* t = &tmp[((size_t)py * bw + px) * 4];
                t[0] = (BYTE)(sumB / count);
                t[1] = (BYTE)(sumG / count);
                t[2] = (BYTE)(sumR / count);
                t[3] = 255;
            }
        }
        for (int py = 0; py < bh; py++)
        {
            for (int px = 0; px < bw; px++)
            {
                int sumB = 0, sumG = 0, sumR = 0, count = 0;
                for (int ky = -r; ky <= r; ky++)
                {
                    int sy = py + ky;
                    if (sy < 0) sy = 0;
                    if (sy >= bh) sy = bh - 1;
                    BYTE* pixel = &tmp[((size_t)sy * bw + px) * 4];
                    sumB += pixel[0];
                    sumG += pixel[1];
                    sumR += pixel[2];
                    count++;
                }
                BYTE* dst = dstData + (size_t)py * dstStride + (size_t)px * 4;
                int avB = sumB / count;
                int avG = sumG / count;
                int avR = sumR / count;
                // Лёгкое обесцвечивание (~12%): гасит остатки цветного шума
                // (субпиксельные ореолы), стекло остаётся живым
                int lum = (avR * 77 + avG * 150 + avB * 29) >> 8;
                dst[0] = (BYTE)(avB + ((lum - avB) * 32 >> 8));
                dst[1] = (BYTE)(avG + ((lum - avG) * 32 >> 8));
                dst[2] = (BYTE)(avR + ((lum - avR) * 32 >> 8));
                dst[3] = 255;
            }
        }

        lockSrc->Release();
        lockDst->Release();
    }
    else
    {
        if (lockSrc) lockSrc->Release();
        if (lockDst) lockDst->Release();
    }

    srcBitmap->Release();
    return blurred;
}

RenderedBitmap WidgetRenderer::RenderWidget(const GroupData& group,
    const WidgetRenderContext& ctx, int hoverGlow, bool selected)
{
    if (!m_d2dFactory || !m_wicFactory || !m_dwriteFactory)
        return { nullptr, 0, 0 };

    int w = WidgetWidth(group);
    int h = WidgetHeight(group);

    IWICBitmap* wicBitmap = nullptr;
    m_wicFactory->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wicBitmap);
    if (!wicBitmap) return { nullptr, 0, 0 };

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ID2D1RenderTarget* rt = nullptr;
    m_d2dFactory->CreateWicBitmapRenderTarget(wicBitmap, rtProps, &rt);
    if (!rt)
    {
        wicBitmap->Release();
        return { nullptr, 0, 0 };
    }

    rt->BeginDraw();

    // ClearType subpixel AA breaks on transparent targets (color fringes),
    // grayscale is the correct mode for layered glass windows.
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    rt->Clear(D2D1::ColorF(0, 0, 0, 0));

    D2D1_RECT_F widgetRect = D2D1::RectF(0, 0, (float)w, (float)h);

    ID2D1RoundedRectangleGeometry* rrg = nullptr;
    m_d2dFactory->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(widgetRect, (float)WIDGET_RADIUS, (float)WIDGET_RADIUS), &rrg);
    ID2D1Layer* glassLayer = nullptr;
    if (rt) rt->CreateLayer(&glassLayer);
    if (rrg && glassLayer)
    {
        // Clip everything to the rounded shape so corners stay fully transparent
        rt->PushLayer(
            D2D1::LayerParameters(D2D1::InfiniteRect(), rrg,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::Matrix3x2F::Identity(),
                1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE),
            glassLayer);

        // Blurred copy of the desktop behind the widget, drawn semi-transparent
        // so the live desktop still shows through -> frosted glass.
        // Кэшируется по геометрии: повторные кадры почти бесплатны.
        IWICBitmap* blurBmp = GetBlurredBackground(ctx.widgetScreenX, ctx.widgetScreenY, w, h, 8);
        if (blurBmp)
        {
            ID2D1Bitmap* d2dBlur = nullptr;
            rt->CreateBitmapFromWicBitmap(blurBmp, &d2dBlur);
            if (d2dBlur)
            {
                rt->DrawBitmap(d2dBlur, widgetRect, 0.62f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                d2dBlur->Release();
            }
        }

        // Light veil tint — low alpha keeps the widget transparent.
        // Hover glow (animated 0..255 by the caller) brightens tint+sheen.
        float glowT = hoverGlow < 0 ? 0.0f : (hoverGlow > 255 ? 1.0f : hoverGlow / 255.0f);
        D2D1_COLOR_F glassTint = GlassTintColor(group, 0.22f + 0.08f * glowT);
        ID2D1SolidColorBrush* tintBrush = nullptr;
        rt->CreateSolidColorBrush(glassTint, &tintBrush);
        if (tintBrush)
        {
            rt->FillRectangle(widgetRect, tintBrush);
            tintBrush->Release();
        }

        // Top-down sheen: glass highlight fading out toward the middle
        ID2D1LinearGradientBrush* sheenBrush = nullptr;
        ID2D1GradientStopCollection* stops = nullptr;
        D2D1_GRADIENT_STOP gradStops[2] = {
            { 0.0f, ColorF(1.0f, 1.0f, 1.0f, 0.20f + 0.15f * glowT) },
            { 1.0f, ColorF(1.0f, 1.0f, 1.0f, 0.0f) }
        };
        if (SUCCEEDED(rt->CreateGradientStopCollection(
                gradStops, 2, &stops)))
        {
            if (SUCCEEDED(rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(0, 0),
                        D2D1::Point2F(0, (float)h * 0.55f)),
                    stops, &sheenBrush)))
            {
                rt->FillRectangle(widgetRect, sheenBrush);
                sheenBrush->Release();
            }
            stops->Release();
        }

        rt->PopLayer();
        glassLayer->Release();
        rrg->Release();
    }
    else
    {
        if (glassLayer) glassLayer->Release();
        if (rrg) rrg->Release();
    }

    int cell = WidgetCell(group);
    // Подпись живёт внутри бокса полосой снизу; мини-иконки центрируем
    // по оставшейся площади (иначе налезут на текст).
    // Счётчик переполнения — тут же в строке подписи («Game +21»):
    // отдельная пилюля только спорила за место с иконками.
    int grid = group.gridSize;
    if (grid < 2) grid = 2;
    if (grid > 3) grid = 3;
    int slots = (int)group.shortcuts.size() <= 1 ? 1 : grid * grid;
    int count = (int)group.shortcuts.size();
    bool overflow = Settings::IsShowOverflow() && group.showOverflow && count > slots;
    int labelStrip = (overflow || (!group.hideName && !group.name.empty()))
        ? WIDGET_LABEL_H : 0;
    float iconsH = (float)(cell - labelStrip);
    DrawMiniIcons(rt, group, w / 2.0f, iconsH / 2.0f);

    if (labelStrip > 0)
    {
        std::wstring label;
        if (!group.hideName && !group.name.empty())
        {
            label = group.name;
            if (label.length() > 8) label = label.substr(0, 7) + L"...";
        }
        if (overflow)
        {
            WCHAR cnt[16];
            swprintf_s(cnt, L"+%d", count - slots + 1);
            if (!label.empty()) label += L" ";
            label += cnt;
        }
        D2D1_RECT_F labelRect = D2D1::RectF(2, (float)(cell - labelStrip), (float)(w - 2), (float)cell);
        // Тень под текстом: подпись лежит на живом стекле (обои любые),
        // без тени белый текст тонет на светлом фоне.
        D2D1_RECT_F shadowRect = labelRect;
        shadowRect.left += 1.0f; shadowRect.top += 1.0f;
        shadowRect.right += 1.0f; shadowRect.bottom += 1.0f;
        DrawText(rt, label, shadowRect, 10.0f, ColorF(0, 0, 0, 0.55f),
            false, DWRITE_TEXT_ALIGNMENT_CENTER, true);
        DrawText(rt, label, labelRect, 10.0f, COL_WIDGET_TEXT,
            false, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }

    // Glass edges: thin dark outer border + bright inner highlight on top
    DrawRoundedRect(rt, widgetRect, (float)WIDGET_RADIUS, ColorF(0, 0, 0, 0), ColorF(0.0f, 0.0f, 0.0f, 0.14f), 1.0f);
    D2D1_RECT_F innerRect = D2D1::RectF(1.0f, 1.0f, (float)w - 1.0f, (float)h - 1.0f);
    DrawRoundedRect(rt, innerRect, (float)WIDGET_RADIUS - 1.0f, ColorF(0, 0, 0, 0), ColorF(1.0f, 1.0f, 1.0f, 0.45f), 1.0f);

    // Мультивыделение: акцентная рамка поверх всего (как выделение в проводнике).
    if (selected)
    {
        D2D1_RECT_F selRect = D2D1::RectF(1.5f, 1.5f, (float)w - 1.5f, (float)h - 1.5f);
        DrawRoundedRect(rt, selRect, (float)WIDGET_RADIUS - 1.0f,
            ColorF(0, 0, 0, 0), ColorF(0.12f, 0.52f, 0.95f, 0.95f), 2.0f);
    }

    HDC hdcScreen = GetDC(nullptr);
    HBITMAP hBmp = FinishRendering(rt, wicBitmap, hdcScreen, w, h);
    ReleaseDC(nullptr, hdcScreen);

    rt->Release();
    wicBitmap->Release();

    return { hBmp, w, h };
}

RenderedBitmap WidgetRenderer::RenderPopup(const GroupData& group, int hoveredIndex,
    int screenX, int screenY, int scrollY, int maxListH, int popupW, bool fastBg,
    int visListParam)
{
    if (!m_d2dFactory || !m_wicFactory || !m_dwriteFactory)
        return { nullptr, 0, 0 };

    int count = (int)group.shortcuts.size();
    int w = popupW;
    if (w < POPUP_MIN_W) w = POPUP_MIN_W;
    int headerH = POPUP_HEADER_H;
    int fullListH = count * POPUP_ITEM_H;
    // Видимая высота списка задаётся окном (авто/скролл/ручной ресайз).
    // Может превышать контент — тогда внизу пустое стекло.
    int visListH = (visListParam >= 0) ? visListParam : fullListH;
    if (maxListH >= 0 && visListH > maxListH)
        visListH = maxListH;
    if (visListH < 0) visListH = 0;
    if (scrollY < 0) scrollY = 0;
    int maxScroll = fullListH - visListH;
    if (maxScroll < 0) maxScroll = 0;
    if (scrollY > maxScroll) scrollY = maxScroll;
    int h = headerH + visListH + POPUP_PAD * 2;
    bool needScroll = fullListH > visListH;

    IWICBitmap* wicBitmap = nullptr;
    m_wicFactory->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wicBitmap);
    if (!wicBitmap) return { nullptr, 0, 0 };

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ID2D1RenderTarget* rt = nullptr;
    m_d2dFactory->CreateWicBitmapRenderTarget(wicBitmap, rtProps, &rt);
    if (!rt)
    {
        wicBitmap->Release();
        return { nullptr, 0, 0 };
    }

    rt->BeginDraw();
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));

    D2D1_RECT_F popupRect = D2D1::RectF(0, 0, (float)w, (float)h);

    {
        ID2D1RoundedRectangleGeometry* rrg = nullptr;
        m_d2dFactory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(popupRect, (float)POPUP_RADIUS, (float)POPUP_RADIUS), &rrg);
        ID2D1Layer* glassLayer = nullptr;
        if (rt) rt->CreateLayer(&glassLayer);
        if (rrg && glassLayer)
        {
            // Rounded clip keeps corners fully transparent
            rt->PushLayer(
                D2D1::LayerParameters(D2D1::InfiniteRect(), rrg,
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                    D2D1::Matrix3x2F::Identity(),
                    1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE),
                glassLayer);

            IWICBitmap* blurBmp = GetBlurredBackground(screenX, screenY, w, h, 6, fastBg);
            if (blurBmp)
            {
                ID2D1Bitmap* d2dBlur = nullptr;
                rt->CreateBitmapFromWicBitmap(blurBmp, &d2dBlur);
                if (d2dBlur)
                {
                    rt->DrawBitmap(d2dBlur, popupRect, 0.62f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    d2dBlur->Release();
                }
            }

            D2D1_COLOR_F glassTint = GlassTintColor(group, 0.25f);
            ID2D1SolidColorBrush* tintBrush = nullptr;
            rt->CreateSolidColorBrush(glassTint, &tintBrush);
            if (tintBrush)
            {
                rt->FillRectangle(popupRect, tintBrush);
                tintBrush->Release();
            }

            // Subtle top sheen for glass look
            ID2D1LinearGradientBrush* sheenBrush = nullptr;
            ID2D1GradientStopCollection* stops = nullptr;
            D2D1_GRADIENT_STOP gradStops[2] = {
                { 0.0f, ColorF(1.0f, 1.0f, 1.0f, 0.16f) },
                { 1.0f, ColorF(1.0f, 1.0f, 1.0f, 0.0f) }
            };
            if (SUCCEEDED(rt->CreateGradientStopCollection(
                    gradStops, 2, &stops)))
            {
                if (SUCCEEDED(rt->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(
                            D2D1::Point2F(0, 0),
                            D2D1::Point2F(0, (float)headerH * 2.0f)),
                        stops, &sheenBrush)))
                {
                    rt->FillRectangle(popupRect, sheenBrush);
                    sheenBrush->Release();
                }
                stops->Release();
            }

            rt->PopLayer();
            glassLayer->Release();
            rrg->Release();
        }
        else
        {
            if (glassLayer) glassLayer->Release();
            if (rrg) rrg->Release();
        }
    }

    DrawRoundedRect(rt, popupRect, (float)POPUP_RADIUS, ColorF(0, 0, 0, 0), ColorF(0.0f, 0.0f, 0.0f, 0.14f), 1.0f);
    D2D1_RECT_F popupInner = D2D1::RectF(1.0f, 1.0f, (float)w - 1.0f, (float)h - 1.0f);
    DrawRoundedRect(rt, popupInner, (float)POPUP_RADIUS - 1.0f, ColorF(0, 0, 0, 0), ColorF(1.0f, 1.0f, 1.0f, 0.45f), 1.0f);

    D2D1_RECT_F headerRect = D2D1::RectF(POPUP_PAD, 0, (float)(w - POPUP_PAD), (float)headerH);
    DrawText(rt, group.name, headerRect, 13.0f, COL_POPUP_TEXT, true,
        DWRITE_TEXT_ALIGNMENT_CENTER, true);

    float lineY = (float)headerH - 0.5f;
    ID2D1SolidColorBrush* lineBrush = nullptr;
    rt->CreateSolidColorBrush(ColorF(0, 0, 0, 0.06f), &lineBrush);
    if (lineBrush)
    {
        rt->DrawLine(D2D1::Point2F((float)POPUP_PAD, lineY),
            D2D1::Point2F((float)(w - POPUP_PAD), lineY), lineBrush, 0.5f);
        lineBrush->Release();
    }

    float listTop = (float)headerH;
    float listBottom = (float)(h - POPUP_PAD);
    float textRight = needScroll ? (float)(w - 22) : (float)(w - 12);
    rt->PushAxisAlignedClip(D2D1::RectF(0, listTop, (float)w, listBottom),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    for (int i = 0; i < count; i++)
    {
        float itemY = (float)(headerH + POPUP_PAD + i * POPUP_ITEM_H - scrollY);
        if (itemY + POPUP_ITEM_H < listTop || itemY > listBottom)
            continue; // вне видимой области
        D2D1_RECT_F itemRect = D2D1::RectF(0, itemY, (float)w, itemY + POPUP_ITEM_H);

        if (i == hoveredIndex)
        {
            ID2D1SolidColorBrush* hoverBrush = nullptr;
            rt->CreateSolidColorBrush(COL_HOVER, &hoverBrush);
            if (hoverBrush)
            {
                D2D1_ROUNDED_RECT hrr = D2D1::RoundedRect(
                    D2D1::RectF(4, itemY + 2, textRight + 8, itemY + POPUP_ITEM_H - 2),
                    6, 6);
                rt->FillRoundedRectangle(hrr, hoverBrush);
                hoverBrush->Release();
            }
        }

        float iconX = 12;
        float iconY = itemY + (POPUP_ITEM_H - 28) / 2.0f;

        ID2D1Bitmap* iconBmp = GetIconBitmap(rt, group.shortcuts[i]);
        if (iconBmp)
        {
            D2D1_RECT_F src = CenterSquareSrc(iconBmp);
            rt->DrawBitmap(iconBmp, D2D1::RectF(iconX, iconY, iconX + 28, iconY + 28),
                1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
            iconBmp->Release();
        }

        float textX = iconX + 36;
        D2D1_RECT_F nameRect = D2D1::RectF(textX, itemY + 4, textRight, itemY + POPUP_ITEM_H / 2);
        DrawText(rt, group.shortcuts[i].name, nameRect, 12.0f, COL_POPUP_TEXT,
            false, DWRITE_TEXT_ALIGNMENT_LEADING, true);

        PCWSTR fname = PathFindFileNameW(group.shortcuts[i].targetPath.c_str());
        std::wstring dir;
        if (!group.shortcuts[i].targetPath.empty())
        {
            dir = fname ? group.shortcuts[i].targetPath.substr(0,
                fname - group.shortcuts[i].targetPath.c_str()) : L"";
        }
        else
        {
            // Обычный файл/папка: цели нет — показываем расширение (.pdf)
            PCWSTR e = PathFindExtensionW(group.shortcuts[i].lnkPath.c_str());
            if (e && e[1]) dir = e;
        }
        D2D1_RECT_F pathRect = D2D1::RectF(textX, itemY + POPUP_ITEM_H / 2 + 2,
            textRight, itemY + POPUP_ITEM_H - 4);
        DrawText(rt, dir, pathRect, 9.0f, COL_POPUP_SUBTEXT,
            false, DWRITE_TEXT_ALIGNMENT_LEADING, true);

        if (i < count - 1)
        {
            ID2D1SolidColorBrush* sepBrush = nullptr;
            rt->CreateSolidColorBrush(ColorF(0, 0, 0, 0.04f), &sepBrush);
            if (sepBrush)
            {
                float sepY = itemY + POPUP_ITEM_H - 0.5f;
                rt->DrawLine(D2D1::Point2F((float)(iconX + 36), sepY),
                    D2D1::Point2F(textRight, sepY), sepBrush, 0.5f);
                sepBrush->Release();
            }
        }
    }

    rt->PopAxisAlignedClip();

    if (needScroll)
    {
        float trackX = (float)(w - 10);
        float trackY0 = listTop + 4;
        float trackY1 = listBottom - 4;
        ID2D1SolidColorBrush* trackBrush = nullptr;
        rt->CreateSolidColorBrush(ColorF(0, 0, 0, 0.10f), &trackBrush);
        if (trackBrush)
        {
            D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
                D2D1::RectF(trackX, trackY0, trackX + 5, trackY1), 2.5f, 2.5f);
            rt->FillRoundedRectangle(track, trackBrush);
            trackBrush->Release();
        }

        float trackH = trackY1 - trackY0;
        float thumbH = trackH * (float)visListH / (float)fullListH;
        if (thumbH < 24) thumbH = 24;
        if (thumbH > trackH) thumbH = trackH;
        float thumbY = trackY0;
        if (maxScroll > 0)
            thumbY += (trackH - thumbH) * (float)scrollY / (float)maxScroll;

        ID2D1SolidColorBrush* thumbBrush = nullptr;
        rt->CreateSolidColorBrush(ColorF(0, 0, 0, 0.30f), &thumbBrush);
        if (thumbBrush)
        {
            D2D1_ROUNDED_RECT thumb = D2D1::RoundedRect(
                D2D1::RectF(trackX, thumbY, trackX + 5, thumbY + thumbH), 2.5f, 2.5f);
            rt->FillRoundedRectangle(thumb, thumbBrush);
            thumbBrush->Release();
        }
    }

    HDC hdcScreen = GetDC(nullptr);
    HBITMAP hBmp = FinishRendering(rt, wicBitmap, hdcScreen, w, h);
    ReleaseDC(nullptr, hdcScreen);

    rt->Release();
    wicBitmap->Release();

    return { hBmp, w, h };
}

int WidgetRenderer::HitTestPopup(const GroupData& group, int mouseX, int mouseY, int scrollY)
{
    int headerH = POPUP_HEADER_H;
    int localY = mouseY - POPUP_PAD + scrollY;
    if (localY < headerH) return -1;

    int idx = (localY - headerH) / POPUP_ITEM_H;
    if (idx >= 0 && idx < (int)group.shortcuts.size())
        return idx;
    return -1;
}

// ---------------------------------------------------------------------------
// Modern glass menu
// ---------------------------------------------------------------------------

float WidgetRenderer::MeasureTextWidth(const std::wstring& text, float fontSize, bool bold)
{
    if (text.empty() || !m_dwriteFactory) return 0.0f;
    const CachedTextFormat* ctf = GetTextFormat(fontSize, bold,
        DWRITE_TEXT_ALIGNMENT_LEADING, false);
    if (!ctf || !ctf->fmt) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(m_dwriteFactory->CreateTextLayout(text.c_str(),
            (UINT32)text.size(), ctf->fmt, 2000.0f, 100.0f, &layout)))
        return 0.0f;
    DWRITE_TEXT_METRICS m = {};
    float w = 0.0f;
    if (SUCCEEDED(layout->GetMetrics(&m)))
        w = m.width;
    layout->Release();
    return w;
}

void WidgetRenderer::DrawMenuIcon(ID2D1RenderTarget* rt, HBITMAP hbm,
    float x, float y, float size)
{
    if (!rt || !hbm || !m_wicFactory) return;
    IWICBitmap* wic = nullptr;
    if (FAILED(m_wicFactory->CreateBitmapFromHBITMAP(hbm, nullptr,
            WICBitmapUsePremultipliedAlpha, &wic)) || !wic)
        return;
    ID2D1Bitmap* bmp = nullptr;
    rt->CreateBitmapFromWicBitmap(wic, &bmp);
    wic->Release();
    if (bmp)
    {
        rt->DrawBitmap(bmp, D2D1::RectF(x, y, x + size, y + size), 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        bmp->Release();
    }
}

SIZE WidgetRenderer::MeasureMenu(const std::vector<MenuRenderItem>& items)
{
    return MeasureMenu({}, items);
}

SIZE WidgetRenderer::MeasureMenu(const std::vector<MenuRenderItem>& commands,
    const std::vector<MenuRenderItem>& items)
{
    SIZE sz = { 180, MENU_PAD * 2 };
    if (!m_dwriteFactory) return sz;

    // Win11: пункты 14px, шапка 12px semibold, хоткеи и подписи панели 12px.
    const CachedTextFormat* ctf = GetTextFormat(14.0f, false,
        DWRITE_TEXT_ALIGNMENT_LEADING, false);
    const CachedTextFormat* ctfTitle = GetTextFormat(12.0f, true,
        DWRITE_TEXT_ALIGNMENT_LEADING, false);

    float maxW = 0;
    float maxShortW = 0;
    bool hasCheck = false;
    bool hasSwatch = false;
    bool hasIcon = false;
    bool hasArrow = false;
    bool hasShortcut = false;
    for (const auto& it : items)
    {
        if (it.separator)
        {
            sz.cy += MENU_SEP_H;
            continue;
        }
        if (it.isTitle)
        {
            sz.cy += MENU_TITLE_H;
            const CachedTextFormat* use = (ctfTitle && ctfTitle->fmt) ? ctfTitle : ctf;
            if (use && use->fmt && !it.text.empty())
            {
                IDWriteTextLayout* layout = nullptr;
                if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(it.text.c_str(),
                        (UINT32)it.text.size(), use->fmt, 2000.0f, 100.0f, &layout)))
                {
                    DWRITE_TEXT_METRICS m = {};
                    if (SUCCEEDED(layout->GetMetrics(&m)) && m.width > maxW)
                        maxW = m.width;
                    layout->Release();
                }
            }
            continue;
        }
        sz.cy += MENU_ITEM_H;
        if (it.checked) hasCheck = true;
        if (it.swatch >= 0) hasSwatch = true;
        if (it.icon || it.glyph) hasIcon = true;
        if (it.hasSubmenu) hasArrow = true;
        if (!it.shortcut.empty()) hasShortcut = true;
        if (ctf && ctf->fmt && !it.text.empty())
        {
            IDWriteTextLayout* layout = nullptr;
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(it.text.c_str(),
                    (UINT32)it.text.size(), ctf->fmt, 2000.0f, 100.0f, &layout)))
            {
                DWRITE_TEXT_METRICS m = {};
                if (SUCCEEDED(layout->GetMetrics(&m)) && m.width > maxW)
                    maxW = m.width;
                layout->Release();
            }
        }
        if (!it.shortcut.empty())
        {
            float sw = MeasureTextWidth(it.shortcut, 12.0f, false);
            if (sw > maxShortW) maxShortW = sw;
        }
    }

    if (!commands.empty())
    {
        // Командная панель нового меню: ряд кнопок + разделитель.
        sz.cy += MENU_CMD_H + MENU_SEP_H;
        for (const auto& c : commands)
        {
            float cw = MeasureTextWidth(c.text, 12.0f, false);
            if (cw > maxW) maxW = cw; // грубая оценка: подписи делят ширину
        }
    }

    // Win11-отступы: 12px по бокам, слот иконки 24px, галка/свотч 28px,
    // зона хоткея +16px, шеврон 32px.
    int leftGutter = 12 + (hasIcon ? 24 : 0) + ((hasCheck || hasSwatch) ? 28 : 0);
    int w = leftGutter + (int)(maxW + 0.5f) +
        (hasShortcut ? (int)(maxShortW + 0.5f) + 16 : 0) +
        (hasArrow ? 32 : 0) + 16;
    int minW = commands.empty() ? 180 : MENU_CMD_MIN_W;
    if (w < minW) w = minW;
    if (w > 380) w = 380;
    // Мягкая тень Win11: поля вокруг контента.
    sz.cx = w + MENU_SHADOW * 2;
    sz.cy += MENU_SHADOW * 2;
    return sz;
}

RenderedBitmap WidgetRenderer::RenderMenu(const std::vector<MenuRenderItem>& items,
    int hovered, int pressed, int screenX, int screenY, bool dark)
{
    return RenderMenu({}, items, hovered, pressed, -1, -1, screenX, screenY, dark);
}

RenderedBitmap WidgetRenderer::RenderMenu(const std::vector<MenuRenderItem>& commands,
    const std::vector<MenuRenderItem>& items,
    int hovered, int pressed, int cmdHovered, int cmdPressed,
    int screenX, int screenY, bool dark)
{
    SIZE sz = MeasureMenu(commands, items);
    int w = sz.cx;
    int h = sz.cy;
    const float SH = (float)MENU_SHADOW;
    const int cw = w - MENU_SHADOW * 2; // ширина контента без тени

    if (!m_d2dFactory || !m_wicFactory || !m_dwriteFactory)
        return { nullptr, 0, 0 };

    IWICBitmap* wicBitmap = nullptr;
    m_wicFactory->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wicBitmap);
    if (!wicBitmap) return { nullptr, 0, 0 };

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ID2D1RenderTarget* rt = nullptr;
    m_d2dFactory->CreateWicBitmapRenderTarget(wicBitmap, rtProps, &rt);
    if (!rt)
    {
        wicBitmap->Release();
        return { nullptr, 0, 0 };
    }

    rt->BeginDraw();
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));

    // --- Мягкая тень Win11: три падающих слоя вокруг контента ---
    {
        struct ShadowLayer { float inset; float alpha; };
        static const ShadowLayer layers[] = { { 2, 0.030f }, { 6, 0.055f }, { 10, 0.095f } };
        for (const auto& L : layers)
        {
            ID2D1SolidColorBrush* shBrush = nullptr;
            rt->CreateSolidColorBrush(ColorF(0, 0, 0, L.alpha), &shBrush);
            if (shBrush)
            {
                D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                    D2D1::RectF(L.inset, L.inset, (float)w - L.inset, (float)h - L.inset),
                    (float)MENU_RADIUS + (SH - L.inset), (float)MENU_RADIUS + (SH - L.inset));
                rt->FillRoundedRectangle(rr, shBrush);
                shBrush->Release();
            }
        }
    }

    D2D1_RECT_F menuRect = D2D1::RectF(SH, SH, (float)w - SH, (float)h - SH);

    // --- Фон Win11 Fluent: frosted acrylic + непрозрачная вуаль ---
    // Светлая: почти непрозрачный #F9F9F9, тёмная: #2C2C2C.
    {
        ID2D1RoundedRectangleGeometry* rrg = nullptr;
        m_d2dFactory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(menuRect, (float)MENU_RADIUS, (float)MENU_RADIUS), &rrg);
        ID2D1Layer* glassLayer = nullptr;
        if (rt) rt->CreateLayer(&glassLayer);
        if (rrg && glassLayer)
        {
            rt->PushLayer(
                D2D1::LayerParameters(D2D1::InfiniteRect(), rrg,
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                    D2D1::Matrix3x2F::Identity(),
                    1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE),
                glassLayer);

            IWICBitmap* blurBmp = GetBlurredBackground(screenX, screenY, w, h, 6, false);
            if (blurBmp)
            {
                ID2D1Bitmap* d2dBlur = nullptr;
                rt->CreateBitmapFromWicBitmap(blurBmp, &d2dBlur);
                if (d2dBlur)
                {
                    rt->DrawBitmap(d2dBlur, menuRect, 0.55f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    d2dBlur->Release();
                }
            }

            // Win11 acrylic veil: светлая плотнее для читаемости тёмного текста.
            D2D1_COLOR_F veil = dark ? ColorF(0.17f, 0.17f, 0.18f, 0.82f)
                                     : ColorF(0.976f, 0.976f, 0.976f, 0.82f);
            ID2D1SolidColorBrush* veilBrush = nullptr;
            rt->CreateSolidColorBrush(veil, &veilBrush);
            if (veilBrush)
            {
                rt->FillRectangle(menuRect, veilBrush);
                veilBrush->Release();
            }

            rt->PopLayer();
            glassLayer->Release();
            rrg->Release();
        }
        else
        {
            if (glassLayer) glassLayer->Release();
            if (rrg) rrg->Release();
        }
    }

    // --- Рамка Win11 flyout ---
    DrawRoundedRect(rt, menuRect, (float)MENU_RADIUS, ColorF(0, 0, 0, 0),
        dark ? ColorF(0.0f, 0.0f, 0.0f, 0.45f) : ColorF(0.0f, 0.0f, 0.0f, 0.10f), 1.0f);
    D2D1_RECT_F inner = D2D1::RectF(SH + 1.0f, SH + 1.0f,
        (float)w - SH - 1.0f, (float)h - SH - 1.0f);
    DrawRoundedRect(rt, inner, (float)MENU_RADIUS - 1.0f, ColorF(0, 0, 0, 0),
        dark ? ColorF(1.0f, 1.0f, 1.0f, 0.09f) : ColorF(1.0f, 1.0f, 1.0f, 0.65f), 1.0f);

    // Контент рисуем в 0-базисе со сдвигом на тень.
    rt->SetTransform(D2D1::Matrix3x2F::Translation(SH, SH));

    bool hasCheck = false;
    bool hasSwatch = false;
    bool hasIcon = false;
    bool hasArrow = false;
    for (const auto& it : items)
    {
        if (it.checked) hasCheck = true;
        if (it.swatch >= 0) hasSwatch = true;
        if (it.icon || it.glyph) hasIcon = true;
        if (it.hasSubmenu) hasArrow = true;
    }
    float gutterX = 12.0f;
    float iconX = hasIcon ? 12.0f : -100.0f;
    float textX = 12.0f + (hasIcon ? 24.0f : 0.0f) + ((hasCheck || hasSwatch) ? 28.0f : 0.0f);
    float arrowW = hasArrow ? 32.0f : 0.0f;

    // Палитра Win11.
    const D2D1_COLOR_F colText      = dark ? ColorF(1, 1, 1, 1)
                                           : ColorF(0.106f, 0.106f, 0.106f, 1); // #1B1B1B
    const D2D1_COLOR_F colDisabled  = dark ? ColorF(1, 1, 1, 0.38f)
                                           : ColorF(0.106f, 0.106f, 0.106f, 0.45f);
    const D2D1_COLOR_F colTitle     = dark ? ColorF(1, 1, 1, 0.62f)
                                           : ColorF(0.106f, 0.106f, 0.106f, 0.62f);
    const D2D1_COLOR_F colHover     = dark ? ColorF(1, 1, 1, 0.073f)
                                           : ColorF(0.0f, 0.0f, 0.0f, 0.048f);
    const D2D1_COLOR_F colPressed   = dark ? ColorF(1, 1, 1, 0.11f)
                                           : ColorF(0.0f, 0.0f, 0.0f, 0.083f);
    const D2D1_COLOR_F colSep       = dark ? ColorF(1, 1, 1, 0.10f)
                                           : ColorF(0.0f, 0.0f, 0.0f, 0.083f);
    const D2D1_COLOR_F colChevron   = dark ? ColorF(1, 1, 1, 0.72f)
                                           : ColorF(0.38f, 0.37f, 0.36f, 1); // #605E5C
    const D2D1_COLOR_F colCheck     = dark ? ColorF(1, 1, 1, 1)
                                           : ColorF(0.106f, 0.106f, 0.106f, 1);
    const D2D1_COLOR_F colShortcut  = dark ? ColorF(1, 1, 1, 0.62f)
                                           : ColorF(0.106f, 0.106f, 0.106f, 0.55f);

    int y = MENU_PAD;

    // --- Командная панель нового меню: ряд кнопок с иконками ---
    if (!commands.empty())
    {
        int n = (int)commands.size();
        float cellW = ((float)cw - 8.0f) / (float)n;
        for (int i = 0; i < n; i++)
        {
            const auto& c = commands[i];
            float cx0 = 4.0f + cellW * (float)i;
            float cx1 = cx0 + cellW;
            bool selectable = !c.disabled;
            if ((i == cmdHovered || i == cmdPressed) && selectable)
            {
                ID2D1SolidColorBrush* hoverBrush = nullptr;
                rt->CreateSolidColorBrush(i == cmdPressed ? colPressed : colHover, &hoverBrush);
                if (hoverBrush)
                {
                    D2D1_ROUNDED_RECT hrr = D2D1::RoundedRect(
                        D2D1::RectF(cx0 + 1, (float)y + 1, cx1 - 1, (float)(y + MENU_CMD_H - 1)),
                        4, 4);
                    rt->FillRoundedRectangle(hrr, hoverBrush);
                    hoverBrush->Release();
                }
            }
            D2D1_COLOR_F cellCol = c.disabled ? colDisabled : colText;
            // Глиф по центру верхней части.
            if (c.glyph && m_dwriteFactory)
            {
                EnsureMenuGlyphFmt();
                if (m_menuGlyphFmt)
                {
                    ID2D1SolidColorBrush* gBrush = nullptr;
                    rt->CreateSolidColorBrush(cellCol, &gBrush);
                    if (gBrush)
                    {
                        WCHAR gs[2] = { c.glyph, 0 };
                        D2D1_RECT_F gr = D2D1::RectF(cx0, (float)y + 6,
                            cx1, (float)y + 36);
                        rt->DrawText(gs, 1, m_menuGlyphFmt, gr, gBrush);
                        gBrush->Release();
                    }
                }
            }
            // Подпись под иконкой.
            if (!c.text.empty())
            {
                D2D1_RECT_F lr = D2D1::RectF(cx0 + 2, (float)y + 36,
                    cx1 - 2, (float)(y + MENU_CMD_H - 2));
                DrawText(rt, c.text, lr, 12.0f, cellCol, false,
                    DWRITE_TEXT_ALIGNMENT_CENTER, true);
            }
        }
        y += MENU_CMD_H;
        // Разделитель под панелью.
        ID2D1SolidColorBrush* cmdSep = nullptr;
        rt->CreateSolidColorBrush(colSep, &cmdSep);
        if (cmdSep)
        {
            float sepY = (float)y + MENU_SEP_H / 2.0f;
            rt->DrawLine(D2D1::Point2F(12.0f, sepY),
                D2D1::Point2F((float)(cw - 12), sepY), cmdSep, 1.0f);
            cmdSep->Release();
        }
        y += MENU_SEP_H;
    }
    for (int i = 0; i < (int)items.size(); i++)
    {
        const auto& it = items[i];
        if (it.separator)
        {
            ID2D1SolidColorBrush* sepBrush = nullptr;
            rt->CreateSolidColorBrush(colSep, &sepBrush);
            if (sepBrush)
            {
            float sepY = (float)y + MENU_SEP_H / 2.0f;
            rt->DrawLine(D2D1::Point2F(12.0f, sepY),
                D2D1::Point2F((float)(cw - 12), sepY), sepBrush, 1.0f);
                sepBrush->Release();
            }
            y += MENU_SEP_H;
            continue;
        }

        if (it.isTitle)
        {
            // Шапка: 12px semibold, серый, по левому краю gutter.
            D2D1_RECT_F titleRect = D2D1::RectF(gutterX, (float)y,
                (float)cw - 12, (float)(y + MENU_TITLE_H));
            DrawText(rt, it.text, titleRect, 12.0f, colTitle, true,
                DWRITE_TEXT_ALIGNMENT_LEADING, true);
            y += MENU_TITLE_H;
            continue;
        }

        bool selectable = !it.disabled;
        if ((i == hovered || i == pressed) && selectable)
        {
            ID2D1SolidColorBrush* hoverBrush = nullptr;
            rt->CreateSolidColorBrush(i == pressed ? colPressed : colHover, &hoverBrush);
            if (hoverBrush)
            {
                // Win11 hover-пилюля 4px, отступ 4px по бокам.
                D2D1_ROUNDED_RECT hrr = D2D1::RoundedRect(
                    D2D1::RectF(4, (float)y + 1, (float)(cw - 4), (float)(y + MENU_ITEM_H - 1)),
                    4, 4);
                rt->FillRoundedRectangle(hrr, hoverBrush);
                hoverBrush->Release();
            }
        }

        float midY = (float)y + MENU_ITEM_H / 2.0f;

        if (it.swatch >= 0)
        {
            int rgb = it.swatch;
            D2D1_COLOR_F sc = ColorF(((rgb >> 16) & 0xFF) / 255.0f,
                ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f);
            ID2D1SolidColorBrush* swBrush = nullptr;
            rt->CreateSolidColorBrush(sc, &swBrush);
            if (swBrush)
            {
                D2D1_ROUNDED_RECT sw = D2D1::RoundedRect(
                    D2D1::RectF(gutterX + 2, midY - 8, gutterX + 18, midY + 8), 4, 4);
                rt->FillRoundedRectangle(sw, swBrush);
                swBrush->Release();
            }
            ID2D1SolidColorBrush* swEdge = nullptr;
            rt->CreateSolidColorBrush(dark ? ColorF(1, 1, 1, 0.18f)
                                           : ColorF(0, 0, 0, 0.22f), &swEdge);
            if (swEdge)
            {
                D2D1_ROUNDED_RECT sw = D2D1::RoundedRect(
                    D2D1::RectF(gutterX + 2, midY - 8, gutterX + 18, midY + 8), 4, 4);
                rt->DrawRoundedRectangle(sw, swEdge, 1.0f);
                swEdge->Release();
            }
        }

        if (it.checked)
        {
            // Векторная галка Win11; на светлых свотчах — тёмная.
            D2D1_COLOR_F checkCol = colCheck;
            if (it.swatch >= 0)
            {
                int rgb = it.swatch;
                float lum = (0.30f * ((rgb >> 16) & 0xFF) +
                             0.59f * ((rgb >> 8) & 0xFF) +
                             0.11f * (rgb & 0xFF)) / 255.0f;
                if (lum > 0.6f) checkCol = ColorF(0, 0, 0, 0.78f);
                else checkCol = ColorF(1, 1, 1, 1);
            }
            ID2D1SolidColorBrush* checkBrush = nullptr;
            rt->CreateSolidColorBrush(checkCol, &checkBrush);
            if (checkBrush)
            {
                float cx = (it.swatch >= 0) ? (gutterX + 10) : (gutterX + 8);
                ID2D1PathGeometry* path = nullptr;
                if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(&path)))
                {
                    ID2D1GeometrySink* sink = nullptr;
                    if (SUCCEEDED(path->Open(&sink)))
                    {
                        sink->BeginFigure(D2D1::Point2F(cx - 5, midY),
                            D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddLine(D2D1::Point2F(cx - 1, midY + 4));
                        sink->AddLine(D2D1::Point2F(cx + 6, midY - 5));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        if (SUCCEEDED(sink->Close()))
                            rt->DrawGeometry(path, checkBrush, 1.8f);
                        sink->Release();
                    }
                    path->Release();
                }
                checkBrush->Release();
            }
        }

        D2D1_COLOR_F textCol = it.disabled ? colDisabled : colText;
        float textRight = (float)cw - 12 - arrowW;
        float shortW = 0.0f;
        if (!it.shortcut.empty())
        {
            // Хоткей справа серым, как в новом меню Win11.
            shortW = MeasureTextWidth(it.shortcut, 12.0f, false);
            D2D1_RECT_F shortRect = D2D1::RectF(textRight - shortW, (float)y,
                textRight, (float)(y + MENU_ITEM_H));
            DrawText(rt, it.shortcut, shortRect, 12.0f,
                it.disabled ? colDisabled : colShortcut, false,
                DWRITE_TEXT_ALIGNMENT_TRAILING, false);
            textRight -= shortW + 12.0f;
        }
        D2D1_RECT_F textRect = D2D1::RectF(textX, (float)y,
            textRight, (float)(y + MENU_ITEM_H));
        DrawText(rt, it.text, textRect, 14.0f, textCol, false,
            DWRITE_TEXT_ALIGNMENT_LEADING, true);

        if (hasIcon && it.icon)
            DrawMenuIcon(rt, it.icon, iconX + 4.0f, midY - 8.0f, 16.0f);

        // Глиф Segoe MDL2 в слоте иконки (например, «…» у доп. параметров).
        // Тот же 14px формат — для шевронов подменю ниже.
        EnsureMenuChevronFmt();
        if (it.glyph && !it.icon && m_menuChevronFmt)
        {
            ID2D1SolidColorBrush* glBrush = nullptr;
            rt->CreateSolidColorBrush(it.disabled ? colDisabled : colChevron, &glBrush);
            if (glBrush)
            {
                WCHAR gs[2] = { it.glyph, 0 };
                D2D1_RECT_F gr = D2D1::RectF(iconX, (float)y,
                    iconX + 24.0f, (float)(y + MENU_ITEM_H));
                rt->DrawText(gs, 1, m_menuChevronFmt, gr, glBrush);
                glBrush->Release();
            }
        }

        if (it.hasSubmenu)
        {
            // Шеврон Fluent (как в новом меню Win11), а не текстовый ›.
            if (m_menuChevronFmt)
            {
                ID2D1SolidColorBrush* chBrush = nullptr;
                rt->CreateSolidColorBrush(it.disabled ? colDisabled : colChevron, &chBrush);
                if (chBrush)
                {
                    D2D1_RECT_F arrowRect = D2D1::RectF((float)(cw - 12 - arrowW),
                        (float)y, (float)(cw - 12), (float)(y + MENU_ITEM_H));
                    rt->DrawText(L"\uE76C", 1, m_menuChevronFmt, arrowRect, chBrush);
                    chBrush->Release();
                }
            }
            else
            {
                D2D1_RECT_F arrowRect = D2D1::RectF((float)(cw - 12 - arrowW),
                    (float)y, (float)(cw - 12), (float)(y + MENU_ITEM_H));
                DrawText(rt, L"\u203A", arrowRect, 15.0f,
                    it.disabled ? colDisabled : colChevron, false,
                    DWRITE_TEXT_ALIGNMENT_TRAILING, false);
            }
        }

        y += MENU_ITEM_H;
    }

    HDC hdcScreen = GetDC(nullptr);
    HBITMAP hBmp = FinishRendering(rt, wicBitmap, hdcScreen, w, h);
    ReleaseDC(nullptr, hdcScreen);

    rt->Release();
    wicBitmap->Release();

    return { hBmp, w, h };
}
