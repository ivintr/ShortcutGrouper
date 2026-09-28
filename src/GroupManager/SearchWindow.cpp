#include "SearchWindow.h"
#include "WidgetManager.h"
#include "SettingsDialog.h"
#include "DesktopGrid.h"
#include "ModernMenu.h"
#include "Lang.h"
#include <shellapi.h>
#include <commctrl.h>
#include <algorithm>

#pragma comment(lib, "Shell32.lib")

static const wchar_t* SEARCH_CLASS = L"DesktopGroupSearch";
static const int Q_BASE_W = 460;
static const int Q_PAD = 12;
static const int Q_EDIT_H = 34;
static const int Q_ROW_H = 46;
static const int Q_MAX_ROWS = 8;
static const int Q_RADIUS = 12;

UINT SearchWindow::Dpi() const
{
    UINT dpi = 96;
    if (m_hwnd)
    {
        HMODULE hUser = GetModuleHandleW(L"user32.dll");
        if (hUser)
        {
            auto fn = (UINT(WINAPI*)(HWND))GetProcAddress(hUser, "GetDpiForWindow");
            if (fn) dpi = fn(m_hwnd);
        }
    }
    return dpi ? dpi : 96;
}

std::wstring SearchWindow::Lower(std::wstring s)
{
    for (auto& c : s) c = towlower(c);
    return s;
}

HICON SearchWindow::FetchIcon(const std::wstring& lnkPath)
{
    auto it = m_icons.find(lnkPath);
    if (it != m_icons.end()) return it->second;
    // Лимит кэша: без него карта растёт безгранично на переименованиях.
    if (m_icons.size() > 2048)
        ClearIcons();
    HICON hIcon = nullptr;
    if (!lnkPath.empty())
    {
        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(lnkPath.c_str(), 0, &sfi, sizeof(sfi),
                SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon)
            hIcon = sfi.hIcon;
    }
    m_icons[lnkPath] = hIcon; // и nullptr кэшируем, чтобы не дёргать shell
    return hIcon;
}

void SearchWindow::ClearIcons()
{
    for (auto& kv : m_icons)
        if (kv.second) DestroyIcon(kv.second);
    m_icons.clear();
}

void SearchWindow::Rebuild()
{
    m_all.clear();
    ClearIcons();
    if (!m_manager) return;
    for (const auto& g : m_manager->GetGroups())
    {
        for (const auto& s : g.shortcuts)
        {
            Result r;
            r.groupId = g.id;
            r.groupName = g.name;
            r.name = s.name.empty() ? s.lnkPath : s.name;
            r.lnkPath = s.lnkPath;
            r.targetPath = s.targetPath;
            m_all.push_back(std::move(r));
        }
    }
    DebugLogGrid("search", (int)m_all.size(), 0, 0, 0, 0, 0);    ApplyFilter();
}

void SearchWindow::ApplyFilter()
{
    m_hits.clear();
    std::wstring q = Lower(m_query);
    // Пустой запрос — показываем всё (как лаунчер).
    for (const auto& r : m_all)
    {
        Result h = r;
        if (q.empty())
        {
            h.rank = 1;
            m_hits.push_back(std::move(h));
            continue;
        }
        std::wstring nm = Lower(r.name);
        size_t p = nm.find(q);
        if (p == 0) h.rank = 0;
        else if (p != std::wstring::npos) h.rank = 1;
        else
        {
            std::wstring gr = Lower(r.groupName);
            if (gr.find(q) == std::wstring::npos) continue;
            h.rank = 2;
        }
        m_hits.push_back(std::move(h));
    }
    std::stable_sort(m_hits.begin(), m_hits.end(),
        [](const Result& a, const Result& b) { return a.rank < b.rank; });
    if ((int)m_hits.size() > 200) m_hits.resize(200);
    m_hovered = m_hits.empty() ? -1 : 0;
    m_scrollY = 0;
    Layout();
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SearchWindow::Layout()
{
    if (!m_hwnd) return;
    int rows = (int)m_hits.size();
    if (rows == 0) rows = 1; // место под «Ничего не найдено» целиком
    if (rows > Q_MAX_ROWS) rows = Q_MAX_ROWS;
    int listH = rows * Px(Q_ROW_H);
    int h = Px(Q_PAD) + Px(Q_EDIT_H) + Px(8) + listH + Px(Q_PAD);
    int w = Px(Q_BASE_W);
    DebugLogGrid("search-layout", (int)m_hits.size(), rows, w, h, 0, 0);

    RECT wr = {};
    GetWindowRect(m_hwnd, &wr);
    SetWindowPos(m_hwnd, nullptr, 0, 0, w, h,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
}

int SearchWindow::RowAt(int y) const
{
    int listTop = Px(Q_PAD) + Px(Q_EDIT_H) + Px(8);
    int rel = y - listTop + m_scrollY;
    if (rel < 0) return -1;
    int idx = rel / Px(Q_ROW_H);
    return (idx >= 0 && idx < (int)m_hits.size()) ? idx : -1;
}

void SearchWindow::SetHovered(int idx)
{
    if (idx == m_hovered) return;
    m_hovered = idx;
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SearchWindow::EnsureVisible(int idx)
{
    int rowH = Px(Q_ROW_H);
    int rows = (int)m_hits.size();
    if (rows > Q_MAX_ROWS) rows = Q_MAX_ROWS;
    int visH = rows * rowH;
    int top = idx * rowH;
    if (top < m_scrollY) m_scrollY = top;
    else if (top + rowH > m_scrollY + visH) m_scrollY = top + rowH - visH;
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SearchWindow::Launch(int idx)
{
    if (!m_manager || idx < 0 || idx >= (int)m_hits.size()) return;
    const Result hit = m_hits[idx]; // копия: группа ниже может измениться

    // Свежий индекс по (группа + путь): список мог устареть после Refresh.
    const GroupData* g = m_manager->FindGroup(hit.groupId);
    if (!g) { Rebuild(); return; }
    int fresh = -1;
    for (int i = 0; i < (int)g->shortcuts.size(); i++)
        if (g->shortcuts[i].lnkPath == hit.lnkPath) { fresh = i; break; }
    if (fresh < 0) { Rebuild(); return; }

    ShortcutInfo si = g->shortcuts[fresh];
    if (Settings::IsAutoPruneDead() && !WidgetManager::IsShortcutAlive(si))
    {
        std::wstring gid = hit.groupId;
        m_manager->RemoveDeadShortcut(gid, fresh);
        std::wstring disp = si.name.empty() ? si.lnkPath : si.name;
        WCHAR buf[512];
        swprintf_s(buf, Lang::Get(Str::W_DeadMsg), disp.c_str());
        MessageBoxW(m_hwnd, buf, Lang::Get(Str::W_DeadCaption),
            MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        Rebuild();
        return;
    }
    // Запускаем сам .lnk (аргументы/рабочая папка сохраняются), фолбэк — цель.
    std::wstring primary = si.lnkPath.empty() ? si.targetPath : si.lnkPath;
    std::wstring fallback = si.lnkPath.empty() ? L"" : si.targetPath;
    if (!primary.empty())
    {
        HINSTANCE rc = ShellExecuteW(m_hwnd, L"open", primary.c_str(),
            nullptr, nullptr, SW_SHOW);
        if ((INT_PTR)rc <= 32 && !fallback.empty() && fallback != primary)
            rc = ShellExecuteW(m_hwnd, L"open", fallback.c_str(),
                nullptr, nullptr, SW_SHOW);
        if ((INT_PTR)rc <= 32)
        {
            WCHAR buf[512];
            swprintf_s(buf, Lang::Get(Str::W_LaunchFailMsg), primary.c_str(), (int)(INT_PTR)rc);
            MessageBoxW(m_hwnd, buf, Lang::Get(Str::W_LaunchFailCaption),
                MB_OK | MB_ICONERROR | MB_TOPMOST);
            return;
        }
        m_manager->RecordShortcutLaunch(hit.groupId, fresh);
    }
    Hide();
}

void SearchWindow::Paint()
{
    if (!m_hwnd) return;
    RECT client = {};
    GetClientRect(m_hwnd, &client);
    int W = client.right - client.left;
    int H = client.bottom - client.top;
    if (W <= 0 || H <= 0) return;

    // Самолечение: если высота окна не соответствует списку (было в проде),
    // чиним геометрию и выходим — следующий Paint нарисует правильно.
    {
        int rows = (int)m_hits.size();
        if (rows > Q_MAX_ROWS) rows = Q_MAX_ROWS;
        int wantH = Px(Q_PAD) + Px(Q_EDIT_H) + Px(8) + rows * Px(Q_ROW_H) + Px(Q_PAD);
        if (abs(H - wantH) > 2)
        {
            DebugLogGrid("search-heal", H, wantH, rows, 0, 0, 0);
            SetWindowPos(m_hwnd, nullptr, 0, 0, W, wantH,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
    }
    {
        // Лог только при смене геометрии/состава (ховер красит постоянно).
        static int s_lw = 0, s_lh = 0, s_ln = -1;
        if (W != s_lw || H != s_lh || (int)m_hits.size() != s_ln)
        {
            s_lw = W; s_lh = H; s_ln = (int)m_hits.size();
            DebugLogGrid("search-paint", W, H, s_ln, 0, 0, 0);
        }
    }

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
    HGDIOBJ hOldBmp = nullptr;
    if (hBmp) hOldBmp = SelectObject(hdcMem, hBmp);
    if (bits) memset(bits, 0, (size_t)W * H * 4);

    // Фон: скруглённый прямоугольник + тонкая рамка.
    COLORREF bg = m_dark ? RGB(45, 45, 45) : RGB(248, 248, 248);
    COLORREF edge = m_dark ? RGB(80, 80, 80) : RGB(220, 220, 220);
    HBRUSH hBg = CreateSolidBrush(bg);
    HBRUSH hEdge = CreateSolidBrush(edge);
    HPEN hPen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ hOldBr = SelectObject(hdcMem, hEdge);
    HGDIOBJ hOldPen = SelectObject(hdcMem, hPen);
    int rad = Px(Q_RADIUS);
    RoundRect(hdcMem, 0, 0, W, H, rad, rad);
    SelectObject(hdcMem, hBg);
    SelectObject(hdcMem, GetStockObject(NULL_PEN));
    RoundRect(hdcMem, 2, 2, W - 2, H - 2, rad - 2 > 0 ? rad - 2 : 0, rad - 2 > 0 ? rad - 2 : 0);
    SelectObject(hdcMem, hOldBr);
    SelectObject(hdcMem, hOldPen);
    DeleteObject(hBg);
    DeleteObject(hEdge);
    DeleteObject(hPen);

    // Видимая граница поля ввода (иначе edit сливается с фоном).
    {
        HPEN hEPen = CreatePen(PS_SOLID, 1,
            m_dark ? RGB(110, 110, 110) : RGB(180, 180, 180));
        HGDIOBJ hEO = SelectObject(hdcMem, hEPen);
        HGDIOBJ hBO = SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
        int ex = Px(Q_PAD) - 2, ey = Px(Q_PAD) - 2;
        int ew = W - Px(Q_PAD) * 2 + 4, eh = Px(Q_EDIT_H) + 4;
        RoundRect(hdcMem, ex, ey, ex + ew, ey + eh, 8, 8);
        SelectObject(hdcMem, hEO);
        SelectObject(hdcMem, hBO);
        DeleteObject(hEPen);
    }

    // Текст ввода + хинт + каретка (свои, не нативный EDIT: тот глючит
    // с альфой на layered-окнах и не всегда отдаёт фокус из трея).
    {
        int tx = Px(Q_PAD) + Px(8);
        int ty = Px(Q_PAD);
        int tw = W - Px(Q_PAD) * 2 - Px(16);
        int th = Px(Q_EDIT_H);
        HGDIOBJ hOF = SelectObject(hdcMem, m_hFont ? m_hFont : GetStockObject(SYSTEM_FONT));
        SetBkMode(hdcMem, TRANSPARENT);
        if (m_query.empty())
        {
            SetTextColor(hdcMem, m_dark ? RGB(130, 130, 130) : RGB(150, 150, 150));
            RECT hr = { tx, ty, tx + tw, ty + th };
            DrawTextW(hdcMem, Lang::Get(Str::Q_Hint), -1, &hr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        else
        {
            // Горизонтальный скролл, чтобы хвост не уезжал из поля.
            RECT full = { 0, 0, 0, 0 };
            DrawTextW(hdcMem, m_query.c_str(), -1, &full,
                DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
            int textW = full.right - full.left;
            int offX = 0;
            if (textW > tw) offX = textW - tw;
            int sv = SaveDC(hdcMem);
            IntersectClipRect(hdcMem, tx, ty, tx + tw, ty + th);
            SetTextColor(hdcMem, m_dark ? RGB(255, 255, 255) : RGB(27, 27, 27));
            RECT tr = { tx - offX, ty, tx - offX + (textW > tw ? textW : tw), ty + th };
            DrawTextW(hdcMem, m_query.c_str(), -1, &tr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            // Каретка по префиксу до m_caretPos.
            if (GetFocus() == m_hwnd && m_caretOn)
            {
                std::wstring pre = m_query.substr(0, m_caretPos);
                RECT pr = { 0, 0, 0, 0 };
                DrawTextW(hdcMem, pre.c_str(), -1, &pr,
                    DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
                int cx = tx + (pr.right - pr.left) - offX;
                if (cx < tx) cx = tx;
                if (cx > tx + tw) cx = tx + tw;
                RECT cr = { cx, ty + Px(6), cx + Px(2), ty + th - Px(6) };
                HBRUSH hb = CreateSolidBrush(m_dark ? RGB(120, 180, 255) : RGB(0, 120, 212));
                FillRect(hdcMem, &cr, hb);
                DeleteObject(hb);
            }
            RestoreDC(hdcMem, sv);
        }
        SelectObject(hdcMem, hOF);
    }

    SetBkMode(hdcMem, TRANSPARENT);
    HGDIOBJ hOldFont = nullptr;
    int listTop = Px(Q_PAD) + Px(Q_EDIT_H) + Px(8);
    int rowH = Px(Q_ROW_H);

    if (m_hits.empty())
    {
        if (m_hFont == nullptr)
        {
            // Шрифты создаются в Show; страховка на случай гонки.
        }
        else
        {
            hOldFont = SelectObject(hdcMem, m_hSubFont ? m_hSubFont : m_hFont);
            SetTextColor(hdcMem, m_dark ? RGB(160, 160, 160) : RGB(120, 120, 120));
            RECT tr = { Px(Q_PAD), listTop, W - Px(Q_PAD), listTop + rowH };
            DrawTextW(hdcMem, Lang::Get(Str::Q_Empty), -1, &tr,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(hdcMem, hOldFont);
        }
    }
    else
    {
        int rows = (int)m_hits.size();
        if (rows > Q_MAX_ROWS) rows = Q_MAX_ROWS;
        // Отсечение списка, чтобы строки не лезли на скругление низа.
        HRGN hClip = CreateRectRgn(Px(6), listTop, W - Px(6), listTop + rows * rowH);
        SelectClipRgn(hdcMem, hClip);
        // Рисуем по абсолютным индексам с учётом скролла.
        int first = m_scrollY / rowH;
        int yOff = -(m_scrollY % rowH);
        HGDIOBJ hDefFont = SelectObject(hdcMem, m_hFont ? m_hFont : GetStockObject(SYSTEM_FONT));
        for (int r = 0; r <= rows && first + r < (int)m_hits.size(); r++)
        {
            int idx = first + r;
            int y = listTop + yOff + r * rowH;
            const Result& hit = m_hits[idx];
            if (idx == m_hovered)
            {
                COLORREF hov = m_dark ? RGB(62, 62, 62) : RGB(232, 240, 250);
                HBRUSH hb = CreateSolidBrush(hov);
                HGDIOBJ ho = SelectObject(hdcMem, hb);
                HPEN hp0 = CreatePen(PS_SOLID, 1, hov);
                HGDIOBJ hpo = SelectObject(hdcMem, hp0);
                RoundRect(hdcMem, Px(8), y + 2, W - Px(8), y + rowH - 2, 10, 10);
                SelectObject(hdcMem, ho);
                SelectObject(hdcMem, hpo);
                DeleteObject(hb);
                DeleteObject(hp0);
            }
            HICON hIcon = FetchIcon(hit.lnkPath);
            int iconSz = Px(32);
            int iconY = y + (rowH - iconSz) / 2;
            if (hIcon)
                DrawIconEx(hdcMem, Px(14), iconY, hIcon, iconSz, iconSz, 0, nullptr, DI_NORMAL);
            else
            {
                // Плейсхолдер: скруглённый квадрат.
                HBRUSH hb = CreateSolidBrush(m_dark ? RGB(70, 70, 70) : RGB(210, 210, 210));
                HGDIOBJ ho = SelectObject(hdcMem, hb);
                SelectObject(hdcMem, GetStockObject(NULL_PEN));
                RoundRect(hdcMem, Px(14), iconY, Px(14) + iconSz, iconY + iconSz, 8, 8);
                SelectObject(hdcMem, ho);
                DeleteObject(hb);
            }
            SelectObject(hdcMem, m_hFont ? m_hFont : GetStockObject(SYSTEM_FONT));
            SetTextColor(hdcMem, m_dark ? RGB(255, 255, 255) : RGB(27, 27, 27));
            RECT tr = { Px(14) + iconSz + Px(10), y, W - Px(12), y + rowH - Px(16) };
            DrawTextW(hdcMem, hit.name.c_str(), -1, &tr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(hdcMem, m_hSubFont ? m_hSubFont : (m_hFont ? m_hFont : GetStockObject(SYSTEM_FONT)));
            SetTextColor(hdcMem, m_dark ? RGB(160, 160, 160) : RGB(120, 120, 120));
            RECT gr = { Px(14) + iconSz + Px(10), y + rowH - Px(20), W - Px(12), y + rowH - Px(2) };
            DrawTextW(hdcMem, hit.groupName.c_str(), -1, &gr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        SelectObject(hdcMem, hDefFont);
        SelectClipRgn(hdcMem, nullptr);
        DeleteObject(hClip);
    }

    POINT ptDst = {};
    RECT wndRc = {};
    GetWindowRect(m_hwnd, &wndRc);
    ptDst.x = wndRc.left;
    ptDst.y = wndRc.top;
    SIZE sizeWnd = { W, H };
    POINT ptSrc = { 0, 0 };
    // GDI не пишет альфу (остаётся 0 = прозрачно): все нарисованные пиксели
    // делаем непрозрачными. У иконок своя альфа — их не трогаем.
    if (bits)
    {
        DWORD* px = (DWORD*)bits;
        int n = W * H;
        for (int i = 0; i < n; i++)
        {
            DWORD v = px[i];
            if ((v & 0xFF000000) == 0 && (v & 0x00FFFFFF) != 0)
                px[i] = v | 0xFF000000;
        }
    }
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    if (hBmp)
        UpdateLayeredWindow(m_hwnd, hdcScreen, &ptDst, &sizeWnd,
            hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    if (hBmp) SelectObject(hdcMem, hOldBmp);
    if (hBmp) DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

void SearchWindow::OnChar(wchar_t ch)
{
    if (ch < 32 || ch == 127) return;
    if (GetKeyState(VK_CONTROL) & 0x8000) return; // хоткеи не печатаем
    if (m_query.size() >= 64) return;
    if (m_caretPos < 0) m_caretPos = 0;
    if (m_caretPos > (int)m_query.size()) m_caretPos = (int)m_query.size();
    m_query.insert(m_caretPos, 1, ch);
    m_caretPos++;
    ApplyFilter();
}

void SearchWindow::OnKeyDown(WPARAM vk)
{
    switch (vk)
    {
    case VK_ESCAPE:
        Hide();
        break;
    case VK_RETURN:
        Launch(m_hovered >= 0 ? m_hovered : 0);
        break;
    case VK_UP:
    case VK_DOWN:
        if (!m_hits.empty())
        {
            int n = (int)m_hits.size();
            int idx = m_hovered < 0 ? 0 : m_hovered;
            idx = (vk == VK_DOWN) ? (idx + 1) % n : (idx - 1 + n) % n;
            SetHovered(idx);
            EnsureVisible(idx);
        }
        break;
    case VK_BACK:
        if (m_caretPos > 0 && !m_query.empty())
        {
            if (m_caretPos > (int)m_query.size()) m_caretPos = (int)m_query.size();
            m_query.erase(m_caretPos - 1, 1);
            m_caretPos--;
            ApplyFilter();
        }
        break;
    case VK_DELETE:
        if (m_caretPos >= 0 && m_caretPos < (int)m_query.size())
        {
            m_query.erase(m_caretPos, 1);
            ApplyFilter();
        }
        break;
    case VK_LEFT:
        if (m_caretPos > 0) { m_caretPos--; InvalidateRect(m_hwnd, nullptr, FALSE); }
        break;
    case VK_RIGHT:
        if (m_caretPos < (int)m_query.size()) { m_caretPos++; InvalidateRect(m_hwnd, nullptr, FALSE); }
        break;
    case VK_HOME:
        m_caretPos = 0;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        break;
    case VK_END:
        m_caretPos = (int)m_query.size();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        break;
    }
}

LRESULT CALLBACK SearchWindow::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    SearchWindow* self = (SearchWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

    switch (uMsg)
    {
    case WM_CREATE:
    {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        self = (SearchWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)self);
        return 0;
    }
    case WM_COMMAND:
        return 0;
    case WM_CHAR:
        if (self) self->OnChar((wchar_t)wParam);
        return 0;
    case WM_LBUTTONDOWN:
        if (self)
        {
            SetFocus(hWnd);
            // Клик по полю ввода — каретка в конец; по списку — просто фокус.
            if (HIWORD(lParam) < self->Px(Q_PAD) + self->Px(Q_EDIT_H) + self->Px(8))
                self->m_caretPos = (int)self->m_query.size();
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (self) self->SetHovered(self->RowAt(HIWORD(lParam)));
        return 0;
    case WM_LBUTTONUP:
        if (self)
        {
            int idx = self->RowAt(HIWORD(lParam));
            if (idx >= 0) self->Launch(idx);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (self && !self->m_hits.empty())
        {
            int rowH = self->Px(Q_ROW_H);
            int rows = (int)self->m_hits.size();
            if (rows > Q_MAX_ROWS) rows = Q_MAX_ROWS;
            int visH = rows * rowH;
            int fullH = (int)self->m_hits.size() * rowH;
            int maxS = fullH - visH;
            if (maxS < 0) maxS = 0;
            self->m_scrollY -= (int)((long long)GET_WHEEL_DELTA_WPARAM(wParam) * rowH * 2 / WHEEL_DELTA);
            if (self->m_scrollY < 0) self->m_scrollY = 0;
            if (self->m_scrollY > maxS) self->m_scrollY = maxS;
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hWnd, &pt);
            self->SetHovered(self->RowAt(pt.y));
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    case WM_ACTIVATE:
        if (self && LOWORD(wParam) == WA_INACTIVE)
            self->Hide();
        return 0;
    case WM_KEYDOWN:
        if (self) self->OnKeyDown(wParam);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (self)
        {
            self->m_caretOn = true;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (self && wParam == 27)
        {
            self->m_caretOn = !self->m_caretOn;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        // Без этого все InvalidateRect (фильтр, ховер, скролл, каретка)
        // уходят в DefWindowProc и ничего не перерисовывают.
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        if (self) self->Paint();
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (self)
        {
            self->ClearIcons();
            if (self->m_hFont) DeleteObject(self->m_hFont);
            if (self->m_hSubFont) DeleteObject(self->m_hSubFont);
            self->m_hFont = self->m_hSubFont = nullptr;
            self->m_hwnd = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void SearchWindow::Show(WidgetManager* manager)
{
    m_manager = manager;
    m_dark = ModernMenuIsDarkMode();

    if (!m_hwnd)
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.lpfnWndProc = SearchWindow::WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = SEARCH_CLASS;
            wc.hbrBackground = nullptr;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassExW(&wc);
            registered = true;
        }
        // Позиция: сверху по центру рабочей области (как лаунчеры).
        RECT work = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int w = Px(Q_BASE_W);
        int x = work.left + ((work.right - work.left) - w) / 2;
        int y = work.top + (work.bottom - work.top) / 6;
        m_hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            SEARCH_CLASS, L"", WS_POPUP,
            x, y, w, 100, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!m_hwnd) return;
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);

        m_hFont = CreateFontW(-Px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        m_hSubFont = CreateFontW(-Px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }

    m_caretPos = (int)m_query.size();
    m_caretOn = true;
    Rebuild();
    Layout();
    Paint();
    ShowWindow(m_hwnd, SW_SHOW);
    SetTimer(m_hwnd, 27, 530, nullptr); // мигание каретки
    // Фокус ввода: из трея foreground могут не отдать, поэтому временно
    // связываем ввод с потоком переднего окна (приём из настроек).
    {
        HWND hFore = GetForegroundWindow();
        DWORD foreTid = hFore ? GetWindowThreadProcessId(hFore, nullptr) : 0;
        DWORD thisTid = GetCurrentThreadId();
        BOOL attached = FALSE;
        if (foreTid && foreTid != thisTid)
            attached = AttachThreadInput(thisTid, foreTid, TRUE);
        for (int i = 0; i < 3 && GetFocus() != m_hwnd; i++)
        {
            SetForegroundWindow(m_hwnd);
            SetFocus(m_hwnd);
            if (GetFocus() != m_hwnd) Sleep(30);
        }
        if (attached)
            AttachThreadInput(thisTid, foreTid, FALSE);
        RECT er = {};
        GetWindowRect(m_hwnd, &er);
        DebugLogGrid("search-show", er.right - er.left, er.bottom - er.top,
            GetFocus() == m_hwnd ? 1 : 0, (int)m_query.size(), 0, 0);
    }
}

void SearchWindow::Hide()
{
    if (m_hwnd)
        KillTimer(m_hwnd, 27);
    if (m_hwnd && IsWindowVisible(m_hwnd))
        ShowWindow(m_hwnd, SW_HIDE);
}

void SearchWindow::Destroy()
{
    ClearIcons();
    if (m_hwnd && IsWindow(m_hwnd))
    {
        KillTimer(m_hwnd, 27);
        DestroyWindow(m_hwnd); // остальное чистится в WM_DESTROY
    }
    m_manager = nullptr;
}
