#include "ModernMenu.h"
#include "Renderer.h"
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

ModernMenu* ModernMenu::s_modalRoot = nullptr;
HHOOK ModernMenu::s_mouseHook = nullptr;
HHOOK ModernMenu::s_kbdHook = nullptr;

static const wchar_t* MENU_CLASS = L"ModernGlassMenuWin11";

ModernMenu::ModernMenu(WidgetRenderer* renderer) : m_renderer(renderer) {}

bool ModernMenuIsDarkMode()
{
    DWORD v = 1;
    DWORD cb = sizeof(v);
    DWORD type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_DWORD, &type, &v, &cb) == ERROR_SUCCESS)
        return v == 0;
    return false;
}

bool ModernMenu::ReadDarkMode()
{
    return ModernMenuIsDarkMode();
}

// Конвертация пунктов в структуры рендера (единый формат Renderer).
static std::vector<WidgetRenderer::MenuRenderItem> ToRenderItems(
    const std::vector<ModernMenuItem>& items)
{
    std::vector<WidgetRenderer::MenuRenderItem> ri;
    ri.reserve(items.size());
    for (const auto& it : items)
    {
        WidgetRenderer::MenuRenderItem r;
        r.text = it.text;
        r.checked = it.checked;
        r.disabled = it.disabled || it.title;
        r.separator = it.separator;
        r.hasSubmenu = (it.submenu != nullptr);
        r.swatch = it.swatch;
        r.isTitle = it.title;
        r.icon = it.icon;
        r.shortcut = it.shortcut;
        r.glyph = it.glyph;
        ri.push_back(r);
    }
    return ri;
}

static void DoUpdateLayeredMenu(HWND hwnd, HBITMAP hBitmap, int w, int h, BYTE alpha = 255)
{
    if (!hwnd || !hBitmap) return;
    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return;
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) { ReleaseDC(nullptr, hdcScreen); return; }
    HGDIOBJ hOld = SelectObject(hdcMem, hBitmap);
    if (!hOld) { DeleteDC(hdcMem); ReleaseDC(nullptr, hdcScreen); return; }
    POINT ptSrc = { 0, 0 };
    SIZE sizeWnd = { w, h };
    RECT rcWnd;
    GetWindowRect(hwnd, &rcWnd);
    POINT ptDst = { rcWnd.left, rcWnd.top };
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = alpha;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc,
        0, &blend, ULW_ALPHA);
    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

static void RegisterMenuClass()
{
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc = { sizeof(wc) };
    // БЕЗ CS_DROPSHADOW: тень у нас своя мягкая (Renderer), а системная
    // на части сборок даёт светлый ободок по краю окна.
    wc.lpfnWndProc = ModernMenu::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = MENU_CLASS;
    wc.hbrBackground = nullptr;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
}

static RECT MonitorWorkRectForPoint(POINT pt)
{
    RECT work = {};
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (hMon)
    {
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) work = mi.rcWork;
    }
    if (work.bottom <= work.top)
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

bool ModernMenu::IsSelectable(int index) const
{
    if (index < 0 || index >= (int)items.size()) return false;
    const auto& it = items[index];
    if (it.separator || it.disabled || it.title) return false;
    // Родитель подменю выбирается (открывается), даже с id == 0:
    // ActivateHovered/OnLButtonUp всё равно уходят в подменю, а не в CloseTree.
    return it.id != 0 || it.submenu != nullptr;
}

bool ModernMenu::IsCommandSelectable(int index) const
{
    if (index < 0 || index >= (int)commands.size()) return false;
    const auto& c = commands[index];
    return !c.disabled && c.id != 0;
}

// Смещение списка пунктов: тень + командная панель + разделитель под ней.
int ModernMenu::ItemsTop() const
{
    int top = WidgetRenderer::MENU_SHADOW + WidgetRenderer::MENU_PAD;
    if (commands.empty()) return top;
    return top + WidgetRenderer::MENU_CMD_H + WidgetRenderer::MENU_SEP_H;
}

int ModernMenu::HitTest(int y) const
{
    int yy = ItemsTop();
    for (int i = 0; i < (int)items.size(); i++)
    {
        int h;
        if (items[i].separator)
            h = WidgetRenderer::MENU_SEP_H;
        else if (items[i].title)
            h = WidgetRenderer::MENU_TITLE_H;
        else
            h = WidgetRenderer::MENU_ITEM_H;
        if (y >= yy && y < yy + h) return i;
        yy += h;
    }
    return -1;
}

int ModernMenu::HitTestCmd(int x, int y) const
{
    if (commands.empty()) return -1;
    x -= WidgetRenderer::MENU_SHADOW;
    y -= WidgetRenderer::MENU_SHADOW;
    int top = WidgetRenderer::MENU_PAD;
    if (y < top || y >= top + WidgetRenderer::MENU_CMD_H) return -1;
    int n = (int)commands.size();
    if (n <= 0 || m_menuW <= 0) return -1;
    // Та же геометрия, что в Renderer: поля по 4px, ячейки поровну
    // от ширины КОНТЕНТА (без тени).
    float contentW = (float)m_menuW - 2.0f * (float)WidgetRenderer::MENU_SHADOW;
    float cellW = (contentW - 8.0f) / (float)n;
    if (cellW <= 0) return -1;
    int idx = (int)(((float)x - 4.0f) / cellW);
    if (idx < 0 || idx >= n) return -1;
    return idx;
}

void ModernMenu::Render()
{
    if (!m_renderer || !m_hwnd) return;
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    auto rcCmd = ToRenderItems(commands);
    auto ri = ToRenderItems(items);
    auto result = m_renderer->RenderMenu(rcCmd, ri, m_hovered, m_pressed,
        m_cmdHovered, m_cmdPressed, rc.left, rc.top, m_dark);
    if (m_hBitmap) DeleteObject(m_hBitmap);
    m_hBitmap = result.hBitmap;
    m_menuW = result.width;
    m_menuH = result.height;
    if (m_hwnd && m_hBitmap)
        DoUpdateLayeredMenu(m_hwnd, m_hBitmap, result.width, result.height,
            (BYTE)(m_animAlpha < 0 ? 0 : (m_animAlpha > 255 ? 255 : m_animAlpha)));
}

void ModernMenu::PositionIntoWorkArea(int* px, int* py, int w, int h)
{
    POINT pt = { *px, *py };
    RECT work = MonitorWorkRectForPoint(pt);
    int x = *px, y = *py;
    if (x + w > work.right - 4) x = work.right - w - 4;
    if (y + h > work.bottom - 4) y = work.bottom - h - 4;
    if (x < work.left + 4) x = work.left + 4;
    if (y < work.top + 4) y = work.top + 4;
    *px = x; *py = y;
}

void ModernMenu::ApplyWin11Style()
{
    if (!m_hwnd) return;
    HMODULE hDwm = GetModuleHandleW(L"dwmapi.dll");
    if (!hDwm) return;
    using FnAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = (FnAttr)GetProcAddress(hDwm, "DwmSetWindowAttribute");
    if (!fn) return;

    // Углы у нас свои (скруглённый контент + прозрачные поля под тень
    // в самом битмапе) — DWM-скругление НЕ просим: за ним DWM рисует
    // свою светлую рамку 1px по периметру окна (= видимое кольцо).
    int corner = DWMWCP_DONOTROUND;
    fn(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    // Тёмный / светлый хром окна (тон тени) под тему.
    BOOL dark = m_dark ? TRUE : FALSE;
    fn(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    // NOTE: системный backdrop (Mica/Acrylic) и DWMWA_BORDER_COLOR НЕ ставим:
    // DWM заливает ими и прозрачные поля под нашу мягкую тень — получается
    // толстая серая рамка вокруг меню. Стекло, тень и тонкая рамка у нас
    // свои, рисуются в Renderer::RenderMenu.
}

void ModernMenu::DestroySelf()
{
    if (m_hwnd)
        KillTimer(m_hwnd, 1);
    if (m_parent && m_parent->m_child == this)
    {
        m_parent->m_child = nullptr;
        m_parent->m_childFor = -1;
    }
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_hBitmap)
    {
        DeleteObject(m_hBitmap);
        m_hBitmap = nullptr;
    }
    m_child = nullptr;
    m_childFor = -1;
}

void ModernMenu::DestroyTreeWindows()
{
    if (m_child) m_child->DestroyTreeWindows();
    m_child = nullptr;
    m_childFor = -1;
    DestroySelf();
}

void ModernMenu::CloseTree(int result)
{
    ModernMenu* r = this;
    while (r->m_parent) r = r->m_parent;
    r->m_result = result;
    r->m_done = true;
    // Сначала будим модальный цикл, окна снесём в Show() после выхода.
    PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
}

ModernMenu* ModernMenu::Deepest()
{
    ModernMenu* m = this;
    while (m->m_child && m->m_child->m_hwnd) m = m->m_child;
    return m;
}

bool ModernMenu::PointInTree(POINT pt) const
{
    if (m_hwnd)
    {
        RECT rc;
        GetWindowRect(m_hwnd, &rc);
        if (PtInRect(&rc, pt)) return true;
    }
    if (m_child) return m_child->PointInTree(pt);
    return false;
}

void ModernMenu::CloseChild()
{
    if (m_child)
    {
        m_child->DestroyTreeWindows();
        m_child = nullptr;
        m_childFor = -1;
    }
}

void ModernMenu::OpenChildFor(int index)
{
    if (index < 0 || index >= (int)items.size()) return;
    ModernMenu* sub = items[index].submenu;
    if (!sub || !m_renderer) return;
    if (m_child == sub && m_childFor == index) return;
    CloseChild();
    m_child = sub;
    m_childFor = index;
    sub->m_parent = this;
    sub->m_dark = m_dark;
    sub->m_renderer = m_renderer;

    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    auto subCmd = ToRenderItems(sub->commands);
    auto ri = ToRenderItems(sub->items);
    SIZE sz = m_renderer->MeasureMenu(subCmd, ri);
    int yy = ItemsTop();
    for (int i = 0; i < index; i++)
    {
        if (items[i].separator) yy += WidgetRenderer::MENU_SEP_H;
        else if (items[i].title) yy += WidgetRenderer::MENU_TITLE_H;
        else yy += WidgetRenderer::MENU_ITEM_H;
    }
    int x = rc.right - 4;
    int y = rc.top + yy - 4;
    RECT work = MonitorWorkRectForPoint({ x, y });
    if (x + sz.cx > work.right - 4) x = rc.left - sz.cx + 4;
    if (y + sz.cy > work.bottom - 4) y = work.bottom - sz.cy - 4;
    if (y < work.top + 4) y = work.top + 4;
    sub->CreateAndShow(x, y, sz.cx, sz.cy);
}

void ModernMenu::CreateAndShow(int x, int y, int w, int h)
{
    RegisterMenuClass();
    m_menuW = w;
    m_menuH = h;
    m_hovered = -1;
    m_pressed = -1;
    m_cmdHovered = -1;
    m_cmdPressed = -1;
    m_animAlpha = 0; // fade-in с нуля
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
    m_hwnd = CreateWindowExW(exStyle, MENU_CLASS, L"", WS_POPUP,
        x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_hwnd) return;
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
    ApplyWin11Style();
    Render();
    // Позиция могла скорректироваться после замера — показываем по факту.
    ShowWindow(m_hwnd, SW_SHOWNA);
    UpdateWindow(m_hwnd);
    // Без проверки SetTimer fade залипал на alpha=0 — сразу показываем.
    if (!SetTimer(m_hwnd, 1, 16, nullptr) && m_hwnd)
    {
        m_animAlpha = 255;
        Render();
    }
}

void ModernMenu::MoveHover(int dir)
{
    if (items.empty()) return;
    int n = (int)items.size();
    int idx = m_hovered;
    for (int k = 0; k < n; k++)
    {
        idx += dir;
        if (idx < 0) idx = n - 1;
        if (idx >= n) idx = 0;
        if (IsSelectable(idx)) break;
    }
    if (!IsSelectable(idx)) return;
    if (idx != m_hovered)
    {
        m_hovered = idx;
        m_pressed = -1;
        if (items[idx].submenu) OpenChildFor(idx);
        else CloseChild();
        Render();
    }
}

void ModernMenu::ActivateHovered()
{
    if (!IsSelectable(m_hovered)) return;
    if (items[m_hovered].submenu) { OpenChildFor(m_hovered); return; }
    int id = items[m_hovered].id;
    m_pressed = m_hovered;
    Render();
    CloseTree(id);
}

void ModernMenu::OnKey(int vk)
{
    switch (vk)
    {
    case VK_DOWN: MoveHover(1); break;
    case VK_UP: MoveHover(-1); break;
    case VK_RIGHT:
        if (m_hovered >= 0 && m_hovered < (int)items.size() &&
            items[m_hovered].submenu)
            OpenChildFor(m_hovered);
        break;
    case VK_LEFT:
        if (m_parent)
        {
            ModernMenu* parent = m_parent;
            parent->CloseChild();
            parent->Render();
            SetFocus(nullptr);
        }
        break;
    case VK_RETURN:
    case VK_SPACE:
        ActivateHovered();
        break;
    case VK_ESCAPE:
        CloseTree(0);
        break;
    }
}

void ModernMenu::OnMouseMove(int x, int y)
{
    TrackLeave();
    int hitC = HitTestCmd(x, y);
    if (hitC >= 0)
    {
        if (hitC != m_cmdHovered)
        {
            m_cmdHovered = hitC;
            m_hovered = -1;
            CloseChild();
            Render();
        }
        return;
    }
    if (m_cmdHovered != -1)
    {
        m_cmdHovered = -1;
        Render();
    }
    int hit = HitTest(y);
    if (hit != m_hovered)
    {
        m_hovered = hit;
        // m_pressed НЕ трогаем: нажатую кнопку можно увести и вернуть —
        // отпущенная над ней всё равно сработает (как в настоящих меню).
        if (hit >= 0 && hit < (int)items.size() && items[hit].submenu)
            OpenChildFor(hit);
        else
            CloseChild();
        Render();
    }
}

void ModernMenu::OnMouseLeave()
{
    POINT pt;
    GetCursorPos(&pt);
    if (m_child && m_child->m_hwnd)
    {
        RECT rc;
        GetWindowRect(m_child->m_hwnd, &rc);
        if (PtInRect(&rc, pt)) return; // ушли в открытое подменю
    }
    if (m_hovered != -1 || m_cmdHovered != -1)
    {
        m_hovered = -1;
        m_pressed = -1;
        m_cmdHovered = -1;
        m_cmdPressed = -1;
        Render();
    }
}

void ModernMenu::OnLButtonDown(int x, int y)
{
    // Захватываем мышь как настоящие меню: иначе нажатие гаснет, если
    // между down/up курсор выходит из окна (панель команд в 6px от края!).
    SetCapture(m_hwnd);
    int hitC = HitTestCmd(x, y);
    if (hitC >= 0)
    {
        m_cmdHovered = hitC;
        m_cmdPressed = IsCommandSelectable(hitC) ? hitC : -1;
        m_hovered = -1;
        m_pressed = -1;
        CloseChild();
        Render();
        return;
    }
    m_cmdPressed = -1;
    int hit = HitTest(y);
    if (hit >= 0 && hit < (int)items.size() && items[hit].submenu)
    {
        // Клик по родителю подменю — просто открыть его.
        m_hovered = hit;
        OpenChildFor(hit);
        Render();
        return;
    }
    m_pressed = (hit >= 0 && IsSelectable(hit)) ? hit : -1;
    if (m_pressed >= 0)
    {
        m_hovered = hit;
        Render();
    }
    (void)x;
}

void ModernMenu::OnLButtonUp(int x, int y)
{
    DWORD pos = GetMessagePos();
    POINT ptScreen = { (short)LOWORD(pos), (short)HIWORD(pos) };
    OnLButtonUpAt(x, y, ptScreen);
}

void ModernMenu::OnLButtonUpAt(int x, int y, POINT ptScreen)
{
    // Состояние читаем ДО ReleaseCapture: смена захвата шлёт
    // WM_CAPTURECHANGED синхронно, а его обработчик всё сбрасывает.
    int hitC = HitTestCmd(x, y);
    int cmdPressed = m_cmdPressed;
    int hit = HitTest(y);
    int pressed = m_pressed;
    m_cmdPressed = -1;
    m_pressed = -1;
    if (GetCapture() == m_hwnd) ReleaseCapture();
    if (cmdPressed >= 0)
    {
        if (hitC == cmdPressed && IsCommandSelectable(cmdPressed))
        {
            int id = commands[cmdPressed].id;
            Render();
            CloseTree(id);
            return;
        }
    }
    else if (pressed >= 0 && hit == pressed && IsSelectable(pressed) &&
        !items[pressed].submenu)
    {
        int id = items[pressed].id;
        Render();
        CloseTree(id);
        return;
    }
    // Иначе решает место отпускания (press-drag-release через окна дерева,
    // микродрожание, отпуск после увода и возврата) — как в настоящих меню.
    if (!ActivateAt(ptScreen))
        Render();
    (void)x;
}

bool ModernMenu::ActivateAt(POINT ptScreen)
{
    if (!m_hwnd || !IsWindow(m_hwnd)) return false;
    POINT pt = ptScreen;
    if (!ScreenToClient(m_hwnd, &pt)) return false;
    int hitC = HitTestCmd(pt.x, pt.y);
    if (hitC >= 0 && IsCommandSelectable(hitC))
    {
        CloseTree(commands[hitC].id);
        return true;
    }
    int hit = HitTest(pt.y);
    if (hit >= 0 && IsSelectable(hit) && !items[hit].submenu)
    {
        CloseTree(items[hit].id);
        return true;
    }
    if (m_child && m_child->m_hwnd)
        return m_child->ActivateAt(ptScreen);
    return false;
}

void ModernMenu::TrackLeave()
{
    if (!m_hwnd) return;
    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hwnd, 0 };
    TrackMouseEvent(&tme);
}

LRESULT CALLBACK ModernMenu::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    ModernMenu* self = (ModernMenu*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    switch (uMsg)
    {
    case WM_LBUTTONDOWN:
        if (self) self->OnLButtonDown(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_LBUTTONUP:
        if (self) self->OnLButtonUp(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_MOUSEMOVE:
        if (self) self->OnMouseMove(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_MOUSELEAVE:
        if (self) self->OnMouseLeave();
        return 0;
    case WM_CAPTURECHANGED:
        // Захват увели снаружи — висящее нажатие отменяем.
        if (self)
        {
            self->m_pressed = -1;
            self->m_cmdPressed = -1;
            self->Render();
        }
        return 0;
    case WM_KEYDOWN:
        if (self) self->OnKey((int)wParam);
        return 0;
    case WM_TIMER:
        if (self && wParam == 1)
        {
            // Fade-in появления.
            self->m_animAlpha += 64;
            if (self->m_animAlpha >= 255)
            {
                self->m_animAlpha = 255;
                KillTimer(hWnd, 1);
            }
            if (self->m_hBitmap)
                DoUpdateLayeredMenu(hWnd, self->m_hBitmap,
                    self->m_menuW, self->m_menuH, (BYTE)self->m_animAlpha);
        }
        return 0;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK ModernMenu::MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && s_modalRoot)
    {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN)
        {
            MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
            if (!s_modalRoot->PointInTree(p->pt))
            {
                s_modalRoot->CloseTree(0);
                return 1; // съедаем клик, закрывший меню
            }
        }
    }
    return CallNextHookEx(s_mouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK ModernMenu::KbdProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && s_modalRoot && wParam == WM_KEYDOWN)
    {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        ModernMenu* deep = s_modalRoot->Deepest();
        switch (p->vkCode)
        {
        case VK_ESCAPE:
            s_modalRoot->CloseTree(0);
            return 1;
        case VK_DOWN:
            deep->MoveHover(1);
            return 1;
        case VK_UP:
            deep->MoveHover(-1);
            return 1;
        case VK_RIGHT:
            if (deep->m_hovered >= 0 && deep->m_hovered < (int)deep->items.size() &&
                deep->items[deep->m_hovered].submenu)
                deep->OpenChildFor(deep->m_hovered);
            return 1;
        case VK_LEFT:
            if (deep != s_modalRoot)
            {
                ModernMenu* parent = deep->m_parent;
                if (parent)
                {
                    parent->CloseChild();
                    parent->Render();
                }
            }
            else
                s_modalRoot->CloseTree(0);
            return 1;
        case VK_RETURN:
        case VK_SPACE:
            deep->ActivateHovered();
            return 1;
        }
    }
    return CallNextHookEx(s_kbdHook, nCode, wParam, lParam);
}

void ModernMenu::InstallHooks()
{
    if (!s_mouseHook)
        s_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, nullptr, 0);
    if (!s_kbdHook)
        s_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbdProc, nullptr, 0);
}

void ModernMenu::UninstallHooks()
{
    if (s_mouseHook) { UnhookWindowsHookEx(s_mouseHook); s_mouseHook = nullptr; }
    if (s_kbdHook) { UnhookWindowsHookEx(s_kbdHook); s_kbdHook = nullptr; }
}

int ModernMenu::Show(int x, int y)
{
    if (!m_renderer || (items.empty() && commands.empty())) return 0;
    m_dark = ReadDarkMode();
    m_parent = nullptr;
    m_child = nullptr;
    m_childFor = -1;
    m_result = 0;
    m_done = false;

    auto rcCmd = ToRenderItems(commands);
    auto ri = ToRenderItems(items);
    SIZE sz = m_renderer->MeasureMenu(rcCmd, ri);
    PositionIntoWorkArea(&x, &y, sz.cx, sz.cy);
    CreateAndShow(x, y, sz.cx, sz.cy);
    if (!m_hwnd) return 0;

    s_modalRoot = this;
    InstallHooks();
    MSG msg;
    while (!m_done && GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UninstallHooks();
    s_modalRoot = nullptr;
    // Сносим всё дерево окон разом (включая подменю).
    DestroyTreeWindows();
    // Дети могли остаться с висячим m_parent — чистим.
    m_parent = nullptr;
    m_child = nullptr;
    int r = m_result;
    m_result = 0;
    m_done = false;
    return r;
}
