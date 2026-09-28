#include "DesktopWidget.h"
#include "Logger.h"
#include "PopupWindow.h"
#include "BlurHelper.h"
#include "WidgetManager.h"
#include "SettingsDialog.h"
#include "DesktopGrid.h"
#include "ModernMenu.h"
#include "ColorDialog.h"
#include "Lang.h"
#include <vector>
#include <algorithm>
#include <map>
#include <cstdio>
#include <climits>
#include <memory>
#include <new>
#include <shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <shlwapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <oleidl.h>
#include <dwmapi.h>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Ole32.lib")

void DoUpdateLayered(HWND hwnd, HBITMAP hBitmap, int w, int h, BYTE alpha){
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HGDIOBJ hOld = SelectObject(hdcMem, hBitmap);

    POINT ptSrc = { 0, 0 };
    SIZE sizeWnd = { w, h };

    RECT rcWnd;
    GetWindowRect(hwnd, &rcWnd);
    POINT ptDst = { rcWnd.left, rcWnd.top };

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = alpha;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

const wchar_t* const WIDGET_CLASS  = L"DesktopGroupWidget";
const wchar_t* const POPUP_CLASS   = L"DesktopGroupPopup";

// Временный замер производительности открытия попапа (диагностика лага).
void LogTiming(const wchar_t* fmt, ULONGLONG v)
{
    AppLog(L"TIME", L"%s: %llu ms", fmt, v);
}

// Определены ниже, нужны раньше для снапа виджетов
static POINT SnapPointToGrid(POINT pt, const GroupData& group, HWND hSelf);
static POINT NudgeToFreeSpot(POINT pt, const GroupData& group, HWND hSelf);

// ---------------------------------------------------------------------------
// OLE DropTarget for WS_POPUP windows (DragAcceptFiles doesn't work)
// ---------------------------------------------------------------------------

class WidgetDropTarget : public IDropTarget {
public:
    WidgetDropTarget(DesktopWidget* widget) : m_ref(1), m_widget(widget) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        OutputDebugStringW(L"WidgetDropTarget DragEnter\n");
        if (!pdwEffect) return E_POINTER;
        *pdwEffect = DROPEFFECT_COPY;
        // Подсветка цели: виджет вспыхивает, пока над ним тащат файлы.
        if (m_widget) m_widget->RefreshGlow();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        if (!pdwEffect) return E_POINTER;
        *pdwEffect = DROPEFFECT_COPY;
        if (m_widget) m_widget->RefreshGlow();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (m_widget) m_widget->RefreshGlow();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        OutputDebugStringW(L"WidgetDropTarget Drop\n");
        if (!pdwEffect) return E_POINTER;
        *pdwEffect = DROPEFFECT_COPY;
        if (!m_widget) return E_FAIL;

        // Захватываем заранее: AddShortcutToGroup может пересоздать виджеты,
        // после него m_widget трогать нельзя без перепроверки.
        WidgetManager* mgr = m_widget->m_manager;
        std::wstring gid = m_widget->m_groupId;
        if (!mgr) return E_FAIL;

        FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg = {};
        if (FAILED(pDataObj->GetData(&fmt, &stg))) return E_FAIL;

        // hGlobal УЖЕ HDROP; GlobalLock здесь неверен.
        HDROP hDrop = (HDROP)stg.hGlobal;
        if (!hDrop) { ReleaseStgMedium(&stg); return E_FAIL; }

        int count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        bool added = false;
        for (int i = 0; i < count; i++) {
            UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
            if (len == 0) continue;
            std::wstring path(len + 1, L'\0');
            if (!DragQueryFileW(hDrop, i, &path[0], len + 1)) continue;
            path.resize(len);
            OutputDebugStringW(L"OLE Drop: ");
            OutputDebugStringW(path.c_str());
            OutputDebugStringW(L"\n");
            if (IsGroupableShortcut(path)) {
                if (mgr->AddShortcutToGroup(gid, path))
                {
                    added = true;
                    OutputDebugStringW(L"OLE Drop: added OK\n");
                }
                else
                {
                    OutputDebugStringW(L"OLE Drop: AddShortcutToGroup FAILED\n");
                }
            }
        }

        ReleaseStgMedium(&stg);

        if (added) {
            // Виджет мог быть пересоздан — ищем заново, а не по висячему this.
            if (DesktopWidget* w = mgr->FindWidget(gid))
            {
                const GroupData* g = mgr->FindGroup(gid);
                if (g) w->Update(*g);
            }
        }
        return S_OK;
    }

private:
    ULONG m_ref;
    DesktopWidget* m_widget;
};


static HWND s_hDesktopParent = nullptr;
static WidgetManager* s_manager = nullptr;
// Счётчик OleInitialize для виджетов (парный OleUninitialize).
static int s_oleRefs = 0;

void SetDesktopParent(HWND h) { s_hDesktopParent = h; }
void SetGlobalManager(WidgetManager* m) { s_manager = m; }
WidgetManager* GetGlobalManager() { return s_manager; }

// Прячем все наши окна (виджеты + попап) на время захвата экрана для блюра,
// иначе они попадают в собственный фон: двоение текста/иконок, ореолы.
// DwmFlush ждёт перекомпозицию, чтобы скрытие успело сработать до BitBlt.
static std::vector<HWND> s_hiddenForCapture;

static void HideOurWindows(bool hide, int cx, int cy, int cw, int ch)
{
    if (hide)
    {
        // Прячем только окна, пересекающиеся с кадром: иначе ховер по одному
        // виджету гасит все (мигание)
        RECT cap = { cx - 2, cy - 2, cx + cw + 2, cy + ch + 2 };
        s_hiddenForCapture.clear();
        HWND h = nullptr;
        while ((h = FindWindowExW(nullptr, h, WIDGET_CLASS, nullptr)) != nullptr)
        {
            if (!IsWindowVisible(h)) continue;
            RECT wr = {};
            GetWindowRect(h, &wr);
            RECT inter = {};
            if (!IntersectRect(&inter, &cap, &wr)) continue;
            s_hiddenForCapture.push_back(h);
            ShowWindow(h, SW_HIDE);
        }
        h = nullptr;
        while ((h = FindWindowExW(nullptr, h, POPUP_CLASS, nullptr)) != nullptr)
        {
            if (!IsWindowVisible(h)) continue;
            RECT wr = {};
            GetWindowRect(h, &wr);
            RECT inter = {};
            if (!IntersectRect(&inter, &cap, &wr)) continue;
            s_hiddenForCapture.push_back(h);
            ShowWindow(h, SW_HIDE);
        }
        if (!s_hiddenForCapture.empty())
            DwmFlush();
    }
    else
    {
        for (HWND x : s_hiddenForCapture)
        {
            if (IsWindow(x))
                ShowWindow(x, SW_SHOWNA);
        }
        s_hiddenForCapture.clear();
    }
}

void SetGlobalRenderer(WidgetRenderer* r)
{
    if (r) r->SetCaptureGuard(HideOurWindows);
}

static void RegisterWidgetClass(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = DesktopWidget::WndProc;
    wc.hInstance      = hInst;
    wc.lpszClassName  = WIDGET_CLASS;
    wc.hbrBackground  = nullptr;
    RegisterClassExW(&wc);

    WNDCLASSEXW wc2 = { sizeof(wc2) };
    wc2.lpfnWndProc   = PopupWindow::WndProc;
    wc2.hInstance      = hInst;
    wc2.lpszClassName  = POPUP_CLASS;
    wc2.hbrBackground  = nullptr;
    RegisterClassExW(&wc2);
}

// ---------------------------------------------------------------------------
// DesktopWidget
// ---------------------------------------------------------------------------

bool DesktopWidget::Create(HWND hParent, const GroupData& group,
    WidgetRenderer* renderer, WidgetManager* manager)
{
    static bool registered = false;
    if (!registered)
    {
        RegisterWidgetClass(GetModuleHandleW(nullptr));
        registered = true;
    }

    m_group = group;
    m_groupId = group.id;
    m_renderer = renderer;
    m_manager = manager;

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    // Без WS_VISIBLE: первый рендер идёт пока окно скрыто (чистый захват
    // без самого себя), показ — в ShowAt после UpdateBitmap
    m_hwnd = CreateWindowExW(exStyle, WIDGET_CLASS, L"",
        WS_POPUP | WS_CLIPCHILDREN, 0, 0, WidgetWidth(m_group), WidgetHeight(m_group),
        nullptr, nullptr, GetModuleHandleW(nullptr), this);

    if (!m_hwnd) {
        OutputDebugStringW(L"DesktopWidget::CreateWindowExW FAILED\n");
        return false;
    }

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);

    // OleInitialize считаем попарно: последний разрушенный виджет
    // вызывает OleUninitialize. RegisterDragDrop без проверки hr тёк
    // m_dropTarget при ошибке — проверяем и откатываем.
    // Возврат OleInitialize проверяем: при FAILED (напр. RPC_E_CHANGED_MODE)
    // безусловный OleUninitialize был бы разбалансирован.
    bool oleCounted = false;
    if (s_oleRefs == 0)
    {
        if (SUCCEEDED(OleInitialize(nullptr)))
        {
            s_oleRefs++;
            oleCounted = true;
        }
    }
    else
    {
        s_oleRefs++;
        oleCounted = true;
    }
    m_oleCounted = oleCounted;
    m_dropTarget = new (std::nothrow) WidgetDropTarget(this);
    HRESULT hr = m_dropTarget
        ? RegisterDragDrop(m_hwnd, m_dropTarget)
        : E_OUTOFMEMORY;
    if (FAILED(hr))
    {
        if (m_dropTarget) { m_dropTarget->Release(); m_dropTarget = nullptr; }
        // Счётчик уже откачен здесь — флаг сбрасываем, чтобы Destroy
        // не вычел второй раз.
        if (oleCounted && --s_oleRefs == 0) OleUninitialize();
        m_oleCounted = false;
    }

    UpdateBitmap();
    return true;
}

void DesktopWidget::Destroy()
{
    if (m_hwnd)
    {
        if (m_dropTarget) {
            RevokeDragDrop(m_hwnd);
            m_dropTarget->Release();
            m_dropTarget = nullptr;
            if (m_oleCounted)
            {
                m_oleCounted = false;
                if (--s_oleRefs == 0) OleUninitialize();
            }
        }
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    else if (m_dropTarget)
    {
        // Create упал после создания dropTarget: откатываем счётчик.
        m_dropTarget->Release();
        m_dropTarget = nullptr;
        if (m_oleCounted)
        {
            m_oleCounted = false;
            if (--s_oleRefs == 0) OleUninitialize();
        }
    }
    if (m_hBitmap)
    {
        DeleteObject(m_hBitmap);
        m_hBitmap = nullptr;
    }
}

void DesktopWidget::Update(const GroupData& group)
{
    m_group = group;
    // Размер зависит от сетки и подписи — приводим окно к актуальному
    if (m_hwnd)
    {
        RECT rc;
        GetWindowRect(m_hwnd, &rc);
        int wantW = WidgetWidth(m_group);
        int wantH = WidgetHeight(m_group);
        if (rc.right - rc.left != wantW || rc.bottom - rc.top != wantH)
            SetWindowPos(m_hwnd, nullptr, 0, 0, wantW, wantH,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    }
    UpdateBitmap();
}

void DesktopWidget::ShowAt(int x, int y)
{
    if (m_hwnd)
    {
        int wantW = WidgetWidth(m_group);
        int wantH = WidgetHeight(m_group);
        if (Settings::IsSnapToGrid())
        {
            POINT p = SnapPointToGrid({ x, y }, m_group, m_hwnd);
            x = p.x;
            y = p.y;
        }
        else
        {
            // Даже без сетки не наезжаем на файлы: лёгкий сдвиг до свободы.
            POINT p = NudgeToFreeSpot({ x, y }, m_group, m_hwnd);
            x = p.x;
            y = p.y;
        }
        // Виджет — слой рабочего стола: всегда под обычными окнами.
        // HWND_BOTTOM здесь и страховка в WM_WINDOWPOSCHANGING ниже.
        SetWindowPos(m_hwnd, HWND_BOTTOM, x, y, wantW, wantH,
            SWP_NOACTIVATE);
        // Фиксируем скорректированную позицию, чтобы следующий запуск
        // не пытался снова встать на занятую иконками ячейку.
        m_group.x = x;
        m_group.y = y;
        if (m_manager) m_manager->SaveGroupPosition(m_groupId, x, y);
        UpdateBitmap();
        ShowWindow(m_hwnd, SW_SHOWNA);
    }
}

void DesktopWidget::SnapToGrid()
{
    if (!m_hwnd) return;
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    // Размер тоже приводим к актуальному (мог поменяться флаг подписи)
    SetWindowPos(m_hwnd, nullptr, 0, 0, WidgetWidth(m_group), WidgetHeight(m_group),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    GetWindowRect(m_hwnd, &rc);
    POINT p = SnapPointToGrid({ rc.left, rc.top }, m_group, m_hwnd);
    if (p.x == rc.left && p.y == rc.top) { UpdateBitmap(); return; }
    SetWindowPos(m_hwnd, nullptr, p.x, p.y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    m_group.x = p.x;
    m_group.y = p.y;
    if (m_manager) m_manager->SaveGroupPosition(m_groupId, p.x, p.y);
    UpdateBitmap(); // фон перезахватывается на новом месте
}

void DesktopWidget::FinishDropPosition()
{
    if (!m_hwnd) return;
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    int nx = rc.left;
    int ny = rc.top;
    if (Settings::IsSnapToGrid())
    {
        POINT p = SnapPointToGrid({ nx, ny }, m_group, m_hwnd);
        nx = p.x;
        ny = p.y;
    }
    else
    {
        // Без сетки: не даём бросить виджет поверх иконки/виджета.
        POINT p = NudgeToFreeSpot({ nx, ny }, m_group, m_hwnd);
        nx = p.x;
        ny = p.y;
    }
    SetWindowPos(m_hwnd, nullptr, nx, ny, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    m_group.x = nx;
    m_group.y = ny;
    if (m_manager) m_manager->SaveGroupPosition(m_groupId, nx, ny);
    // После перетаскивания — обратно на слой рабочего стола,
    // иначе виджет навсегда останется поверх окон.
    SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    UpdateBitmap();
}

void DesktopWidget::UpdateBitmap()
{
    if (!m_renderer || !m_hwnd) return;
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    WidgetRenderContext ctx;
    ctx.widgetScreenX = rc.left;
    ctx.widgetScreenY = rc.top;
    auto result = m_renderer->RenderWidget(m_group, ctx, m_glow,
        m_manager && m_manager->IsSelected(m_groupId));
    // Не затираем старый битмап, если рендер не отдал новый.
    if (!result.hBitmap) return;
    if (m_hBitmap) DeleteObject(m_hBitmap);
    m_hBitmap = result.hBitmap;
    if (m_hwnd && m_hBitmap)
        DoUpdateLayered(m_hwnd, m_hBitmap, WidgetWidth(m_group), WidgetHeight(m_group));
}

LRESULT CALLBACK DesktopWidget::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    DesktopWidget* self = (DesktopWidget*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

    switch (uMsg)
    {
    case WM_CREATE:
        return 0;

    case WM_PAINT:
        if (self) self->OnPaint();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN:
        if (self) self->OnLButtonDown(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_LBUTTONUP:
        if (self) self->OnLButtonUp(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_RBUTTONDOWN:
        // Cancel any left-button drag so the menu opens cleanly
        if (self && self->m_dragging)
        {
            self->m_dragging = false;
            ReleaseCapture();
        }
        return 0;

    case WM_RBUTTONUP:
        if (self) self->OnRButtonUp(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_CONTEXTMENU:
        if (self) self->ShowContextMenu();
        return 0;

    case WM_MOUSEMOVE:
        if (self) self->OnMouseMove(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_MOUSELEAVE:
        if (self)
        {
            if (self->m_dragging)
            {
                self->m_dragging = false;
                ReleaseCapture();
            }
            self->OnMouseLeave();
        }
        return 0;

    case WM_TIMER:
        if (self) self->OnTimer();
        return 0;

    case WM_CAPTURECHANGED:
        if (self && self->m_dragging)
        {
            self->m_dragging = false;
        }
        return 0;

    case WM_NCHITTEST:
        return HTCLIENT;

    case WM_WINDOWPOSCHANGING:
        // Виджет живёт на слое рабочего стола: любая попытка поднять его
        // поверх окон (фокус, ShowWindow, соседние ресайзы) сбрасывается вниз.
        // Исключение — активное перетаскивание самим пользователем.
        if (self && !self->m_dragging)
        {
            WINDOWPOS* wp = (WINDOWPOS*)lParam;
            if (!(wp->flags & SWP_NOZORDER))
                wp->hwndInsertAfter = HWND_BOTTOM;
        }
        return 0;

    case WM_SETCURSOR:
        // Над виджетом всегда обычная стрелка: сбрасываем курсор,
        // который мог остаться от ресайза/перетаскивания попапа
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void DesktopWidget::OnPaint()
{
}

void DesktopWidget::OnLButtonDown(int x, int y)
{
    m_pressX = x;
    m_pressY = y;
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    m_pressAbsX = rc.left + x;
    m_pressAbsY = rc.top + y;
    m_moved = false;
    m_ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    m_pressedOnSelected = false;
    m_dragPeers.clear();
    if (m_manager)
    {
        if (m_ctrlDown)
        {
            // Тоггл — при отпускании без движения (как в проводнике).
        }
        else if (!m_manager->IsSelected(m_groupId))
        {
            m_manager->SelectSingleGroup(m_groupId);
        }
        else
        {
            // Нажали на уже выделенный: состав держим для возможного
            // группового перетаскивания, одиночный выбор — при отпускании.
            m_pressedOnSelected = true;
        }
        // Пиры группового драга: все выделенные включая себя.
        if (m_manager->SelectedCount() > 1 && m_manager->IsSelected(m_groupId))
        {
            for (auto* w : m_manager->SelectedWidgets())
            {
                if (!w || !w->GetHwnd()) continue;
                RECT r = {};
                GetWindowRect(w->GetHwnd(), &r);
                DesktopWidget::DragPeer peer;
                peer.w = w;
                peer.startX = r.left;
                peer.startY = r.top;
                m_dragPeers.push_back(peer);
            }
        }
    }
    m_dragging = true;
    SetCapture(m_hwnd);
    RefreshGlow();
}

void DesktopWidget::TogglePopup()
{
    WidgetManager* mgr = m_manager;
    std::wstring gid = m_groupId;
    if (mgr && mgr->PruneGroup(gid))
    {
        return; // группа состояла из битых ярлыков и удалена вместе с виджетом
    }
    PopupWindow* popup = FindPopupByGroupId(m_groupId);
    if (popup && popup->IsVisible())
    {
        if (popup->IsClosing())
            popup->CancelClose();
        else
            popup->CloseAnimated();
    }
    else
    {
        // Свежие данные из менеджера: там запомненный размер попапа
        GroupData g = m_group;
        if (m_manager)
        {
            if (const GroupData* fresh = m_manager->FindGroup(m_groupId))
                g = *fresh;
        }
        RECT rc;
        GetWindowRect(m_hwnd, &rc);
        PopupWindow* pw = new PopupWindow();
        pw->Create(s_hDesktopParent, g, m_renderer);
        pw->ShowNear(rc.left + WidgetWidth(m_group) / 2, rc.top);
    }
}

void DesktopWidget::OnLButtonUp(int x, int y)
{
    if (m_dragging)
    {
        ReleaseCapture();
        m_dragging = false;

        if (!m_moved)
        {
            if (m_ctrlDown)
            {
                // Ctrl+клик без движения: тоггл выделения, попап не открываем.
                if (m_manager) m_manager->ToggleSelectGroup(m_groupId);
            }
            else
            {
                // Клик по выделенному в пачке без движения: схлопнуть выбор.
                if (m_pressedOnSelected && m_manager &&
                    m_manager->SelectedCount() > 1)
                    m_manager->SelectSingleGroup(m_groupId);
                TogglePopup();
            }
        }
        else if (!m_dragPeers.empty())
        {
            // Групповой дроп: каждый пир финализируется отдельно
            // (видят свежие позиции друг друга через живые окна).
            for (const auto& peer : m_dragPeers)
            {
                if (!peer.w || !peer.w->GetHwnd() || !IsWindow(peer.w->GetHwnd()))
                    continue;
                peer.w->FinishDropPosition();
            }
            m_dragPeers.clear();
            // Пачка встала — иконки из-под неё вниз по сетке.
            PushDesktopIconsOutOfWidgets(CollectAllWidgetRects());
        }
        else
        {
            FinishDropPosition();
            // Виджет встал — иконки из-под него вниз по сетке.
            PushDesktopIconsOutOfWidgets(CollectAllWidgetRects());
        }
        RefreshGlow();
    }
    (void)x; (void)y;
}

enum GroupMenuCmd {
    GMC_OPEN = 1,
    GMC_RENAME = 2,
    GMC_UNGROUP = 3,
    GMC_DELETE = 4,
    GMC_HIDE_NAME = 5,
    GMC_SHOW_OVERFLOW = 10,
    GMC_BULK_UNGROUP = 6,
    GMC_BULK_DELETE = 7,
    GMC_BULK_CLEAR = 8,
    GMC_BULK_UNINSTALL_APPS = 9,
    GMC_SORT_OFF = 11,
    GMC_SORT_NAME_ASC = 12,
    GMC_SORT_NAME_DESC = 13,
    GMC_SORT_TYPE = 14,
    GMC_SORT_RECENT = 15,
    GMC_COLOR_BASE = 21,
    GMC_GRID_2X2 = 31,
    GMC_GRID_3X3 = 32,
    GMC_COLOR_CUSTOM = 33
};

// UninstallInfo и хелперы — в PopupWindow.h (общие для bulk- и shell-меню).

struct GroupColorOption {
    Str name;
    int rgb;
};

static const GroupColorOption GROUP_COLORS[] = {
    { Str::C_Default, 0xFFFFFF },
    { Str::C_Blue,    0x3B82F6 },
    { Str::C_Teal,    0x14B8A6 },
    { Str::C_Green,   0x22C55E },
    { Str::C_Yellow,  0xEAB308 },
    { Str::C_Orange,  0xF97316 },
    { Str::C_Red,     0xEF4444 },
    { Str::C_Pink,    0xEC4899 },
    { Str::C_Purple,  0x8B5CF6 },
    { Str::C_Dark,    0x2B2B30 },
};
static const int GROUP_COLOR_COUNT =
    (int)(sizeof(GROUP_COLORS) / sizeof(GROUP_COLORS[0]));

void DesktopWidget::OnRButtonUp(int x, int y)
{
    (void)x; (void)y;
    if (m_dragging)
    {
        ReleaseCapture();
        bool wasMoved = m_moved;
        m_dragging = false;
        m_moved = false;
        if (wasMoved)
        {
            if (!m_dragPeers.empty())
            {
                for (const auto& peer : m_dragPeers)
                {
                    if (!peer.w || !peer.w->GetHwnd() || !IsWindow(peer.w->GetHwnd()))
                        continue;
                    peer.w->FinishDropPosition();
                }
                m_dragPeers.clear();
            }
            else
            {
                FinishDropPosition();
            }
            // Виджет(ы) встал(и) — иконки из-под него вниз по сетке.
            PushDesktopIconsOutOfWidgets(CollectAllWidgetRects());
            RefreshGlow();
            return;
        }
    }
    ShowContextMenu();
    RefreshGlow();
}

void DesktopWidget::ShowContextMenu()
{
    if (!m_manager || !m_renderer) return;

    // Пачка выделена и клик по своей — общее меню вместо одиночного.
    if (m_manager->SelectedCount() > 1 && m_manager->IsSelected(m_groupId))
    {
        std::vector<std::wstring> ids;
        for (auto* w : m_manager->SelectedWidgets())
            ids.push_back(w->GetGroupId());
        if (ids.size() > 1)
        {
            ShowBulkMenu(ids);
            return;
        }
    }

    const GroupData* fresh = m_manager->FindGroup(m_groupId);
    std::wstring name = fresh ? fresh->name : m_group.name;
    std::wstring title = name.empty() ? Lang::Get(Str::W_DefaultGroupName) : name;
    int curSort = fresh ? fresh->sortMode : 0;
    if (curSort < 0 || curSort > 4) curSort = 0;
    int curColor = fresh ? fresh->glassColor : 0xFFFFFF;
    bool hideName = fresh && fresh->hideName;
    bool showOv = fresh ? fresh->showOverflow : m_group.showOverflow;
    int curGrid = fresh ? fresh->gridSize : 2;
    if (curGrid < 2 || curGrid > 3) curGrid = 2;

    // Подменю должны жить всё время модального Show() — держим на стеке.
    ModernMenu sortMenu(m_renderer);
    sortMenu.items = {
        { Lang::Get(Str::W_SortOff),  GMC_SORT_OFF,      curSort == 0 },
        { L"", 0, false, false, true },
        { Lang::Get(Str::W_SortAsc),  GMC_SORT_NAME_ASC, curSort == 1 },
        { Lang::Get(Str::W_SortDesc), GMC_SORT_NAME_DESC, curSort == 2 },
        { Lang::Get(Str::W_SortType), GMC_SORT_TYPE,     curSort == 3 },
        { Lang::Get(Str::W_SortRecent), GMC_SORT_RECENT, curSort == 4 },
    };

    ModernMenu colorMenu(m_renderer);
    for (int i = 0; i < GROUP_COLOR_COUNT; i++)
    {
        ModernMenuItem it;
        it.text = Lang::Get(GROUP_COLORS[i].name);
        it.id = GMC_COLOR_BASE + i;
        it.checked = (GROUP_COLORS[i].rgb == curColor);
        it.swatch = GROUP_COLORS[i].rgb;
        colorMenu.items.push_back(it);
    }
    // Свой цвет с кольца — в конце, с разделителем. Если текущий цвет
    // не из пресетов — показываем его свотчем с галкой.
    {
        bool isPreset = false;
        for (int i = 0; i < GROUP_COLOR_COUNT; i++)
            if (GROUP_COLORS[i].rgb == curColor) { isPreset = true; break; }
        ModernMenuItem sep;
        sep.separator = true;
        colorMenu.items.push_back(sep);
        ModernMenuItem custom;
        custom.text = Lang::Get(Str::W_CustomColor);
        custom.id = GMC_COLOR_CUSTOM;
        custom.checked = !isPreset;
        if (!isPreset) custom.swatch = curColor;
        colorMenu.items.push_back(custom);
    }

    ModernMenu gridMenu(m_renderer);
    gridMenu.items = {
        { L"2×2", GMC_GRID_2X2, curGrid == 2 },
        { L"3×3", GMC_GRID_3X3, curGrid == 3 },
    };

    // Новое меню в стиле Windows 11 (Fluent): шапка, пилюля ховера,
    // скругление 8px, Mica/акрил, тёмная тема из персонализации.
    ModernMenu menu(m_renderer);
    menu.items.push_back({ title, 0, false, false, false, true });
    menu.items.push_back({ L"", 0, false, false, true });
    menu.items.push_back({ Lang::Get(Str::W_Open), GMC_OPEN });
    menu.items.push_back({ Lang::Get(Str::W_Rename), GMC_RENAME });
    menu.items.push_back({ Lang::Get(Str::W_HideName), GMC_HIDE_NAME, hideName });
    menu.items.push_back({ Lang::Get(Str::S_Overflow), GMC_SHOW_OVERFLOW, showOv });
    {
        ModernMenuItem sort;
        sort.text = Lang::Get(Str::W_Sorting);
        sort.submenu = &sortMenu;
        menu.items.push_back(sort);
    }
    {
        ModernMenuItem col;
        col.text = Lang::Get(Str::W_Color);
        col.submenu = &colorMenu;
        menu.items.push_back(col);
    }
    {
        ModernMenuItem gr;
        gr.text = Lang::Get(Str::W_GridSize);
        gr.submenu = &gridMenu;
        menu.items.push_back(gr);
    }
    menu.items.push_back({ Lang::Get(Str::W_Ungroup), GMC_UNGROUP });
    menu.items.push_back({ L"", 0, false, false, true });
    menu.items.push_back({ Lang::Get(Str::W_DeleteGroup), GMC_DELETE });

    POINT pt;
    GetCursorPos(&pt);
    int cmd = menu.Show(pt.x, pt.y);

    switch (cmd)
    {
    case GMC_OPEN:
        TogglePopup();
        break;
    case GMC_RENAME:
    {
        std::wstring newName;
        if (m_manager->ShowRenameDialog(m_hwnd, name, newName) && newName != name)
            m_manager->RenameGroup(m_groupId, newName);
        break;
    }
    case GMC_HIDE_NAME:
    {
        bool hide = !(fresh && fresh->hideName);
        m_manager->SetHideName(m_groupId, hide);
        break;
    }
    case GMC_SHOW_OVERFLOW:
    {
        bool show = !(fresh ? fresh->showOverflow : m_group.showOverflow);
        m_manager->SetOverflowBadge(m_groupId, show);
        break;
    }
    case GMC_UNGROUP:
    {
        WCHAR buf[512];
        swprintf_s(buf, Lang::Get(Str::W_UngroupConfirm),
            name.c_str());
        if (MessageBoxW(m_hwnd, buf, Lang::Get(Str::W_UngroupCaption),
                MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES)
        {
            // NOTE: destroys this widget — do not touch members afterwards
            m_manager->UngroupGroup(m_groupId);
        }
        break;
    }
    case GMC_SORT_OFF:
    case GMC_SORT_NAME_ASC:
    case GMC_SORT_NAME_DESC:
    case GMC_SORT_TYPE:
    case GMC_SORT_RECENT:
        m_manager->ApplySort(m_groupId, (GroupSortMode)(cmd - GMC_SORT_OFF));
        break;
    case GMC_GRID_2X2:
        m_manager->SetGridSize(m_groupId, 2);
        break;
    case GMC_GRID_3X3:
        m_manager->SetGridSize(m_groupId, 3);
        break;
    case GMC_COLOR_CUSTOM:
    {
        int rgb = 0;
        const GroupData* g = m_manager->FindGroup(m_groupId);
        if (ShowColorDialog(m_hwnd, m_manager ? m_manager->GetRenderer() : nullptr,
                g ? g->glassColor : 0xFFFFFF, rgb))
            m_manager->SetGroupColor(m_groupId, rgb);
        break;
    }
    case GMC_DELETE:
    {
        WCHAR buf[512];
        swprintf_s(buf, Lang::Get(Str::W_DeleteConfirm),
            name.c_str());
        if (MessageBoxW(m_hwnd, buf, Lang::Get(Str::W_DeleteCaption),
                MB_YESNO | MB_ICONWARNING | MB_TOPMOST) == IDYES)
        {
            // NOTE: destroys this widget — do not touch members afterwards
            m_manager->DeleteGroupWithFiles(m_groupId);
        }
        break;
    }
    default:
        break;
    }

    if (cmd >= GMC_COLOR_BASE && cmd < GMC_COLOR_BASE + GROUP_COLOR_COUNT)
    {
        m_manager->SetGroupColor(m_groupId, GROUP_COLORS[cmd - GMC_COLOR_BASE].rgb);
    }
}

// Общее меню мультивыделения: действия применяются ко всем id сразу.
// Осторожно: разгруппировка/удаление сносят виджеты включая this —
// после них нельзя трогать члены, только return.
void DesktopWidget::ShowBulkMenu(const std::vector<std::wstring>& ids)
{
    if (!m_manager || !m_renderer || ids.size() < 2) return;
    ClearUninstallCache();

    // Общие значения (галка — только если совпали у всех).
    int commonColor = 0xFFFFFF, commonSort = 0, commonGrid = 2;
    bool sameColor = true, sameSort = true, sameGrid = true;
    bool first = true;
    for (const auto& id : ids)
    {
        const GroupData* g = m_manager->FindGroup(id);
        if (!g) continue;
        if (first)
        {
            commonColor = g->glassColor;
            commonSort = g->sortMode;
            commonGrid = g->gridSize;
            first = false;
        }
        else
        {
            if (g->glassColor != commonColor) sameColor = false;
            if (g->sortMode != commonSort) sameSort = false;
            if (g->gridSize != commonGrid) sameGrid = false;
        }
    }
    if (commonSort < 0 || commonSort > 4) { commonSort = 0; sameSort = false; }
    if (commonGrid < 2 || commonGrid > 3) { commonGrid = 2; sameGrid = false; }

    ModernMenu sortMenu(m_renderer);
    sortMenu.items = {
        { Lang::Get(Str::W_SortOff),  GMC_SORT_OFF,      sameSort && commonSort == 0 },
        { L"", 0, false, false, true },
        { Lang::Get(Str::W_SortAsc),  GMC_SORT_NAME_ASC, sameSort && commonSort == 1 },
        { Lang::Get(Str::W_SortDesc), GMC_SORT_NAME_DESC, sameSort && commonSort == 2 },
        { Lang::Get(Str::W_SortType), GMC_SORT_TYPE,     sameSort && commonSort == 3 },
        { Lang::Get(Str::W_SortRecent), GMC_SORT_RECENT, sameSort && commonSort == 4 },
    };

    ModernMenu colorMenu(m_renderer);
    for (int i = 0; i < GROUP_COLOR_COUNT; i++)
    {
        ModernMenuItem it;
        it.text = Lang::Get(GROUP_COLORS[i].name);
        it.id = GMC_COLOR_BASE + i;
        it.checked = sameColor && GROUP_COLORS[i].rgb == commonColor;
        it.swatch = GROUP_COLORS[i].rgb;
        colorMenu.items.push_back(it);
    }

    ModernMenu gridMenu(m_renderer);
    gridMenu.items = {
        { L"2×2", GMC_GRID_2X2, sameGrid && commonGrid == 2 },
        { L"3×3", GMC_GRID_3X3, sameGrid && commonGrid == 3 },
    };

    // Приложения пачки с uninstaller'ом (дедуп по цели, Steam/Appx/реестр).
    struct BulkApp { ShortcutInfo si; UninstallInfo ui; };
    std::vector<BulkApp> apps;
    for (const auto& id : ids)
    {
        const GroupData* g = m_manager->FindGroup(id);
        if (!g) continue;
        for (const auto& s : g->shortcuts)
        {
            std::wstring target = s.targetPath.empty() ? s.lnkPath : s.targetPath;
            bool seen = false;
            for (const auto& a : apps)
            {
                std::wstring at = a.si.targetPath.empty() ? a.si.lnkPath : a.si.targetPath;
                if (_wcsicmp(at.c_str(), target.c_str()) == 0) { seen = true; break; }
            }
            if (seen) continue;
            UninstallInfo ui;
            if (FindUninstallerForTarget(target, ui))
            {
                BulkApp a;
                a.si = s;
                a.ui = ui;
                apps.push_back(std::move(a));
            }
        }
    }

    WCHAR title[64];
    swprintf_s(title, Lang::Get(Str::W_SelCount), (int)ids.size());
    ModernMenu menu(m_renderer);
    menu.items.push_back({ title, 0, false, false, false, true });
    menu.items.push_back({ L"", 0, false, false, true });    {
        ModernMenuItem col;
        col.text = Lang::Get(Str::W_Color);
        col.submenu = &colorMenu;
        menu.items.push_back(col);
    }
    {
        ModernMenuItem gr;
        gr.text = Lang::Get(Str::W_GridSize);
        gr.submenu = &gridMenu;
        menu.items.push_back(gr);
    }
    {
        ModernMenuItem sort;
        sort.text = Lang::Get(Str::W_Sorting);
        sort.submenu = &sortMenu;
        menu.items.push_back(sort);
    }
    menu.items.push_back({ Lang::Get(Str::W_UngroupAll), GMC_BULK_UNGROUP });
    if (!apps.empty())
    {
        WCHAR appsTitle[64];
        swprintf_s(appsTitle, Lang::Get(Str::W_UninstallApps), (int)apps.size());
        ModernMenuItem it;
        it.text = appsTitle;
        it.id = GMC_BULK_UNINSTALL_APPS;
        it.glyph = L'\uE711'; // крестик, как у кнопки панели
        menu.items.push_back(std::move(it));
    }
    menu.items.push_back({ L"", 0, false, false, true });
    menu.items.push_back({ Lang::Get(Str::W_DeleteAll), GMC_BULK_DELETE });
    menu.items.push_back({ L"", 0, false, false, true });
    menu.items.push_back({ Lang::Get(Str::W_ClearSel), GMC_BULK_CLEAR });

    POINT pt;
    GetCursorPos(&pt);
    int cmd = menu.Show(pt.x, pt.y);

    if (cmd >= GMC_COLOR_BASE && cmd < GMC_COLOR_BASE + GROUP_COLOR_COUNT)
    {
        int rgb = GROUP_COLORS[cmd - GMC_COLOR_BASE].rgb;
        for (const auto& id : ids)
            m_manager->SetGroupColor(id, rgb);
        return;
    }

    switch (cmd)
    {
    case GMC_SORT_OFF:
    case GMC_SORT_NAME_ASC:
    case GMC_SORT_NAME_DESC:
    case GMC_SORT_TYPE:
    case GMC_SORT_RECENT:
        for (const auto& id : ids)
            m_manager->ApplySort(id, (GroupSortMode)(cmd - GMC_SORT_OFF));
        break;
    case GMC_GRID_2X2:
        for (const auto& id : ids)
            m_manager->SetGridSize(id, 2);
        break;
    case GMC_GRID_3X3:
        for (const auto& id : ids)
            m_manager->SetGridSize(id, 3);
        break;
    case GMC_BULK_UNGROUP:
    {
        WCHAR buf[512];
        swprintf_s(buf, Lang::Get(Str::W_UngroupAllConfirm), (int)ids.size());
        if (MessageBoxW(m_hwnd, buf, Lang::Get(Str::W_UngroupCaption),
                MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES)
        {
            // NOTE: сносит виджеты включая this — дальше ничего не трогать
            std::vector<std::wstring> copy = ids;
            for (const auto& id : copy)
                m_manager->UngroupGroup(id);
        }
        break;
    }
    case GMC_BULK_UNINSTALL_APPS:
    {
        if (apps.empty()) break;
        WCHAR buf[512];
        swprintf_s(buf, Lang::Get(Str::W_UninstallAppsConfirm), (int)apps.size());
        if (MessageBoxW(m_hwnd, buf, Lang::Get(Str::W_UninstallCaption),
                MB_YESNO | MB_ICONWARNING | MB_TOPMOST) == IDYES)
        {
            // Общее подтверждение уже получено — дальше без лишних вопросов.
            // Группы не трогаем, this жив: удаление приложений группу не сносит.
            for (const auto& a : apps)
                DoUninstallApp(m_hwnd, a.si, a.ui, false);
        }
        break;
    }
    case GMC_BULK_DELETE:
    {
        WCHAR buf[512];
        swprintf_s(buf, Lang::Get(Str::W_DeleteAllConfirm), (int)ids.size());
        if (MessageBoxW(m_hwnd, buf, Lang::Get(Str::W_DeleteCaption),
                MB_YESNO | MB_ICONWARNING | MB_TOPMOST) == IDYES)
        {
            // NOTE: сносит виджеты включая this — дальше ничего не трогать
            std::vector<std::wstring> copy = ids;
            for (const auto& id : copy)
                m_manager->DeleteGroupWithFiles(id);
        }
        break;
    }
    case GMC_BULK_CLEAR:
        m_manager->ClearSelection();
        break;
    default:
        break;
    }
}

void DesktopWidget::OnMouseLeave()
{
    m_glowTarget = 0;
    if (m_glow != m_glowTarget && !m_glowTimer)
        m_glowTimer = SetTimer(m_hwnd, 7, 30, nullptr);
}

void DesktopWidget::RefreshGlow()
{
    if (!m_hwnd) return;
    POINT pt;
    GetCursorPos(&pt);
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    int t = PtInRect(&rc, pt) ? 255 : 0;
    if (t != m_glowTarget)
    {
        m_glowTarget = t;
        if (!m_glowTimer)
            m_glowTimer = SetTimer(m_hwnd, 7, 30, nullptr);
    }
}

void DesktopWidget::OnTimer()
{
    if (m_glow < m_glowTarget)
    {
        m_glow += 64;
        if (m_glow > m_glowTarget) m_glow = m_glowTarget;
    }
    else if (m_glow > m_glowTarget)
    {
        m_glow -= 64;
        if (m_glow < m_glowTarget) m_glow = m_glowTarget;
    }
    else
    {
        if (m_glowTimer) { KillTimer(m_hwnd, m_glowTimer); m_glowTimer = 0; }
        return;
    }
    UpdateBitmap();
}

void DesktopWidget::OnMouseMove(int x, int y)
{
    if (m_dragging)
    {
        POINT pt;
        GetCursorPos(&pt);
        int dx = pt.x - m_pressAbsX;
        int dy = pt.y - m_pressAbsY;
        if (abs(pt.x - m_pressAbsX) > 3 || abs(pt.y - m_pressAbsY) > 3) m_moved = true;

        // Во время тяги — свободное движение (как иконки Windows),
        // привязка к сетке применяется при отпускании в OnLButtonUp.
        // Пачка едет вместе: каждый пир сдвигается на ту же дельту.
        if (!m_dragPeers.empty())
        {
            for (const auto& peer : m_dragPeers)
            {
                if (!peer.w || !peer.w->GetHwnd() || !IsWindow(peer.w->GetHwnd()))
                    continue;
                SetWindowPos(peer.w->GetHwnd(), HWND_TOP,
                    peer.startX + dx, peer.startY + dy, 0, 0,
                    SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
            }
        }
        else
        {
            int nx = pt.x - m_pressX;
            int ny = pt.y - m_pressY;
            SetWindowPos(m_hwnd, HWND_TOP, nx, ny, 0, 0,
                SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        }
        return;
    }

    // Hover glow: трекаем выход мыши и анимируем подсветку
    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hwnd, 0 };
    TrackMouseEvent(&tme);
    if (m_glowTarget != 255)
    {
        m_glowTarget = 255;
        if (!m_glowTimer)
            m_glowTimer = SetTimer(m_hwnd, 7, 30, nullptr);
    }
    (void)x; (void)y;
}

// ---------------------------------------------------------------------------
// PopupWindow
// ---------------------------------------------------------------------------



// --- Проверка занятости: виджет не должен наезжать на файлы/иконки ---
static bool RectsOverlap(const RECT& a, const RECT& b)
{
    return a.left < b.right && a.right > b.left &&
           a.top < b.bottom && a.bottom > b.top;
}

static RECT InflatedRect(RECT r, int pad)
{
    r.left -= pad; r.top -= pad; r.right += pad; r.bottom += pad;
    return r;
}

// Прямоугольники остальных наших виджетов (себя исключаем), чтобы виджеты
// не складывались друг на друга при снапе.
static std::vector<RECT> GetOtherWidgetRects(HWND hSelf)
{
    std::vector<RECT> out;
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, WIDGET_CLASS, nullptr)) != nullptr)
    {
        if (h == hSelf || !IsWindow(h)) continue;
        RECT wr = {};
        if (!GetWindowRect(h, &wr)) continue;
        if (wr.right <= wr.left || wr.bottom <= wr.top) continue;
        out.push_back(wr);
    }
    return out;
}

// Все наши виджеты (включая себя) — для выталкивания иконок после дропа.
std::vector<RECT> CollectAllWidgetRects()
{
    std::vector<RECT> out;
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, WIDGET_CLASS, nullptr)) != nullptr)
    {
        if (!IsWindow(h)) continue;
        RECT wr = {};
        if (!GetWindowRect(h, &wr)) continue;
        if (wr.right <= wr.left || wr.bottom <= wr.top) continue;
        out.push_back(wr);
    }
    return out;
}

// Свободен ли кандидат: внутри рабочей области, без наезда на иконки
// рабочего стола и на другие виджеты.
//
// Что считаем «виджетом»: только непрозрачную часть — стеклянный бокс
// плюс верх подписи (WidgetCell + 8px). Хвост подписи почти прозрачен
// (мелкий текст на стекле), а полный её учёт выгонял виджет на целую
// ячейку из-за пары px — отсюда были большие дыры между группами.
// Что считаем «иконкой»: ячейку со срезанными полями (бока 8, верх 6,
// низ 14 — внизу у иконок пустая подложка). Реальное наложение
// (папка наполовину под виджетом) ловится с запасом.
// Строгая поклеточная модель: занятость — только непрозрачный бокс
// (подпись теперь внутри бокса, хвостов снаружи нет вообще).
// Бокс 2×2 (76px) встаёт в шаг иконок (76×83) с зазором со всех сторон:
// соседние ряды и колонки свободны, дыр и наездов нет.
static RECT WidgetCollisionRect(int x, int y, const GroupData& group)
{
    int box = WidgetCell(group);
    int cw = box - 6;
    int ch = box - 6;
    if (cw < 8) cw = 8;
    if (ch < 8) ch = 8;
    return { x + 3, y + 3, x + 3 + cw, y + 3 + ch };
}

static RECT ShrinkIconRect(RECT ic)
{
    RECT r = { ic.left + 8, ic.top + 6, ic.right - 8, ic.bottom - 14 };
    if (r.right <= r.left || r.bottom <= r.top)
        return ic;
    return r;
}

static bool IsWidgetSpotFree(int x, int y, const GroupData& group,
    const RECT& work,
    const std::vector<RECT>& icons, const std::vector<RECT>& widgets)
{
    int fullW = WidgetWidth(group);
    int fullH = WidgetHeight(group);
    if (x < work.left || y < work.top ||
        x + fullW > work.right || y + fullH > work.bottom)
        return false;
    RECT coll = WidgetCollisionRect(x, y, group);
    for (const RECT& ic : icons)
    {
        if (RectsOverlap(coll, ShrinkIconRect(ic)))
            return false;
    }
    // Другие виджеты — тем же боксом (подписи у всех внутри).
    for (const RECT& w : widgets)
    {
        RECT oc = { w.left + 3, w.top + 3, w.right - 3, w.bottom - 3 };
        if (oc.right > oc.left && oc.bottom > oc.top &&
            RectsOverlap(coll, oc))
            return false;
    }
    return true;
}

static void GridParamsForPoint(POINT pt, const RECT& work,
    int& stepX, int& stepY, int& baseX, int& baseY)
{
    // Сетка своего монитора: те же ряды, что у ярлыков/папок на нём.
    // Fallback: системные метрики интервалов иконок.
    DesktopGrid dg = QueryDesktopGridForPoint(pt);
    if (dg.valid)
    {
        stepX = dg.stepX;
        stepY = dg.stepY;
        baseX = dg.originX;
        baseY = dg.originY;
    }
    else
    {
        stepX = GetSystemMetrics(SM_CXICONSPACING);
        stepY = GetSystemMetrics(SM_CYICONSPACING);
        if (stepX < 48) stepX = SNAP_GRID_X;
        if (stepY < 48) stepY = SNAP_GRID_Y;
        baseX = work.left;
        baseY = work.top;
    }
    if (stepX < 16) stepX = 80;
    if (stepY < 16) stepY = 90;
}

static int RoundDiv(int v, int step)
{
    return (v >= 0) ? (v + step / 2) / step : (v - step / 2) / step;
}

// Привязка позиции виджета к сетке своего монитора — строго по ячейкам,
// без пиксельных микросдвигов: ряды виджетов совпадают с рядами иконок.
// Свобода ячейки — по урезанной коллизии (бокс в пределах шага), поэтому
// штатный случай никогда не прыгает: ни наездов, ни дыр, ни «лесенки».
static POINT SnapPointToGrid(POINT pt, const GroupData& group, HWND hSelf)
{
    int widgetW = WidgetWidth(group);
    int widgetH = WidgetHeight(group);
    RECT work = MonitorWorkRect(pt);
    int stepX, stepY, baseX, baseY;
    GridParamsForPoint(pt, work, stepX, stepY, baseX, baseY);

    int qx = RoundDiv(pt.x - baseX, stepX);
    int qy = RoundDiv(pt.y - baseY, stepY);
    int x = baseX + qx * stepX;
    int y = baseY + qy * stepY;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    if (x + widgetW > work.right) x = work.right - widgetW;
    if (y + widgetH > work.bottom) y = work.bottom - widgetH;

    std::vector<RECT> icons = QueryDesktopIconRects();
    std::vector<RECT> widgets = GetOtherWidgetRects(hSelf);

    // Точная точка сетки.
    if (IsWidgetSpotFree(x, y, group, work, icons, widgets))
        return { x, y };

    // Соседние ячейки сетки кольцами: ближайшее свободное место.
    int sqx = RoundDiv(x - baseX, stepX);
    int sqy = RoundDiv(y - baseY, stepY);
    const int kMaxRing = 40;
    for (int r = 1; r <= kMaxRing; r++)
    {
        for (int dx = -r; dx <= r; dx++)
        {
            for (int dy = -r; dy <= r; dy++)
            {
                if (dx != r && dx != -r && dy != r && dy != -r)
                    continue;
                int cx = baseX + (sqx + dx) * stepX;
                int cy = baseY + (sqy + dy) * stepY;
                if (IsWidgetSpotFree(cx, cy, group, work, icons, widgets))
                    return { cx, cy };
            }
        }
    }
    return { x, y }; // всё занято — лучше встать по сетке, чем улететь
}

// Режим без сетки: оставляем позицию пользователя, но если виджет лёг
// поверх иконки/виджета — сдвигаем до ближайшего свободного пикселя.
static POINT NudgeToFreeSpot(POINT pt, const GroupData& group, HWND hSelf)
{
    int widgetW = WidgetWidth(group);
    int widgetH = WidgetHeight(group);
    RECT work = MonitorWorkRect(pt);
    int x = pt.x, y = pt.y;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    if (x + widgetW > work.right) x = work.right - widgetW;
    if (y + widgetH > work.bottom) y = work.bottom - widgetH;

    std::vector<RECT> icons = QueryDesktopIconRects();
    std::vector<RECT> widgets = GetOtherWidgetRects(hSelf);

    if (IsWidgetSpotFree(x, y, group, work, icons, widgets))
        return { x, y };

    const int kStep = 8;
    const int kMaxRing = 48; // ~384px вокруг точки дропа
    for (int r = 1; r <= kMaxRing; r++)
    {
        for (int dx = -r; dx <= r; dx++)
        {
            for (int dy = -r; dy <= r; dy++)
            {
                if (dx != r && dx != -r && dy != r && dy != -r)
                    continue;
                int cx = x + dx * kStep;
                int cy = y + dy * kStep;
                if (IsWidgetSpotFree(cx, cy, group, work, icons, widgets))
                    return { cx, cy };
            }
        }
    }
    return { x, y };
}

