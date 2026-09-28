#include "ColorDialog.h"
#include "Lang.h"
#include "ColorUtils.h"
#include "DpiHelper.h"
#include "ModernMenu.h"
#include "Renderer.h"
#include "../../resources/resource.h"
#include <Windows.h>
#include <dwmapi.h>
#include <cmath>

#pragma comment(lib, "msimg32.lib")

namespace {

// Число Пи для колеса Hue (atan2 -> градусы).
constexpr double kPi = 3.141592653589793;

// Базовые размеры при 96 DPI, масштабируются.
constexpr int kPad = 20;
constexpr int kWheel = 200;
constexpr int kGap = 16;
constexpr int kSquare = 150;
constexpr int kPreviewH = 26;
constexpr int kBtnW = 110;
constexpr int kBtnH = 32;
constexpr int kTitleH = 44;   // свой стеклянный заголовок как в настройках
constexpr int kCloseW = 46;
constexpr int kCloseH = 32;

struct ColorDlg {
    HWND hwnd = nullptr;
    HWND hOk = nullptr;
    HWND hCancel = nullptr;
    WidgetRenderer* renderer = nullptr; // нужен для обновления фона при перетаскивании
    UINT dpi = 96;
    bool dark = false;
    HBITMAP hBg = nullptr; // матовое стекло фона (захват до показа)
    HFONT hTitleFont = nullptr;
    HFONT hCloseGlyph = nullptr;
    ColorHsv hsv = { 0, 1, 1 };
    int drag = 0; // 0 нет, 1 кольцо, 2 квадрат
    int hoverBtn = 0; // 0 нет, 1 OK, 2 отмена
    int pressedBtn = 0;
    bool confirmed = false;
    bool done = false;
    HBITMAP hWheel = nullptr; // x2 DIB для гладкости
    HBITMAP hSquare = nullptr;
    RECT rcTitle = {};
    RECT rcClose = {};
    bool closeHover = false;
    bool closePressed = false;
    RECT rcWheel = {};
    RECT rcSquare = {};
    RECT rcPreview = {};
    RECT rcOk = {};
    RECT rcCancel = {};
    int clientW = 0;
    int clientH = 0;
};

int S(const ColorDlg* st, int x) { return MulDiv(x, (int)st->dpi, 96); }

COLORREF RgbInt(int rgb)
{
    return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

// DIB 32bpp, возвращает HBITMAP (владеет вызыватель).
HBITMAP BuildWheelDib(int sizePx)
{
    // Честное сглаживание: считаем в SSxSS и усредняем боксом в финальный
    // премультиплицированный DIB. Растягивать через AlphaBlend нельзя —
    // его фильтр даёт лесенку на краях.
    constexpr int SS = 4;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = sizePx;
    bmi.bmiHeader.biHeight = -sizePx;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hbmp || !bits) { if (hbmp) DeleteObject(hbmp); return nullptr; }

    double R = sizePx / 2.0 - 1;
    double thick = R * 0.22;
    if (thick < 6) thick = 6;
    double inR = R - thick;
    BYTE* px = (BYTE*)bits;
    for (int y = 0; y < sizePx; y++)
    {
        for (int x = 0; x < sizePx; x++)
        {
            double sr = 0, sg = 0, sb = 0, sa = 0;
            for (int sy = 0; sy < SS; sy++)
            {
                for (int sx = 0; sx < SS; sx++)
                {
                    // Центр субпикселя в финальных координатах.
                    double fx = x + (sx + 0.5) / SS;
                    double fy = y + (sy + 0.5) / SS;
                    double dx = fx - sizePx / 2.0, dy = fy - sizePx / 2.0;
                    double dist = sqrt(dx * dx + dy * dy);
                    // Плавный край ~1px: 0 внутри полосы, 1 снаружи.
                    double d = max(inR - dist, dist - R);
                    double a = 1.0 - max(0.0, min(1.0, d + 0.5));
                    double hue = 0;
                    if (a > 0.0)
                    {
                        hue = atan2(-dy, dx) * 180.0 / kPi;
                        if (hue < 0) hue += 360;
                    }
                    int rgb = HsvToRgb(hue, 1.0, 1.0);
                    sr += ((rgb >> 16) & 255) * a;
                    sg += ((rgb >> 8) & 255) * a;
                    sb += (rgb & 255) * a;
                    sa += a;
                }
            }
            double n = (double)(SS * SS);
            BYTE* p = px + ((size_t)y * sizePx + x) * 4;
            p[0] = (BYTE)(sb / n + 0.5);
            p[1] = (BYTE)(sg / n + 0.5);
            p[2] = (BYTE)(sr / n + 0.5);
            p[3] = (BYTE)(sa / n * 255 + 0.5);
        }
    }
    return hbmp;
}

HBITMAP BuildSquareDib(int sizePx, double hue)
{
    // sizePx<=1 давал деление на ноль (sizePx-1) ниже — отбой заранее.
    if (sizePx <= 1) return nullptr;
    // SV-диск: та же развёртка S/V, что у квадрата, но вписанный круг.
    // Вне круга — прозрачность (там стекло). Честное сглаживание:
    // суперсемплинг + бокс (растяжение через AlphaBlend даёт лесенку).
    constexpr int SS = 2;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = sizePx;
    bmi.bmiHeader.biHeight = -sizePx;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hbmp || !bits) { if (hbmp) DeleteObject(hbmp); return nullptr; }

    double R = sizePx / 2.0 - 1;
    BYTE* px = (BYTE*)bits;
    for (int y = 0; y < sizePx; y++)
    {
        for (int x = 0; x < sizePx; x++)
        {
            double sr = 0, sg = 0, sb = 0, sa = 0;
            for (int sy = 0; sy < SS; sy++)
            {
                for (int sx = 0; sx < SS; sx++)
                {
                    double fx = x + (sx + 0.5) / SS;
                    double fy = y + (sy + 0.5) / SS;
                    double dx = fx - sizePx / 2.0, dy = fy - sizePx / 2.0;
                    double dist = sqrt(dx * dx + dy * dy);
                    double a = 1.0 - max(0.0, min(1.0, dist - (R - 0.5)));
                    double s = fx / (sizePx - 1);
                    double v = 1.0 - fy / (sizePx - 1);
                    if (s < 0) s = 0; if (s > 1) s = 1;
                    if (v < 0) v = 0; if (v > 1) v = 1;
                    int rgb = HsvToRgb(hue, s, v);
                    sr += ((rgb >> 16) & 255) * a;
                    sg += ((rgb >> 8) & 255) * a;
                    sb += (rgb & 255) * a;
                    sa += a;
                }
            }
            double n = (double)(SS * SS);
            BYTE* p = px + ((size_t)y * sizePx + x) * 4;
            p[0] = (BYTE)(sb / n + 0.5);
            p[1] = (BYTE)(sg / n + 0.5);
            p[2] = (BYTE)(sr / n + 0.5);
            p[3] = (BYTE)(sa / n * 255 + 0.5);
        }
    }
    return hbmp;
}

void RebuildSquare(ColorDlg* st)
{
    if (st->hSquare) { DeleteObject(st->hSquare); st->hSquare = nullptr; }
    int side = st->rcSquare.right - st->rcSquare.left;
    if (side > 0)
        st->hSquare = BuildSquareDib(side, st->hsv.h);
}

void LayoutColorDlg(ColorDlg* st)
{
    int pad = S(st, kPad);
    int wheel = S(st, kWheel);
    int sq = S(st, kSquare);
    int W = pad + wheel + S(st, kGap) + sq + pad;
    // Свой стеклянный заголовок сверху (как в настройках).
    st->rcTitle = { S(st, 16), 0, W - S(st, 60), S(st, kTitleH) };
    st->rcClose = { W - S(st, kCloseW), 0, W, S(st, kCloseH) };
    int y = S(st, kTitleH) + S(st, 4);
    st->rcWheel = { pad, y, pad + wheel, y + wheel };
    st->rcSquare = { pad + wheel + S(st, kGap), y,
        pad + wheel + S(st, kGap) + sq, y + sq };
    y += max(wheel, sq) + S(st, 16);
    st->rcPreview = { pad, y, W - pad, y + S(st, kPreviewH) };
    y += S(st, kPreviewH) + S(st, 16);
    int bw = S(st, kBtnW), bh = S(st, kBtnH);
    st->rcCancel = { W - pad - bw, y, W - pad, y + bh };
    st->rcOk = { W - pad - bw * 2 - S(st, 12), y, W - pad - bw - S(st, 12), y + bh };
    y += bh + pad;
    st->clientW = W;
    st->clientH = y;
    if (st->hOk)
        SetWindowPos(st->hOk, nullptr, st->rcOk.left, st->rcOk.top,
            st->rcOk.right - st->rcOk.left, st->rcOk.bottom - st->rcOk.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    if (st->hCancel)
        SetWindowPos(st->hCancel, nullptr, st->rcCancel.left, st->rcCancel.top,
            st->rcCancel.right - st->rcCancel.left, st->rcCancel.bottom - st->rcCancel.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
}

bool HueFromPoint(const ColorDlg* st, POINT pt, double& hue)
{
    int cx = (st->rcWheel.left + st->rcWheel.right) / 2;
    int cy = (st->rcWheel.top + st->rcWheel.bottom) / 2;
    double outer = (st->rcWheel.right - st->rcWheel.left) / 2.0 - 1;
    double thick = outer * 0.22;
    if (thick < 6) thick = 6;
    double dx = pt.x - cx, dy = pt.y - cy;
    double dist = sqrt(dx * dx + dy * dy);
    if (dist < outer - thick - 4 || dist > outer + 4) return false;
    hue = atan2(-dy, dx) * 180.0 / kPi;
    if (hue < 0) hue += 360;
    return true;
}

bool SvFromPoint(const ColorDlg* st, POINT pt, double& s, double& v)
{
    int w = st->rcSquare.right - st->rcSquare.left;
    int h = st->rcSquare.bottom - st->rcSquare.top;
    if (w <= 0 || h <= 0) return false;
    // Диск: принимаем клики внутри круга (+ небольшой допуск),
    // точку для S/V прижимаем к кругу.
    double cx = st->rcSquare.left + w / 2.0;
    double cy = st->rcSquare.top + h / 2.0;
    double R = min(w, h) / 2.0 - 1;
    double dx = pt.x - cx, dy = pt.y - cy;
    double dist = sqrt(dx * dx + dy * dy);
    if (dist > R + 8) return false;
    double px = pt.x, py = pt.y;
    if (dist > R && dist > 0)
    {
        px = cx + dx / dist * R;
        py = cy + dy / dist * R;
    }
    s = (px - st->rcSquare.left) / w;
    v = 1.0 - (py - st->rcSquare.top) / h;
    if (s < 0) s = 0; if (s > 1) s = 1;
    if (v < 0) v = 0; if (v > 1) v = 1;
    return true;
}

int HitButton(const ColorDlg* st, POINT pt)
{
    if (PtInRect(&st->rcOk, pt)) return 1;
    if (PtInRect(&st->rcCancel, pt)) return 2;
    return 0;
}

} // namespace

static const wchar_t* COLOR_DLG_CLASS = L"GroupColorDlg";
static const int IDC_COLOR_OK = 3001;
static const int IDC_COLOR_CANCEL = 3002;

static LRESULT CALLBACK ColorDlgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

static void DoColorOk(ColorDlg* st)
{
    if (!st || st->done) return;
    st->confirmed = true;
    st->done = true;
    DestroyWindow(st->hwnd);
}

static void DoColorCancel(ColorDlg* st)
{
    if (!st || st->done) return;
    st->done = true;
    DestroyWindow(st->hwnd);
}

static LRESULT CALLBACK ColorDlgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    ColorDlg* st = (ColorDlg*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    switch (uMsg)
    {
    case WM_CREATE:
    {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        st = (ColorDlg*)cs->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)st);
        st->hwnd = hWnd;
        st->dpi = GetDpiForWindowCompat(hWnd);
        if (!st->dpi) st->dpi = 96;
        st->dark = ModernMenuIsDarkMode();
        st->hTitleFont = CreateFontW(-MulDiv(9, (int)st->dpi, 72), 0, 0, 0, FW_SEMIBOLD,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI Variable Text");
        if (!st->hTitleFont)
            st->hTitleFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        st->hCloseGlyph = CreateFontW(-MulDiv(10, (int)st->dpi, 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe MDL2 Assets");
        if (!st->hCloseGlyph)
            st->hCloseGlyph = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        st->hOk = CreateWindowExW(0, L"BUTTON", Lang::Get(Str::D_Ok),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_OWNERDRAW,
            0, 0, 10, 10, hWnd, (HMENU)(INT_PTR)IDC_COLOR_OK, hInst, nullptr);
        st->hCancel = CreateWindowExW(0, L"BUTTON", Lang::Get(Str::D_Cancel),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 10, 10, hWnd, (HMENU)(INT_PTR)IDC_COLOR_CANCEL, hInst, nullptr);
        // Без кнопок диалог немой — закрываем сразу, а не показываем труп.
        if (!st->hOk || !st->hCancel)
        {
            DestroyWindow(hWnd);
            return 0;
        }
        LayoutColorDlg(st);
        {
            RECT rc = { 0, 0, st->clientW, st->clientH };
            AdjustWindowRectEx(&rc, WS_POPUP | WS_SYSMENU,
                FALSE, WS_EX_APPWINDOW);
            SetWindowPos(hWnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        // DWM: скругление + тёмная рамка под тему.
        {
            HMODULE hDwm = GetModuleHandleW(L"dwmapi.dll");
            if (hDwm)
            {
                using FnAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
                auto fn = (FnAttr)GetProcAddress(hDwm, "DwmSetWindowAttribute");
                if (fn)
                {
                    int corner = 2; // DWMWCP_ROUND
                    fn(hWnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
                    BOOL dark = st->dark ? TRUE : FALSE;
                    fn(hWnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
                }
            }
        }
        // DWM: скругление + тёмная рамка под тему. Акрил НЕ включаем:
        // фон — наше матовое стекло как у контекстного меню (см. hBg).
        int side = st->rcWheel.right - st->rcWheel.left;
        if (side > 0)
            st->hWheel = BuildWheelDib(side);
        RebuildSquare(st);
        return 0;
    }

    case WM_ERASEBKGND:
    {
        if (!st) break;
        // Матовое стекло как у контекстного меню (захвачено до показа).
        // Нет фона — сплошная заливка в цвет темы.
        if (st->hBg)
        {
            HDC hdc = (HDC)wParam;
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, st->hBg);
            RECT cl = {};
            GetClientRect(hWnd, &cl);
            BitBlt(hdc, 0, 0, cl.right, cl.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
            return 1;
        }
        HDC hdc = (HDC)wParam;
        RECT cl = {};
        GetClientRect(hWnd, &cl);
        HBRUSH br = CreateSolidBrush(st->dark ? RGB(32, 32, 32) : RGB(243, 243, 243));
        FillRect(hdc, &cl, br);
        DeleteObject(br);
        return 1;
    }

    case WM_PAINT:
    {
        if (!st) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        SetBkMode(hdc, TRANSPARENT);
        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, nullptr);

        // Свой стеклянный заголовок (как в настройках): текст + крестик.
        {
            HGDIOBJ of = SelectObject(hdc, st->hTitleFont ? st->hTitleFont :
                (HFONT)GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(hdc, st->dark ? RGB(255, 255, 255) : RGB(27, 27, 27));
            DrawTextW(hdc, Lang::Get(Str::D_ColorTitle), -1, &st->rcTitle,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (st->closeHover || st->closePressed)
            {
                COLORREF cb = st->closePressed ? RGB(139, 30, 20) : RGB(196, 43, 28);
                HBRUSH cbr = CreateSolidBrush(cb);
                FillRect(hdc, &st->rcClose, cbr);
                DeleteObject(cbr);
            }
            if (st->hCloseGlyph) SelectObject(hdc, st->hCloseGlyph);
            SetTextColor(hdc, (st->closeHover || st->closePressed)
                ? RGB(255, 255, 255)
                : (st->dark ? RGB(255, 255, 255) : RGB(27, 27, 27)));
            DrawTextW(hdc, L"\uE711", -1, const_cast<RECT*>(&st->rcClose),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(hdc, of);
        }

        // Оба диска — через AlphaBlend (прозрачные края и дырка кольца).
        auto blitAlpha = [&](HBITMAP hb, const RECT& rc) {
            if (!hb) return;
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, hb);
            BITMAP bm = {};
            GetObjectW(hb, sizeof(bm), &bm);
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            GdiAlphaBlend(hdc, rc.left, rc.top, rc.right - rc.left,
                rc.bottom - rc.top, mem, 0, 0, bm.bmWidth, bm.bmHeight, bf);
            SelectObject(mem, old);
            DeleteDC(mem);
        };
        blitAlpha(st->hWheel, st->rcWheel);
        blitAlpha(st->hSquare, st->rcSquare);

        // Маркеры текущего цвета.
        int rgb = HsvToRgb(st->hsv.h, st->hsv.s, st->hsv.v);
        {
            int cx = (st->rcWheel.left + st->rcWheel.right) / 2;
            int cy = (st->rcWheel.top + st->rcWheel.bottom) / 2;
            double outer = (st->rcWheel.right - st->rcWheel.left) / 2.0 - 1;
            double thick = outer * 0.22;
            if (thick < 6) thick = 6;
            double rad = outer - thick / 2.0; // середина полосы кольца
            double a = st->hsv.h * kPi / 180.0;
            int mx = (int)(cx + cos(a) * rad);
            int my = (int)(cy - sin(a) * rad);
            HPEN wpen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HPEN bpen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
            HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            HGDIOBJ op = SelectObject(hdc, wpen);
            Ellipse(hdc, mx - 6, my - 6, mx + 6, my + 6);
            SelectObject(hdc, bpen);
            Ellipse(hdc, mx - 7, my - 7, mx + 7, my + 7);
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(wpen);
            DeleteObject(bpen);
        }
        {
            int w = st->rcSquare.right - st->rcSquare.left;
            int h = st->rcSquare.bottom - st->rcSquare.top;
            double fx = st->hsv.s * w;
            double fy = (1 - st->hsv.v) * h;
            // Прижимаем маркер к кругу (углы квадрата отрезаны).
            double cx = w / 2.0, cy = h / 2.0;
            double R = min(w, h) / 2.0 - 1;
            double dx = fx - cx, dy = fy - cy;
            double dist = sqrt(dx * dx + dy * dy);
            if (dist > R && dist > 0)
            {
                fx = cx + dx / dist * R;
                fy = cy + dy / dist * R;
            }
            int mx = st->rcSquare.left + (int)fx;
            int my = st->rcSquare.top + (int)fy;
            HPEN wpen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            HGDIOBJ op = SelectObject(hdc, wpen);
            Ellipse(hdc, mx - 5, my - 5, mx + 5, my + 5);
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(wpen);
        }

        // Превью текущего цвета.
        {
            HBRUSH br = CreateSolidBrush(RgbInt(rgb));
            HPEN pen = CreatePen(PS_SOLID, 1,
                st->dark ? RGB(63, 63, 63) : RGB(229, 229, 229));
            HGDIOBJ ob = SelectObject(hdc, br);
            HGDIOBJ op = SelectObject(hdc, pen);
            RoundRect(hdc, st->rcPreview.left, st->rcPreview.top,
                st->rcPreview.right, st->rcPreview.bottom, 8, 8);
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(br);
            DeleteObject(pen);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_NCHITTEST:
    {
        if (!st) break;
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hWnd, &pt);
        if (PtInRect(&st->rcClose, pt)) return HTCLIENT; // крестик — кликаем сами
        if (pt.y < st->rcTitle.bottom) return HTCAPTION; // шапка — перетаскивание
        return HTCLIENT;
    }

    case WM_EXITSIZEMOVE:
    {
        // Конец перетаскивания за шапку: фон привязан к экранным координатам,
        // на новом месте нужен свежий захват, иначе стекло врёт.
        if (!st) break;
        if (st->renderer && st->hwnd && IsWindow(st->hwnd))
        {
            // Прячемся на время захвата: иначе в фон попадём мы сами
            // (на открытии этой проблемы нет — окно тогда ещё скрыто).
            ShowWindow(st->hwnd, SW_HIDE);
            HMODULE hDwm = GetModuleHandleW(L"dwmapi.dll");
            if (hDwm)
            {
                using FnFlush = HRESULT(WINAPI*)();
                auto fnFlush = (FnFlush)GetProcAddress(hDwm, "DwmFlush");
                if (fnFlush) fnFlush();
            }
            RECT wr = {};
            GetWindowRect(st->hwnd, &wr);
            RECT cl = {};
            GetClientRect(st->hwnd, &cl);
            HBITMAP fresh = st->renderer->RenderFrostedBackground(
                wr.left, wr.top, cl.right, cl.bottom, st->dark);
            ShowWindow(st->hwnd, SW_SHOW);
            SetFocus(st->hwnd);
            if (fresh)
            {
                if (st->hBg) DeleteObject(st->hBg);
                st->hBg = fresh;
            }
            // Со стиранием: иначе старый фон проступит из-под нового.
            InvalidateRect(st->hwnd, nullptr, TRUE);
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        if (!st) break;
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        int hov = HitButton(st, pt);
        if (hov != st->hoverBtn)
        {
            st->hoverBtn = hov;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        bool onClose = PtInRect(&st->rcClose, pt) != FALSE;
        if (onClose != st->closeHover)
        {
            st->closeHover = onClose;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (st->drag == 1)
        {
            double hue;
            if (HueFromPoint(st, pt, hue))
            {
                st->hsv.h = hue;
                RebuildSquare(st);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        }
        else if (st->drag == 2)
        {
            double s, v;
            if (SvFromPoint(st, pt, s, v))
            {
                st->hsv.s = s;
                st->hsv.v = v;
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (st && (st->hoverBtn != 0 || st->closeHover) && st->drag == 0)
        {
            st->hoverBtn = 0;
            st->closeHover = false;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        if (!st) break;
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        if (PtInRect(&st->rcClose, pt))
        {
            st->closePressed = true;
            SetCapture(hWnd);
            SetFocus(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        int b = HitButton(st, pt);
        if (b)
        {
            st->pressedBtn = b;
            SetCapture(hWnd);
            SetFocus(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        double hue;
        if (HueFromPoint(st, pt, hue))
        {
            st->hsv.h = hue;
            st->drag = 1;
            SetCapture(hWnd);
            SetFocus(hWnd);
            RebuildSquare(st);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        double s, v;
        if (SvFromPoint(st, pt, s, v))
        {
            st->hsv.s = s;
            st->hsv.v = v;
            st->drag = 2;
            SetCapture(hWnd);
            SetFocus(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        SetFocus(hWnd);
        return 0;
    }

    case WM_LBUTTONUP:
    {
        if (!st) break;
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        if (st->closePressed)
        {
            st->closePressed = false;
            if (GetCapture() == hWnd) ReleaseCapture();
            if (PtInRect(&st->rcClose, pt))
            {
                DoColorCancel(st);
                return 0;
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        int wasPressed = st->pressedBtn;
        st->pressedBtn = 0;
        st->drag = 0;
        if (GetCapture() == hWnd) ReleaseCapture();
        if (wasPressed && HitButton(st, pt) == wasPressed)
        {
            if (wasPressed == 1) { DoColorOk(st); return 0; }
            DoColorCancel(st);
            return 0;
        }
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_COMMAND:
    {
        // Клавиатура на кнопках (Tab + Space/Enter через IsDialogMessage).
        if (LOWORD(wParam) == IDC_COLOR_OK) { DoColorOk(st); return 0; }
        if (LOWORD(wParam) == IDC_COLOR_CANCEL) { DoColorCancel(st); return 0; }
        break;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lParam;
        if (di && st)
        {
            bool accent = (di->CtlID == (UINT)IDC_COLOR_OK);
            bool hover = (st->hoverBtn == (di->CtlID == (UINT)IDC_COLOR_OK ? 1 : 2));
            bool pressed = (st->pressedBtn == (di->CtlID == (UINT)IDC_COLOR_OK ? 1 : 2));
            bool focused = (di->itemState & ODS_FOCUS) != 0;
            WCHAR text[32] = {};
            GetWindowTextW(di->hwndItem, text, _countof(text));
            // Шрифт по умолчанию у кнопок уже стоит; рисуем напрямую.
            HDC hdc = di->hDC;
            RECT rc = di->rcItem;
            COLORREF accentCol = RGB(0, 103, 192);
            {
                HMODULE hDwm = GetModuleHandleW(L"dwmapi.dll");
                if (hDwm)
                {
                    DWORD col = 0;
                    BOOL opq = FALSE;
                    using FnC = HRESULT(WINAPI*)(DWORD*, BOOL*);
                    auto fnC = (FnC)GetProcAddress(hDwm, "DwmGetColorizationColor");
                    if (fnC && SUCCEEDED(fnC(&col, &opq)))
                        accentCol = RGB((col >> 16) & 255, (col >> 8) & 255, col & 255);
                }
            }
            COLORREF fill, edge, txt;
            if (accent)
            {
                fill = accentCol;
                if (pressed) fill = RGB(GetRValue(fill) * 65 / 100,
                    GetGValue(fill) * 65 / 100, GetBValue(fill) * 65 / 100);
                else if (hover) fill = RGB(min(255, GetRValue(fill) + 35),
                    min(255, GetGValue(fill) + 35), min(255, GetBValue(fill) + 35));
                edge = fill;
                txt = RGB(255, 255, 255);
            }
            else
            {
                fill = st->dark ? RGB(45, 45, 45) : RGB(255, 255, 255);
                edge = st->dark ? RGB(63, 63, 63) : RGB(229, 229, 229);
                txt = st->dark ? RGB(255, 255, 255) : RGB(27, 27, 27);
            }
            HBRUSH br = CreateSolidBrush(fill);
            HPEN pen = CreatePen(PS_SOLID, 1, edge);
            // Углы вне скругления заливаем фоном диалога, иначе там белое.
            HBRUSH bg = CreateSolidBrush(st->dark ? RGB(32, 32, 32) : RGB(243, 243, 243));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            HGDIOBJ ob = SelectObject(hdc, br);
            HGDIOBJ op = SelectObject(hdc, pen);
            RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(br);
            DeleteObject(pen);
            if (focused)
            {
                HPEN fpen = CreatePen(PS_SOLID, 2, accent
                    ? RGB(min(255, GetRValue(accentCol) + 120), min(255, GetGValue(accentCol) + 120), min(255, GetBValue(accentCol) + 120))
                    : accentCol);
                HGDIOBJ fb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                HGDIOBJ fp = SelectObject(hdc, fpen);
                RoundRect(hdc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1, 8, 8);
                SelectObject(hdc, fb);
                SelectObject(hdc, fp);
                DeleteObject(fpen);
            }
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, txt);
            DrawTextW(hdc, text, -1, const_cast<RECT*>(&rc),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
        DoColorCancel(st);
        return 0;

    case WM_DESTROY:
        // БЕЗ PostQuitMessage: цикл тут вложенный модальный (выход — по done),
        // а WM_QUIT отравил бы все внешние циклы вплоть до главного —
        // приложение бы тихо завершилось (так и было по «Отмене»).
        return 0;

    case WM_NCDESTROY:
        // Память чистит ShowColorDialog после цикла; здесь только отвязка.
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

bool ShowColorDialog(HWND parent, WidgetRenderer* renderer, int initialRgb, int& outRgb)
{
    static bool clsRegistered = false;
    if (!clsRegistered)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = ColorDlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = COLOR_DLG_CLASS;
        wc.hbrBackground = nullptr;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        clsRegistered = true;
    }

    ColorDlg* st = new ColorDlg();
    ColorHsv rgbInit = RgbToHsv(initialRgb);
    st->hsv = rgbInit;
    st->renderer = renderer;

    HWND hWnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_TOPMOST,
        COLOR_DLG_CLASS, Lang::Get(Str::D_ColorTitle),
        WS_POPUP | WS_SYSMENU | WS_CLIPCHILDREN,
        CW_USEDEFAULT, 0, 100, 100,
        parent, nullptr, GetModuleHandleW(nullptr), st);
    if (!hWnd) { delete st; return false; }
    {
        // Иконка приложения для таскбара/Alt+Tab.
        HICON hAppIcon = LoadIconW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_APP_ICON));
        if (hAppIcon)
        {
            SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hAppIcon);
            SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hAppIcon);
        }
    }

    // Центр экрана (пока скрыто).
    RECT wr = {};
    GetWindowRect(hWnd, &wr);
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    int posX = work.left + (work.right - work.left - ww) / 2;
    int posY = work.top + (work.bottom - work.top - wh) / 2;
    SetWindowPos(hWnd, nullptr, posX, posY, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // Фон-стекло захватываем здесь: окно спозиционировано, но ещё скрыто —
    // само в кадр не попадёт. При перетаскивании фон не обновляем (блюр прячет).
    if (renderer)
    {
        RECT cl = {};
        GetClientRect(hWnd, &cl);
        st->hBg = renderer->RenderFrostedBackground(posX, posY,
            cl.right, cl.bottom, st->dark);
    }
    SetWindowPos(hWnd, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetFocus(hWnd);

    MSG msg;
    while (!st->done && GetMessageW(&msg, nullptr, 0, 0))
    {
        if (!IsWindow(hWnd)) break;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
        {
            st->done = true;
            DestroyWindow(hWnd);
            break;
        }
        if (IsDialogMessageW(hWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // Снимаем результат ДО уничтожения окна (память чистим сами).
    bool ok = st->confirmed;
    int rgb = 0;
    if (ok)
        rgb = HsvToRgb(st->hsv.h, st->hsv.s, st->hsv.v);
    if (IsWindow(hWnd)) DestroyWindow(hWnd);
    if (st->hWheel) DeleteObject(st->hWheel);
    if (st->hSquare) DeleteObject(st->hSquare);
    if (st->hBg) DeleteObject(st->hBg);
    if (st->hTitleFont) DeleteObject(st->hTitleFont);
    if (st->hCloseGlyph) DeleteObject(st->hCloseGlyph);
    delete st;
    if (ok) outRgb = rgb;
    return ok;
}
