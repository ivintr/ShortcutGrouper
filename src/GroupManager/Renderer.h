#pragma once
#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <vector>
#include <map>
#include <string>
#include <cstdint>
#include <functional>
#include "WidgetTypes.h"

struct RenderedBitmap {
    HBITMAP hBitmap;
    int width;
    int height;
};

struct WidgetRenderContext {
    int widgetScreenX = 0;
    int widgetScreenY = 0;
};

class WidgetRenderer {
public:
    WidgetRenderer();
    ~WidgetRenderer();
    bool Initialize();
    void Shutdown();

    RenderedBitmap RenderWidget(const GroupData& group, const WidgetRenderContext& ctx = {},
        int hoverGlow = 0, bool selected = false);
    RenderedBitmap RenderPopup(const GroupData& group, int hoveredIndex = -1,
        int screenX = 0, int screenY = 0, int scrollY = 0, int maxListH = -1,
        int popupW = POPUP_W, bool fastBg = false, int visListH = -1);

    int HitTestPopup(const GroupData& group, int mouseX, int mouseY, int scrollY = 0);

    // Modern glass menu (Win11 Fluent style). Item metrics shared with ModernMenu.
    struct MenuRenderItem {
        std::wstring text;
        bool checked = false;
        bool disabled = false;
        bool separator = false;
        bool hasSubmenu = false;
        bool isTitle = false; // шапка меню: некликабельный полужирный заголовок
        int swatch = -1; // 0xRRGGBB or -1
        HBITMAP icon = nullptr; // иконка shell-пункта слева (не владеем)
        std::wstring shortcut;  // хоткей справа ("Ctrl+Shift+C")
        wchar_t glyph = 0;      // глиф Segoe MDL2 (командная панель)
    };
    static const int MENU_ITEM_H = 32;
    static const int MENU_TITLE_H = 30;
    static const int MENU_SEP_H = 9;
    static const int MENU_PAD = 6;
    static const int MENU_RADIUS = 8;
    static const int MENU_CMD_H = 66; // высота командной панели нового меню
    static const int MENU_CMD_MIN_W = 300; // мин. ширина меню с панелью
    static const int MENU_SHADOW = 14; // мягкая тень Win11 вокруг меню
    SIZE MeasureMenu(const std::vector<MenuRenderItem>& items);
    SIZE MeasureMenu(const std::vector<MenuRenderItem>& commands,
        const std::vector<MenuRenderItem>& items);
    RenderedBitmap RenderMenu(const std::vector<MenuRenderItem>& items,
        int hovered, int pressed, int screenX, int screenY, bool dark);
    RenderedBitmap RenderMenu(const std::vector<MenuRenderItem>& commands,
        const std::vector<MenuRenderItem>& items,
        int hovered, int pressed, int cmdHovered, int cmdPressed,
        int screenX, int screenY, bool dark);

    // Отложенная загрузка иконок для быстрого старта: пока включена,
    // GetIconBitmap возвращает nullptr (плейсхолдеры-папки), виджеты
    // рисуются мгновенно без дорогих обращений к shell. После старта
    // выключается + RefreshWidgetIcons перерисовывает всё с иконками.
    void SetDeferIcons(bool defer) { m_deferIcons = defer; }

    // Матовое стекло как у контекстного меню (размытый фон + вуаль),
    // готовый HBITMAP w*h для заливки GDI-окна. Владеет вызыватель.
    // nullptr, если фон недоступен (тогда сплошная заливка).
    HBITMAP RenderFrostedBackground(int x, int y, int w, int h, bool dark);

    // Хук, скрывающий наши окна на время BitBlt-захвата экрана.
    // Иначе попап/виджеты попадают в собственный фон (двоение текста/иконок
    // в блюре, ореолы). Ставится из DesktopWidget.cpp, где видны классы окон.
    // x,y,w,h — прямоугольник захвата: прятать нужно только пересекающиеся.
    // Иначе попап/виджеты попадают в собственный фон (двоение текста/иконок
    // в блюре, ореолы). Ставится из DesktopWidget.cpp, где видны классы окон.
    // x,y,w,h — прямоугольник захвата: прятать нужно только пересекающиеся.
    void SetCaptureGuard(std::function<void(bool, int, int, int, int)> guard) { m_captureGuard = guard; }

private:
    bool CreateFactories();
    void DrawRoundedRect(ID2D1RenderTarget* rt, D2D1_RECT_F rect,
        float radius, D2D1_COLOR_F fill, D2D1_COLOR_F border = {0,0,0,0}, float borderWidth = 0);
    void DrawText(ID2D1RenderTarget* rt, const std::wstring& text,
        D2D1_RECT_F rect, float fontSize, D2D1_COLOR_F color, bool bold = false,
        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_CENTER,
        bool ellipsis = false);
    void DrawMiniIcons(ID2D1RenderTarget* rt, const GroupData& group,
        float cx, float cy);
    void DrawFolderIcon(ID2D1RenderTarget* rt, float cx, float cy, float size);

    // Иконка ярлыка в виде D2D-битмапа. WIC-копия кэшируется по пути файла,
    // чтобы каждый кадр (ховер/скролл/ресайз) не дёргать shell и конвертеры.
    // Возвращает AddRef'нутый битмап или nullptr; вызывающий делает Release.
    ID2D1Bitmap* GetIconBitmap(ID2D1RenderTarget* rt, const ShortcutInfo& si);
    void ClearIconCache();

    // Превью картинки (IShellItemImageFactory) вместо дефолтной иконки.
    // Кэшируется в том же m_iconCache; вызывающий делает Release D2D-битмапу.
    // Возвращает nullptr, если это не картинка или превью недоступно.
    ID2D1Bitmap* TryThumbnailBitmap(ID2D1RenderTarget* rt,
        const std::wstring& cacheKey, const std::wstring& filePath);

    // Конвертация WIC-источника в кэшируемую 32bppPBGRA-копию.
    // Возвращает внутренний указатель (владение у кэша) или nullptr.
    IWICBitmap* ConvertToCache(const std::wstring& key, IWICBitmap* src);

    // Размытый фон под окном. Кэшируется по (x,y,w,h,radius): повторные кадры
    // на той же геометрии (ховер, скролл) не делают BitBlt+blur заново.
    // allowStale=true: вернуть имеющийся кэш даже при смене геометрии
    // (живой ресайз — растянутый старый фон, чёткий доберётся при отпускании).
    // Возвращает внутренний указатель БЕЗ передачи владения — не Release'ить!
    IWICBitmap* GetBlurredBackground(int x, int y, int w, int h, int blurRadius,
        bool allowStale = false);

    HBITMAP FinishRendering(ID2D1RenderTarget* rt, IWICBitmap* wicBitmap,
        HDC hdcScreen, int width, int height);

    IWICBitmap* CaptureDesktopBlur(int x, int y, int w, int h, int blurRadius);

    ID2D1Factory*          m_d2dFactory   = nullptr;
    IDWriteFactory*        m_dwriteFactory = nullptr;
    IWICImagingFactory*    m_wicFactory   = nullptr;
    bool                   m_deferIcons   = false;

    // Кэш текстовых форматов: IDWriteTextFormat — объект уровня фабрики,
    // создание тянет поиск шрифтов, а на кадр их нужны десятки.
    // Ключ: размер x16 | bold | align | ellipsis.
    struct CachedTextFormat {
        IDWriteTextFormat* fmt = nullptr;
        IDWriteInlineObject* trim = nullptr;
    };
    const CachedTextFormat* GetTextFormat(float fontSize, bool bold,
        DWRITE_TEXT_ALIGNMENT align, bool ellipsis);
    void ClearTextFormats();
    std::map<uint64_t, CachedTextFormat> m_textFormats;

    // Ширина текста для раскладки меню (12/14px).
    float MeasureTextWidth(const std::wstring& text, float fontSize, bool bold);
    // Иконка shell-пункта 16px из HBITMAP (конвертация на месте).
    void DrawMenuIcon(ID2D1RenderTarget* rt, HBITMAP hbm, float x, float y, float size);

    // Кэш иконок: путь файла -> WIC-битмап 32bppPBGRA
    std::map<std::wstring, IWICBitmap*> m_iconCache;

    // Формат глифов Segoe MDL2 Assets для командной панели (ленивый).
    IDWriteTextFormat* m_menuGlyphFmt = nullptr;
    // Тот же шрифт 12px для шевронов подменю.
    IDWriteTextFormat* m_menuChevronFmt = nullptr;
    // Ленивое создание под гардом кэша (без гарда — гонка с Shutdown
    // и между потоками).
    void EnsureMenuGlyphFmt();
    void EnsureMenuChevronFmt();

    // Кэш фонов: последние захваченные геометрии + размытые битмапы.
    // Одной записи мало: ховер по соседним виджетам сносил бы кэш и каждый
    // раз прятал все окна (мигание). Ключ — геометрия + радиус.
    struct BgKey {
        int x, y, w, h, r;
        bool operator<(const BgKey& o) const {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            if (w != o.w) return w < o.w;
            if (h != o.h) return h < o.h;
            return r < o.r;
        }
    };
    std::map<BgKey, IWICBitmap*> m_bgCache;
    void ClearBgCache();

    std::function<void(bool, int, int, int, int)> m_captureGuard;

    // Потокобезопасность кэшей: фабрика SINGLE_THREADED, кэши — std::map.
    // Весь рендер идёт в UI-потоке; CS рекурсивна (вложенные вызовы кэшей
    // безопасны), AssertUiThread ловит левые потоки в отладке.
    void LockCache();
    void UnlockCache();
    void AssertUiThread() const;
    struct CacheGuard {
        WidgetRenderer* r;
        explicit CacheGuard(WidgetRenderer* r) : r(r) { r->LockCache(); }
        ~CacheGuard() { r->UnlockCache(); }
    };
    CRITICAL_SECTION m_cacheCs;
    bool m_cacheCsInit = false;
    DWORD m_ownerThread = 0;
};
