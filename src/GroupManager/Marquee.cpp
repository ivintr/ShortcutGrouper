// Marquee: резиновая рамка по пустому месту рабочего стола (было в DesktopWidget.cpp).
#include "Marquee.h"
#include "Logger.h"
#include "DesktopWidget.h"
#include "DesktopGrid.h"
#include "WidgetManager.h"
#include <cstring>

static WidgetManager* s_marqueeManager = nullptr;

// ---------------------------------------------------------------------------
// Marquee: резиновая рамка по пустому месту рабочего стола.
// Драг по пустому столу выделяет группы внутри рамки (как в проводнике),
// клик без движения — снимает выделение. Работает параллельно с родной
// рамкой проводника (та выделяет иконки, наша — виджеты).
// Хук постоянный, но лёгкий: вне зажатой левой кнопки сразу дальше.
// Тяжёлая работа (хит-тесты, перерисовка) — в окне marquee через PostMessage.
// ---------------------------------------------------------------------------

static const wchar_t* MARQUEE_CLASS = L"DesktopGroupMarquee";
static const UINT WM_APP_MQ_DOWN = WM_APP + 10;
static const UINT WM_APP_MQ_MOVE = WM_APP + 11;
static const UINT WM_APP_MQ_UP   = WM_APP + 12;
static const int MQ_DRAG_THRESHOLD = 4;

static HHOOK s_hMarqueeHook = nullptr;
static HWND s_hMarqueeWnd = nullptr;
static bool s_mqBtnDown = false;    // зажата левая кнопка (состояние хука)
static bool s_mqEmptyDown = false;  // нажатие было по пустому месту стола
static bool s_mqOnOurs = false;     // нажатие было по нашим окнам
static bool s_mqActive = false;     // рамка рисуется
static POINT s_mqAnchor = {};
static POINT s_mqCurrent = {};
// Слепок иконок в момент нажатия: если к активации позиции поплыли
// (Explorer оселяет после перемещения файлов) — жест отменяем.
static std::vector<RECT> s_mqDownIcons;

static POINT PointFromLParam(LPARAM lp)
{
    POINT pt = {};
    memcpy(&pt, &lp, sizeof(pt));
    return pt;
}

static LPARAM PointToLParam(POINT pt)
{
    LPARAM lp = 0;
    memcpy(&lp, &pt, sizeof(lp));
    return lp;
}

// Курсор над нашими окнами (виджеты + попапы)?
static bool PointInOurWindows(POINT pt)
{
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, WIDGET_CLASS, nullptr)) != nullptr)
    {
        if (!IsWindowVisible(h)) continue;
        RECT wr = {};
        GetWindowRect(h, &wr);
        if (PtInRect(&wr, pt)) return true;
    }
    h = nullptr;
    while ((h = FindWindowExW(nullptr, h, POPUP_CLASS, nullptr)) != nullptr)
    {
        if (!IsWindowVisible(h)) continue;
        RECT wr = {};
        GetWindowRect(h, &wr);
        if (PtInRect(&wr, pt)) return true;
    }
    return false;
}

// Десктопный ли это вид: поднимаемся к топу, требуем Progman/WorkerW.
// БЕЗ этой проверки ЛЮБОЙ проводник подходил: у каждой папки свой
// SHELLDLL_DefView + SysListView32, и нажатие на файл внутри проводника
// считалось «пустым местом стола» — рамка рисовалась при драге файлов.
static bool IsDesktopView(HWND hListOrDef)
{
    HWND h = hListOrDef;
    while (h)
    {
        wchar_t cls[64] = {};
        if (GetClassNameW(h, cls, _countof(cls)) == 0)
            return false;
        if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW"))
            return true;
        HWND par = GetParent(h);
        if (!par || par == h)
            return false; // топ — не стол (проводник и т.д.)
        h = par;
    }
    return false;
}

// Курсор над пустым местом стола: не наши окна, не иконки файлов,
// окно под курсором — десктопный вид (ListView/DefView под Progman/WorkerW)?
// Опрос иконок может провалиться (Explorer подвис) — тогда считаем НЕ пустым:
// ложное «пусто» вооружает рамку на нажатии по файлу, и драг файлов рисует её.
static bool PointOnDesktopEmpty(POINT pt)
{
    if (PointInOurWindows(pt))
        return false;
    bool iconsValid = false;
    std::vector<RECT> icons = QueryDesktopIconRects(&iconsValid);
    if (!iconsValid)
        return false;
    for (const RECT& ic : icons)
    {
        RECT r = ic;
        InflateRect(&r, -6, -6);
        if ((r.right > r.left && r.bottom > r.top && PtInRect(&r, pt)) ||
            PtInRect(&ic, pt))
            return false;
    }
    HWND h = WindowFromPoint(pt);
    while (h)
    {
        wchar_t cls[64] = {};
        GetClassNameW(h, cls, _countof(cls));
        if (!wcscmp(cls, L"SysListView32"))
            return IsDesktopView(h);
        if (!wcscmp(cls, L"SHELLDLL_DefView"))
            return IsDesktopView(h);
        HWND par = GetParent(h);
        if (!par)
            return !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW");
        h = par;
    }
    return false;
}

static void MarqueeRect(RECT& out)
{
    out.left = s_mqAnchor.x < s_mqCurrent.x ? s_mqAnchor.x : s_mqCurrent.x;
    out.top = s_mqAnchor.y < s_mqCurrent.y ? s_mqAnchor.y : s_mqCurrent.y;
    out.right = s_mqAnchor.x > s_mqCurrent.x ? s_mqAnchor.x : s_mqCurrent.x;
    out.bottom = s_mqAnchor.y > s_mqCurrent.y ? s_mqAnchor.y : s_mqCurrent.y;
}

// Виджеты, пересекающие рамку, — в выделение (живьём во время драга).
static void UpdateMarqueeSelection()
{
    if (!s_marqueeManager) return;
    RECT sel = {};
    MarqueeRect(sel);
    std::vector<std::wstring> ids;
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, WIDGET_CLASS, nullptr)) != nullptr)
    {
        if (!IsWindowVisible(h)) continue;
        RECT wr = {};
        GetWindowRect(h, &wr);
        RECT inter = {};
        if (!IntersectRect(&inter, &sel, &wr)) continue;
        // USERDATA валидируем: окно могли снести/пересоздать, тогда там мусор.
        if (!IsWindow(h)) continue;
        WCHAR cls[64] = {};
        if (GetClassNameW(h, cls, 64) == 0 || wcscmp(cls, WIDGET_CLASS) != 0)
            continue;
        DesktopWidget* w = (DesktopWidget*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (w && !w->GetGroupId().empty())
            ids.push_back(w->GetGroupId());
    }
    s_marqueeManager->SetSelectedGroups(ids);
}

// Отрисовка рамки: полупрозрачная заливка + яркая граница (как в проводнике).
static void MarqueePaint()
{
    if (!s_hMarqueeWnd) return;
    RECT client = {};
    GetClientRect(s_hMarqueeWnd, &client);
    int W = client.right - client.left;
    int H = client.bottom - client.top;
    if (W <= 0 || H <= 0) return;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return;
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) { ReleaseDC(nullptr, hdcScreen); return; }
    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBmp)
    {
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return;
    }
    HBITMAP hOld = nullptr;
    if (hBmp) hOld = (HBITMAP)SelectObject(hdcMem, hBmp);
    if (bits) memset(bits, 0, (size_t)W * H * 4);

    if (bits && s_mqActive)
    {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        RECT r = {};
        MarqueeRect(r);
        OffsetRect(&r, -vx, -vy);
        if (r.left < 0) r.left = 0;
        if (r.top < 0) r.top = 0;
        if (r.right > W) r.right = W;
        if (r.bottom > H) r.bottom = H;

        // Заливка rgba(51,153,255,40), граница rgba(51,153,255,220), premultiplied.
        auto blendPx = [](BYTE fr, BYTE fg, BYTE fb, BYTE fa) -> DWORD {
            BYTE r = (BYTE)(fr * fa / 255);
            BYTE g = (BYTE)(fg * fa / 255);
            BYTE b = (BYTE)(fb * fa / 255);
            return (DWORD)fa << 24 | (DWORD)r << 16 | (DWORD)g << 8 | b;
        };
        DWORD fill = blendPx(51, 153, 255, 40);
        DWORD edge = blendPx(51, 153, 255, 220);
        DWORD* px = (DWORD*)bits;
        for (int y = r.top; y < r.bottom; y++)
        {
            for (int x = r.left; x < r.right; x++)
            {
                bool border = (y == r.top || y == r.bottom - 1 ||
                               x == r.left || x == r.right - 1);
                px[y * W + x] = border ? edge : fill;
            }
        }
    }

    POINT ptDst = { 0, 0 };
    RECT wndRc = {};
    GetWindowRect(s_hMarqueeWnd, &wndRc);
    ptDst.x = wndRc.left;
    ptDst.y = wndRc.top;
    SIZE sizeWnd = { W, H };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    if (hBmp)
        UpdateLayeredWindow(s_hMarqueeWnd, hdcScreen, &ptDst, &sizeWnd,
            hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    if (hBmp) SelectObject(hdcMem, hOld);
    if (hBmp) DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

static void MarqueeShow(bool show)
{
    if (!s_hMarqueeWnd) return;
    // Окно — на весь виртуальный экран (на случай смены мониторов).
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    SetWindowPos(s_hMarqueeWnd, HWND_TOPMOST, vx, vy, vw, vh,
        SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    ShowWindow(s_hMarqueeWnd, show ? SW_SHOWNA : SW_HIDE);
    if (show) MarqueePaint();
}

static LRESULT CALLBACK MarqueeWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_APP_MQ_DOWN:
    {
        POINT pt = PointFromLParam(lParam);
        // Залипшая рамка от прошлого жеста (пропущенный UP) — гасим сразу,
        // иначе новый жест пляшет от мёртвого якоря.
        if (s_mqActive)
        {
            s_mqActive = false;
            MarqueeShow(false);
        }
        s_mqOnOurs = PointInOurWindows(pt);
        s_mqEmptyDown = false;
        if (!s_mqOnOurs && PointOnDesktopEmpty(pt))
        {
            // Кэш позиций иконок живёт 3с: сразу после перемещения файлов
            // нажатие на файл считается «пустым местом», и последующий драг
            // файлов рисует рамку поверх окон. Перепроверяем по свежим данным.
            InvalidateDesktopIconCache();
            if (PointOnDesktopEmpty(pt))
            {
                s_mqEmptyDown = true;
                s_mqAnchor = pt;
                s_mqCurrent = pt;
                // Слепок для проверки сходимости на активации.
                bool dv = false;
                s_mqDownIcons = QueryDesktopIconRects(&dv);
                if (!dv) s_mqDownIcons.clear();
            }
        }
        return 0;
    }
    case WM_APP_MQ_MOVE:
    {
        if (!s_mqEmptyDown) return 0;
        // Схлопываем очередь движений: при быстром ведении мыши хук шлёт
        // сотни MOVE, обрабатываем только самое свежее положение, иначе
        // перерисовка и хит-тесты отстают от курсора.
        POINT pt = PointFromLParam(lParam);
        MSG coalesce = {};
        while (PeekMessageW(&coalesce, hWnd, WM_APP_MQ_MOVE, WM_APP_MQ_MOVE, PM_REMOVE))
            pt = PointFromLParam(coalesce.lParam);
        // Рассинхрон с физической кнопкой (UP потерян — хук сняли/лаг):
        // иначе фантомная рамка ездит за курсором без зажатой кнопки.
        if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
        {
            s_mqBtnDown = false;
            s_mqEmptyDown = false;
            if (s_mqActive)
            {
                s_mqActive = false;
                MarqueeShow(false);
            }
            return 0;
        }
        // pt уже взят выше (схлопнутый из очереди).
        // За пределы стола/наших окон рамку не тянем — замораживаем:
        // иначе при перетаскивании она рисуется поверх проводника.
        if (!PointInOurWindows(pt) && !PointOnDesktopEmpty(pt))
            pt = s_mqCurrent;
        if (!s_mqActive)
        {
            if (abs(pt.x - s_mqAnchor.x) <= MQ_DRAG_THRESHOLD &&
                abs(pt.y - s_mqAnchor.y) <= MQ_DRAG_THRESHOLD)
                return 0;
            // Якорь мог evaluated-устареть: ListView отдаёт «успешные», но ещё
            // не осевшие позиции сразу после перемещения файлов. Перед показом
            // рамки перепроверяем якорь по свежим данным: если там иконка —
            // это был драг файла, жест отменяем целиком.
            InvalidateDesktopIconCache();
            bool av = false;
            std::vector<RECT> ai = QueryDesktopIconRects(&av);
            bool anchorOnIcon = false;
            bool settled = false;
            if (av)
            {
                // Сходимость со слепком нажатия: позиции поплыли — Explorer
                // ещё оселяет иконки после перемещения, жест мог стартовать
                // на файле. Отмена вместо рамки.
                settled = (ai.size() == s_mqDownIcons.size());
                if (settled)
                {
                    for (size_t k = 0; k < ai.size(); k++)
                    {
                        if (memcmp(&ai[k], &s_mqDownIcons[k], sizeof(RECT)) != 0)
                        {
                            settled = false;
                            break;
                        }
                    }
                }
                for (const RECT& ic : ai)
                {
                    RECT r = ic;
                    InflateRect(&r, -6, -6);
                    if ((r.right > r.left && r.bottom > r.top &&
                            PtInRect(&r, s_mqAnchor)) ||
                        PtInRect(&ic, s_mqAnchor))
                    {
                        anchorOnIcon = true;
                        break;
                    }
                }
            }
            if (!av || !settled || anchorOnIcon)
            {
                s_mqEmptyDown = false;
                s_mqDownIcons.clear();
                return 0;
            }
            s_mqDownIcons.clear();
            s_mqActive = true;
            MarqueeShow(true);
        }
        s_mqCurrent = pt;
        MarqueePaint();
        UpdateMarqueeSelection();
        return 0;
    }
    case WM_APP_MQ_UP:
    {
        if (s_mqActive)
        {
            s_mqActive = false;
            MarqueeShow(false);
            UpdateMarqueeSelection(); // финальный состав
        }
        else if (s_mqEmptyDown && s_marqueeManager)
        {
            // Клик по пустому столу без движения — снять выделение.
            s_marqueeManager->ClearSelection();
        }
        s_mqEmptyDown = false;
        s_mqOnOurs = false;
        s_mqDownIcons.clear();
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        MarqueePaint();
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DISPLAYCHANGE:
        if (s_mqActive) MarqueeShow(true);
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static LRESULT CALLBACK MarqueeHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && s_hMarqueeWnd)
    {
        if (wParam == WM_LBUTTONDOWN)
        {
            s_mqBtnDown = true;
            MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
            if (p) PostMessageW(s_hMarqueeWnd, WM_APP_MQ_DOWN, 0, PointToLParam(p->pt));
        }
        else if (wParam == WM_LBUTTONUP)
        {
            if (s_mqBtnDown)
            {
                s_mqBtnDown = false;
                MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
                if (p) PostMessageW(s_hMarqueeWnd, WM_APP_MQ_UP, 0, PointToLParam(p->pt));
            }
        }
        else if (wParam == WM_MOUSEMOVE && s_mqBtnDown)
        {
            MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
            if (p) PostMessageW(s_hMarqueeWnd, WM_APP_MQ_MOVE, 0, PointToLParam(p->pt));
        }
    }
    return CallNextHookEx(s_hMarqueeHook, nCode, wParam, lParam);
}

void MarqueeInit(WidgetManager* manager)
{
    s_marqueeManager = manager;
    if (!s_hMarqueeWnd)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = MarqueeWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = MARQUEE_CLASS;
        wc.hbrBackground = nullptr;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (RegisterClassExW(&wc) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return;

        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        s_hMarqueeWnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            MARQUEE_CLASS, L"", WS_POPUP,
            vx, vy, vw, vh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!s_hMarqueeWnd) return;
    }
    if (!s_hMarqueeHook)
        s_hMarqueeHook = SetWindowsHookExW(WH_MOUSE_LL, MarqueeHookProc, nullptr, 0);
}

void MarqueeShutdown()
{
    s_mqActive = false;
    s_mqBtnDown = false;
    s_mqEmptyDown = false;
    if (s_hMarqueeHook)
    {
        UnhookWindowsHookEx(s_hMarqueeHook);
        s_hMarqueeHook = nullptr;
    }
    if (s_hMarqueeWnd)
    {
        DestroyWindow(s_hMarqueeWnd);
        s_hMarqueeWnd = nullptr;
    }
}
