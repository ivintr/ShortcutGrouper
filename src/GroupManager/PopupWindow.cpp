// PopupWindow: раскрывающееся окно группы (было в DesktopWidget.cpp).
#include "PopupWindow.h"
#include "DesktopWidget.h"   // DoUpdateLayered, GetGlobalManager, классы окон
#include "DesktopGrid.h"     // MonitorWorkRect
#include "WidgetManager.h"   // менеджер, FindPopupByGroupId
#include "SettingsDialog.h"  // Settings::
#include "ModernMenu.h"
#include "Lang.h"
#include "Reg.h"
#include "ShellQuote.h"
#include <vector>
#include <memory>
#include <algorithm>
#include <climits>
#include <string>
#include <cstdio>
#include <cwctype>
#include <cstdlib>
#include <mutex>
#include <shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <shlwapi.h>
#include <objbase.h>
#include <oleidl.h>
// ---------------------------------------------------------------------------
// Minimal IDataObject for DoDragDrop from popup.
// Offers CF_HDROP (file path, for drops outside) plus a private format
// identifying the source group+index (for reorder drops inside a popup).
// ---------------------------------------------------------------------------

struct GroupDropPayload {
    WCHAR groupId[40];
    int index;
};

static UINT GroupDropFormat()
{
    static UINT cf = 0;
    if (!cf) cf = RegisterClipboardFormatW(L"DesktopGroupManagerShortcut");
    return cf;
}

static void GroupDropFormatEtc(FORMATETC& f)
{
    f.cfFormat = (CLIPFORMAT)GroupDropFormat();
    f.ptd = nullptr;
    f.dwAspect = DVASPECT_CONTENT;
    f.lindex = -1;
    f.tymed = TYMED_HGLOBAL;
}

class LnkJunkDataObject : public IDataObject {
public:
    LnkJunkDataObject(const std::wstring& lnkPath, const std::wstring& groupId, int index)
        : m_ref(1), m_lnkPath(lnkPath), m_groupId(groupId), m_index(index) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
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

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* pFmt, STGMEDIUM* pStg) override {
        if (!pFmt || !pStg) return E_INVALIDARG;
        if (pFmt->cfFormat == (CLIPFORMAT)GroupDropFormat() && (pFmt->tymed & TYMED_HGLOBAL))
        {
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(GroupDropPayload));
            if (!hMem) return E_OUTOFMEMORY;
            GroupDropPayload* p = (GroupDropPayload*)GlobalLock(hMem);
            wcsncpy_s(p->groupId, m_groupId.c_str(), _TRUNCATE);
            p->index = m_index;
            GlobalUnlock(hMem);
            pStg->tymed = TYMED_HGLOBAL;
            pStg->hGlobal = hMem;
            pStg->pUnkForRelease = nullptr;
            return S_OK;
        }
        if (pFmt->cfFormat != CF_HDROP || !(pFmt->tymed & TYMED_HGLOBAL))
            return DV_E_FORMATETC;

        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(DROPFILES) + (m_lnkPath.size() + 2) * sizeof(wchar_t));
        if (!hMem) return E_OUTOFMEMORY;

        DROPFILES* pDrop = (DROPFILES*)GlobalLock(hMem);
        pDrop->pFiles = sizeof(DROPFILES);
        pDrop->fWide = TRUE;
        wchar_t* pDst = (wchar_t*)((BYTE*)pDrop + sizeof(DROPFILES));
        wcscpy_s(pDst, m_lnkPath.size() + 1, m_lnkPath.c_str());
        pDst[m_lnkPath.size() + 1] = 0;
        GlobalUnlock(hMem);

        pStg->tymed = TYMED_HGLOBAL;
        pStg->hGlobal = hMem;
        pStg->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* pFmt) override {
        if (!pFmt) return E_INVALIDARG;
        if (pFmt->cfFormat == CF_HDROP && (pFmt->tymed & TYMED_HGLOBAL)) return S_OK;
        if (pFmt->cfFormat == (CLIPFORMAT)GroupDropFormat() && (pFmt->tymed & TYMED_HGLOBAL)) return S_OK;
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* pOut) override {
        if (pOut) { *pOut = {}; pOut->cfFormat = CF_HDROP; pOut->tymed = TYMED_HGLOBAL; pOut->dwAspect = DVASPECT_CONTENT; pOut->lindex = -1; pOut->ptd = nullptr; }
        return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ULONG m_ref;
    std::wstring m_lnkPath;
    std::wstring m_groupId;
    int m_index = -1;
};

// ---------------------------------------------------------------------------
// Minimal IDropSource for DoDragDrop from popup
// ---------------------------------------------------------------------------

class LnkJunkDropSource : public IDropSource {
public:
    LnkJunkDropSource() : m_ref(1) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
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

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL fEscPressed, DWORD grfKeyState) override {
        if (fEscPressed) return DRAGDROP_S_CANCEL;
        if (!(grfKeyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    ULONG m_ref;
};

// ---------------------------------------------------------------------------
// IDropTarget on the popup: accepts our own shortcut drags so dropping
// inside reorders instead of removing from the group.
// ---------------------------------------------------------------------------

static PopupWindow* PopupFromHWND(HWND hWnd)
{
    return (PopupWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
}

class PopupDropTarget : public IDropTarget {
public:
    PopupDropTarget(HWND hwnd) : m_ref(1), m_hwnd(hwnd) {}

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

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD, POINTL, DWORD* pdwEffect) override {
        m_accept = false;
        if (pDataObj)
        {
            FORMATETC f = {};
            GroupDropFormatEtc(f);
            m_accept = SUCCEEDED(pDataObj->QueryGetData(&f));
        }
        *pdwEffect = m_accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (PopupWindow* p = PopupFromHWND(m_hwnd))
        {
            p->m_dropInside = false;
            p->m_dropIndex = -1;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* pdwEffect) override {
        if (!m_accept)
        {
            *pdwEffect = DROPEFFECT_NONE;
            return S_OK;
        }
        *pdwEffect = DROPEFFECT_COPY;
        if (PopupWindow* p = PopupFromHWND(m_hwnd))
        {
            POINT cpt = { pt.x, pt.y };
            ScreenToClient(m_hwnd, &cpt);
            int idx = -1;
            if (p->m_renderer)
                idx = p->m_renderer->HitTestPopup(p->m_group, cpt.x, cpt.y, p->m_scrollY);
            p->m_dropIndex = idx;
            if (idx != p->m_hovered)
            {
                p->m_hovered = idx;
                p->UpdateBitmap(); // живая подсветка цели под курсором
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (PopupWindow* p = PopupFromHWND(m_hwnd))
        {
            p->m_dropInside = false;
            p->m_dropIndex = -1;
            if (p->m_hovered != -1)
            {
                p->m_hovered = -1;
                p->UpdateBitmap();
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL pt, DWORD* pdwEffect) override {
        *pdwEffect = m_accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (m_accept)
        {
            if (PopupWindow* p = PopupFromHWND(m_hwnd))
            {
                POINT cpt = { pt.x, pt.y };
                ScreenToClient(m_hwnd, &cpt);
                if (p->m_renderer)
                    p->m_dropIndex = p->m_renderer->HitTestPopup(
                        p->m_group, cpt.x, cpt.y, p->m_scrollY);
                p->m_dropInside = true;
            }
        }
        return S_OK;
    }

private:
    ULONG m_ref;
    HWND m_hwnd = nullptr;
    bool m_accept = false;
};

static HHOOK s_hMouseHook = nullptr;
static PopupWindow* s_activePopup = nullptr;


static bool popupDragging = false;
static std::wstring popupDragGroupId;
static int popupDragIndex = -1;
static std::wstring popupDragGroupName;

static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && s_activePopup && s_activePopup->IsVisible() && !popupDragging)
    {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN)
        {
            MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
            HWND hPopup = s_activePopup->GetHwnd();
            if (hPopup)
            {
                RECT rc;
                GetWindowRect(hPopup, &rc);
                if (!PtInRect(&rc, p->pt))
                {
                    LogTiming(L"popup-hookclose", 0);
                    s_activePopup->CloseAnimated();
                }
            }
        }
    }
    return CallNextHookEx(s_hMouseHook, nCode, wParam, lParam);
}

void InstallMouseHook()
{
    if (!s_hMouseHook)
        s_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, nullptr, 0);
}

// Клавиатура попапа без фокуса: низкоуровневый хук живёт пока открыт попап.
// Действует только когда foreground — сам попап (иначе клавиши уходят
// в активное приложение): модальные диалоги, меню и чужие окна не трогаем.
// Хук меню ставится позже и глотает своё (порядок LIFO) — двойных
// срабатываний нет.

void UninstallMouseHook()
{
    if (s_hMouseHook)
    {
        UnhookWindowsHookEx(s_hMouseHook);
        s_hMouseHook = nullptr;
    }
}

static HHOOK s_hPopupKbdHook = nullptr;

static LRESULT CALLBACK PopupKbdProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
        s_activePopup && s_activePopup->IsVisible() && !s_activePopup->IsClosing())
    {
        HWND hPopup = s_activePopup->GetHwnd();
        // Попап NOACTIVATE (foreground не берёт), поэтому разрешаем клавиши,
        // когда foreground — сам попап (паранойя) или рабочий стол, с которого
        // его открыли. Чужое активное окно (диалоги, браузер) — не трогаем.
        bool ok = false;
        if (hPopup && IsWindow(hPopup))
        {
            HWND hFore = GetForegroundWindow();
            if (hFore == hPopup)
                ok = true;
            else if (hFore && IsWindow(hFore))
            {
                wchar_t cls[64] = {};
                GetClassNameW(hFore, cls, _countof(cls));
                if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW"))
                    ok = true;
            }
        }
        if (ok)
        {
            KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
            WPARAM vk = p ? p->vkCode : 0;
            switch (vk)
            {
            case VK_ESCAPE:
            case VK_UP:
            case VK_DOWN:
            case VK_HOME:
            case VK_END:
            case VK_RETURN:
            case VK_SPACE:
                s_activePopup->OnKeyDown(vk);
                return 1; // поглотили: проводнику/приложениям не отдавать
            default:
                break;
            }
        }
    }
    return CallNextHookEx(s_hPopupKbdHook, nCode, wParam, lParam);
}

static void InstallPopupKbdHook()
{
    // Один на всё время жизни процесса (дешевле, чем считать попапы):
    // срабатывает только при видимом активном попапе, см. выше.
    if (!s_hPopupKbdHook)
        s_hPopupKbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, PopupKbdProc, nullptr, 0);
}

int PopupWindow::FullListH() const
{
    return (int)m_group.shortcuts.size() * POPUP_ITEM_H;
}

int PopupWindow::MaxScroll() const
{
    int m = FullListH() - VisListH();
    return m > 0 ? m : 0;
}

void PopupWindow::ClampScroll()
{
    if (m_scrollY < 0) m_scrollY = 0;
    int ms = MaxScroll();
    if (m_scrollY > ms) m_scrollY = ms;
}

int PopupWindow::VisListH() const
{
    int vis = (m_customListH >= 0) ? m_customListH : FullListH();
    if (m_maxListH >= 0 && vis > m_maxListH) vis = m_maxListH;
    if (vis < 0) vis = 0;
    return vis;
}

int PopupWindow::VisibleH() const
{
    return POPUP_HEADER_H + VisListH() + POPUP_PAD * 2;
}

PopupWindow::ResizeMode PopupWindow::HitBorder(int x, int y) const
{
    int w = m_popupW;
    int h = VisibleH();
    bool cL = x < POPUP_CORNER_SZ;
    bool cR = x >= w - POPUP_CORNER_SZ;
    bool cT = y < POPUP_CORNER_SZ;
    bool cB = y >= h - POPUP_CORNER_SZ;
    if (cT && cL) return ResizeMode::TopLeft;
    if (cT && cR) return ResizeMode::TopRight;
    if (cB && cL) return ResizeMode::BottomLeft;
    if (cB && cR) return ResizeMode::BottomRight;
    if (x < POPUP_BORDER_SZ) return ResizeMode::Left;
    if (x >= w - POPUP_BORDER_SZ) return ResizeMode::Right;
    if (y < POPUP_BORDER_SZ) return ResizeMode::Top;
    if (y >= h - POPUP_BORDER_SZ) return ResizeMode::Bottom;
    return ResizeMode::None;
}

static LPCWSTR CursorForResizeMode(PopupWindow::ResizeMode m)
{
    using RM = PopupWindow::ResizeMode;
    switch (m)
    {
    case RM::Left:
    case RM::Right: return IDC_SIZEWE;
    case RM::Top:
    case RM::Bottom: return IDC_SIZENS;
    case RM::TopLeft:
    case RM::BottomRight: return IDC_SIZENWSE;
    case RM::TopRight:
    case RM::BottomLeft: return IDC_SIZENESW;
    default: return IDC_ARROW;
    }
}

void PopupWindow::BeginResize(ResizeMode mode)
{
    EndResize();
    if (m_dragging)
    {
        m_dragging = false;
        ReleaseCapture();
    }
    m_resizing = true;
    m_resizeMode = mode;
    GetCursorPos(&m_resizeStartPt);
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    m_resizeStartX = rc.left;
    m_resizeStartY = rc.top;
    m_resizeStartW = rc.right - rc.left;
    m_resizeStartListH = VisListH();
    SetCapture(m_hwnd);
}

void PopupWindow::UpdateResize()
{
    POINT pt;
    GetCursorPos(&pt);
    RECT work = MonitorWorkRect(pt);

    int maxW = (work.right - work.left) - 16;
    if (maxW > POPUP_MAX_W) maxW = POPUP_MAX_W;
    if (maxW < POPUP_MIN_W) maxW = POPUP_MIN_W;

    int lo = POPUP_ITEM_H; // минимум — один ряд
    if (lo > FullListH()) lo = FullListH();
    // Верхний предел — лимит экрана: можно тянуть выше контента,
    // внизу останется пустое стекло
    int hi = (m_maxListH >= 0) ? m_maxListH : FullListH();
    if (hi < lo) hi = lo;

    int dx = pt.x - m_resizeStartPt.x;
    int dy = pt.y - m_resizeStartPt.y;
    int x = m_resizeStartX;
    int y = m_resizeStartY;
    int w = m_resizeStartW;
    int list = m_resizeStartListH;

    using RM = ResizeMode;
    bool west = (m_resizeMode == RM::Left || m_resizeMode == RM::TopLeft || m_resizeMode == RM::BottomLeft);
    bool east = (m_resizeMode == RM::Right || m_resizeMode == RM::TopRight || m_resizeMode == RM::BottomRight);
    bool north = (m_resizeMode == RM::Top || m_resizeMode == RM::TopLeft || m_resizeMode == RM::TopRight);
    bool south = (m_resizeMode == RM::Bottom || m_resizeMode == RM::BottomLeft || m_resizeMode == RM::BottomRight);

    if (east) w = m_resizeStartW + dx;
    if (west) { w = m_resizeStartW - dx; x = m_resizeStartX + dx; }
    if (w < POPUP_MIN_W) { if (west) x -= (POPUP_MIN_W - w); w = POPUP_MIN_W; }
    if (w > maxW) { if (west) x += (w - maxW); w = maxW; }

    if (south) list = m_resizeStartListH + dy;
    if (north) list = m_resizeStartListH - dy;
    if (list < lo) list = lo;
    if (list > hi) list = hi;

    // При ресайзе за верхний край двигаем окно так, чтобы низ стоял на месте
    int startVisH = POPUP_HEADER_H + m_resizeStartListH + POPUP_PAD * 2;
    int newVisH = POPUP_HEADER_H + list + POPUP_PAD * 2;
    if (north) y = m_resizeStartY + (startVisH - newVisH);

    if (x < work.left) x = work.left;
    if (x + w > work.right + 32) x = work.right + 32 - w;
    if (y < work.top) y = work.top;
    if (y + newVisH > work.bottom + 32) y = work.bottom + 32 - newVisH;

    bool changed = (w != m_popupW) || (list != VisListH());
    m_popupW = w;
    m_customListH = list;
    ClampScroll();

    SetWindowPos(m_hwnd, nullptr, x, y, m_popupW, VisibleH(),
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);

    if (!changed)
        return; // только сдвиг окна — перерисовка не нужна
    if (m_renderer && m_hwnd)
    {
        POINT cpt = pt;
        ScreenToClient(m_hwnd, &cpt);
        m_hovered = m_renderer->HitTestPopup(m_group, cpt.x, cpt.y, m_scrollY);
    }
    UpdateBitmap(); // быстрый режим: фон из кэша, без захвата экрана
}

void PopupWindow::EndResize()
{
    if (m_resizing)
    {
        m_resizing = false;
        m_resizeMode = ResizeMode::None;
        ReleaseCapture();
    }
}

// Сколько px списка влезет на монитор в точке pt (с запасом под позицию
// над/под виджетом). Минимум — 3 элемента, дальше скролл.
static int PopupMaxListHForPoint(POINT pt)
{
    RECT work = MonitorWorkRect(pt);
    int avail = (work.bottom - work.top) - POPUP_HEADER_H - POPUP_PAD * 2 - 64;
    int minList = 3 * POPUP_ITEM_H;
    if (avail < minList) avail = minList;
    return avail;
}

bool PopupWindow::Create(HWND hParent, const GroupData& group, WidgetRenderer* renderer)
{
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = PopupWindow::WndProc;
        wc.hInstance      = GetModuleHandleW(nullptr);
        wc.lpszClassName  = POPUP_CLASS;
        wc.hbrBackground  = nullptr;
        RegisterClassExW(&wc);
        registered = true;
    }

    m_group = group;
    m_groupId = group.id;
    m_renderer = renderer;
    m_scrollY = 0;
    m_hovered = -1;
    m_popupW = group.popupW;
    if (m_popupW < POPUP_MIN_W || m_popupW > POPUP_MAX_W) m_popupW = POPUP_W;
    m_customListH = -1;
    m_resizing = false;

    POINT cur = {};
    GetCursorPos(&cur);
    m_maxListH = PopupMaxListHForPoint(cur);

    // Восстанавливаем запомненный размер (если был сохранён)
    if (group.popupListH >= 0)
    {
        int lo = POPUP_ITEM_H;
        if (lo > FullListH()) lo = FullListH();
        m_customListH = group.popupListH;
        if (m_customListH < lo) m_customListH = lo;
        if (m_customListH > m_maxListH) m_customListH = m_maxListH;
    }

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    // Без WS_EX_TOPMOST: попап временный (закрывается кликом мимо),
    // ему нечего делать поверх рабочих окон.
    // NOACTIVATE оставляем: фокус не трогаем вообще (иначе гонка
    // с закрытием по деактивации), клавиатура идёт через LL-хук ниже.
    m_hwnd = CreateWindowExW(exStyle, POPUP_CLASS, L"",
        WS_POPUP, 0, 0, m_popupW, VisibleH(),
        nullptr, nullptr, GetModuleHandleW(nullptr), this);

    if (!m_hwnd) return false;

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);

    m_dropTarget = new PopupDropTarget(m_hwnd);
    if (FAILED(RegisterDragDrop(m_hwnd, m_dropTarget)))
    {
        m_dropTarget->Release();
        m_dropTarget = nullptr;
    }

    m_visible = true;
    RegisterPopupWindow(this);
    // Без UpdateBitmap здесь: окно ещё не спозиционировано (ShowNear сделает
    // move + рендер), слой без битмапа невидим — лишнего захвата в (0,0) нет.
    InstallMouseHook();
    InstallPopupKbdHook();
    s_activePopup = this;
    return true;
}

void PopupWindow::Destroy()
{
    LogTiming(L"popup-destroy", 0);
    if (s_activePopup == this) s_activePopup = nullptr;
    UnregisterPopupWindow(this);
    if (m_animTimer) { KillTimer(m_hwnd, m_animTimer); m_animTimer = 0; }
    m_closing = false;
    m_animAlpha = 255;
    m_visible = false;
    m_hovered = -1;
    m_scrollY = 0;
    m_resizing = false;
    m_moving = false;
    m_dropInside = false;
    m_dropIndex = -1;
    SetCursor(LoadCursorW(nullptr, IDC_ARROW)); // не оставлять ресайз-курсор
    if (m_hwnd)
    {
        if (m_dropTarget)
        {
            RevokeDragDrop(m_hwnd);
            m_dropTarget->Release();
            m_dropTarget = nullptr;
        }
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_hBitmap)
    {
        DeleteObject(m_hBitmap);
        m_hBitmap = nullptr;
    }
}

void PopupWindow::Update(const GroupData& group)
{
    m_group = group;
    ClampScroll();
    if (m_hovered >= (int)m_group.shortcuts.size())
        m_hovered = -1;
    UpdateBitmap();
}

void PopupWindow::ShowNear(int cx, int cy)
{
    if (!m_hwnd) return;

    POINT pt = { cx, cy };
    m_maxListH = PopupMaxListHForPoint(pt);
    ClampScroll();
    int visH = VisibleH();

    RECT work = MonitorWorkRect(pt);

    int x = cx - m_popupW / 2;
    int y = cy - visH - 8; // пробуем над виджетом
    if (y < work.top + 4)
    {
        y = cy + 8; // иначе под виджетом
        if (y + visH > work.bottom - 4)
            y = work.bottom - visH - 4;
        if (y < work.top + 4)
            y = work.top + 4;
    }

    if (x < work.left + 4) x = work.left + 4;
    if (x + m_popupW > work.right - 4) x = work.right - m_popupW - 4;

    // Рендер до показа: окно скрыто — захват фона чистый, вспышки нет
    if (m_hovered < 0 && !m_group.shortcuts.empty())
        m_hovered = 0; // стартовый пункт для клавиатуры (и мыши)
    SetWindowPos(m_hwnd, HWND_TOP, x, y, m_popupW, visH,
        SWP_NOACTIVATE);
    UpdateBitmap();
    ShowWindow(m_hwnd, SW_SHOWNA);

    // Анимация открытия: fade-in + подъём на 10px
    m_closing = false;
    m_animAlpha = 0;
    m_animX = x;
    m_animTargetY = y;
    m_animY = y + 10;
    SetWindowPos(m_hwnd, nullptr, x, m_animY, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    UpdateBitmap();
    if (m_animTimer) KillTimer(m_hwnd, m_animTimer);
    m_animTimer = SetTimer(m_hwnd, 8, 30, nullptr);
}

void PopupWindow::CloseAnimated()
{
    if (!m_hwnd || m_closing) return;
    LogTiming(L"popup-closeanim", 0);
    m_closing = true;
    if (m_animTimer) KillTimer(m_hwnd, m_animTimer);
    m_animTimer = SetTimer(m_hwnd, 8, 30, nullptr);
}

void PopupWindow::CancelClose()
{
    if (!m_closing || !m_hwnd) return;
    m_closing = false;
    if (m_animTimer) { KillTimer(m_hwnd, m_animTimer); m_animTimer = 0; }
    m_animAlpha = 255;
    if (m_hBitmap)
        DoUpdateLayered(m_hwnd, m_hBitmap, m_popupW, VisibleH(), 255);
}

void PopupWindow::OnTimer()
{
    if (!m_hwnd || !m_hBitmap)
    {
        if (m_animTimer) { KillTimer(m_hwnd, m_animTimer); m_animTimer = 0; }
        return;
    }
    if (!m_closing)
    {
        // Открытие: fade-in + подъём
        m_animAlpha += 51;
        m_animY -= 2;
        if (m_animAlpha >= 255 || m_animY <= m_animTargetY)
        {
            m_animAlpha = 255;
            m_animY = m_animTargetY;
            if (m_animTimer) { KillTimer(m_hwnd, m_animTimer); m_animTimer = 0; }
        }
        SetWindowPos(m_hwnd, nullptr, m_animX, m_animY, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        DoUpdateLayered(m_hwnd, m_hBitmap, m_popupW, VisibleH(), (BYTE)m_animAlpha);
    }
    else
    {
        // Закрытие: fade-out, затем destroy
        m_animAlpha -= 64;
        if (m_animAlpha <= 0)
        {
            if (m_animTimer) { KillTimer(m_hwnd, m_animTimer); m_animTimer = 0; }
            Destroy();
            return;
        }
        DoUpdateLayered(m_hwnd, m_hBitmap, m_popupW, VisibleH(), (BYTE)m_animAlpha);
    }
}

void PopupWindow::CloseAll()
{
    if (s_activePopup)
    {
        s_activePopup->Destroy();
        s_activePopup = nullptr;
    }
}

void PopupWindow::UpdateBitmap()
{
    if (!m_renderer || !m_hwnd) return;
    ULONGLONG tb0 = GetTickCount64();
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    ClampScroll();
    auto result = m_renderer->RenderPopup(m_group, m_hovered, rc.left, rc.top,
        m_scrollY, m_maxListH, m_popupW, m_resizing, VisListH());
    if (m_hBitmap) DeleteObject(m_hBitmap);
    m_hBitmap = result.hBitmap;
    if (m_hwnd && m_hBitmap)
        DoUpdateLayered(m_hwnd, m_hBitmap, result.width, result.height,
            (BYTE)(m_animAlpha < 0 ? 0 : (m_animAlpha > 255 ? 255 : m_animAlpha)));
    ULONGLONG dt = GetTickCount64() - tb0;
    if (dt > 40)
        LogTiming(L"popup-bitmap-slow", dt);
}

void PopupWindow::OnMouseWheel(int delta)
{
    if (m_closing) return;
    if (MaxScroll() <= 0) return;
    // Один тик колеса — два элемента списка
    m_scrollY -= (int)((long long)delta * POPUP_ITEM_H * 2 / WHEEL_DELTA);
    ClampScroll();
    // Hover пересчитываем под текущей позицией курсора
    if (m_renderer && m_hwnd)
    {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(m_hwnd, &pt);
        m_hovered = m_renderer->HitTestPopup(m_group, pt.x, pt.y, m_scrollY);
    }
    UpdateBitmap();
}

void PopupWindow::UpdateLayered()
{
}

LRESULT CALLBACK PopupWindow::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PopupWindow* self = (PopupWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

    switch (uMsg)
    {
    case WM_PAINT:
        if (self) self->OnPaint();
        return 0;

    case WM_LBUTTONDOWN:
        if (self) self->OnLButtonDown(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_LBUTTONUP:
        if (self) self->OnLButtonUp(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_RBUTTONUP:
        if (self) self->OnRButtonUp(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_CONTEXTMENU:
        if (self)
        {
            int sx = (short)LOWORD(lParam), sy = (short)HIWORD(lParam);
            if (sx == -1 && sy == -1)
                self->OnContextMenu({}, true);
            else
                self->OnContextMenu({ sx, sy }, false);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (self) self->OnMouseMove(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_MOUSEWHEEL:
        if (self) self->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;

    case WM_TIMER:
        if (self) self->OnTimer();
        return 0;

    case WM_KEYDOWN:
        if (self) self->OnKeyDown(wParam);
        return 0;

    case WM_ACTIVATE:
        if (self)
        {
            LogTiming(L"popup-activate", (ULONGLONG)LOWORD(wParam));
            if (LOWORD(wParam) == WA_INACTIVE && !self->m_dragging)
                self->Destroy();
        }
        return 0;

    case WM_NCHITTEST:
        return HTCLIENT;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void PopupWindow::OnPaint()
{
}

void PopupWindow::OnLButtonDown(int x, int y)
{
    if (m_closing) return;
    if (!m_renderer) return;

    ResizeMode rm = HitBorder(x, y);
    if (rm != ResizeMode::None)
    {
        BeginResize(rm);
        return;
    }

    // Шапка (заголовок) — перетаскивание окна, как за title bar
    if (y < POPUP_HEADER_H)
    {
        EndResize();
        if (m_dragging)
        {
            m_dragging = false;
            ReleaseCapture();
        }
        m_moving = true;
        m_moveDX = x;
        m_moveDY = y;
        SetCapture(m_hwnd);
        return;
    }

    int idx = m_renderer->HitTestPopup(m_group, x, y, m_scrollY);
    OutputDebugStringW(L"Popup OnLButtonDown hitTest=");
    OutputDebugStringW(std::to_wstring(idx).c_str());
    OutputDebugStringW(L"\n");
    if (idx >= 0 && idx < (int)m_group.shortcuts.size())
    {
        m_dragIndex = idx;
        m_pressX = x;
        m_pressY = y;
        RECT rc;
        GetWindowRect(m_hwnd, &rc);
        m_pressAbsX = rc.left + x;
        m_pressAbsY = rc.top + y;
        m_moved = false;
        m_dragging = true;
        SetCapture(m_hwnd);
    }
}

void PopupWindow::OnLButtonUp(int x, int y)
{
    if (m_closing) return;
    if (m_resizing)
    {
        EndResize();
        if (GetGlobalManager())
            GetGlobalManager()->SavePopupSize(m_groupId, m_popupW, m_customListH);
        UpdateBitmap(); // чёткий фон на финальной геометрии
        return;
    }

    if (m_moving)
    {
        m_moving = false;
        ReleaseCapture();
        UpdateBitmap(); // фон перезахватывается на новом месте
        return;
    }

    if (!m_dragging) return;

    ReleaseCapture();
    m_dragging = false;

    if (!m_moved && m_dragIndex >= 0 && m_dragIndex < (int)m_group.shortcuts.size())
    {
        int idx = m_dragIndex;
        m_dragIndex = -1;
        LaunchShortcut(idx);
    }
}

void PopupWindow::LaunchShortcut(int idx)
{
    if (m_closing) return;
    if (idx >= 0 && idx < (int)m_group.shortcuts.size())
    {
        const ShortcutInfo si = m_group.shortcuts[idx]; // копия: группа ниже может измениться
        if (Settings::IsAutoPruneDead() && !WidgetManager::IsShortcutAlive(si))
        {
            // Приложение удалили, а ярлык остался — чистим сразу по клику.
            WidgetManager* mgr = GetGlobalManager();
            std::wstring gid = m_groupId;
            std::wstring disp = si.name.empty() ? si.lnkPath : si.name;
            if (mgr) mgr->RemoveDeadShortcut(gid, idx);
            WCHAR buf[512];
            swprintf_s(buf, Lang::Get(Str::W_DeadMsg),
                disp.c_str());
            MessageBoxW(m_hwnd, buf, Lang::Get(Str::W_DeadCaption),
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
            if (!mgr || !mgr->FindGroup(gid)) { Destroy(); return; }
            for (const auto& g : mgr->GetGroups())
            {
                if (g.id == gid) { Update(g); break; }
            }
            m_dragIndex = -1;
            return;
        }
        // Запускаем сам .lnk (сохраняются аргументы, рабочая папка и IconLocation
        // ярлыка). Голый targetPath отбрасывал всё это. Фолбэк — цель напрямую.
        std::wstring primary = si.lnkPath.empty() ? si.targetPath : si.lnkPath;
        std::wstring fallback = si.lnkPath.empty() ? L"" : si.targetPath;
        if (!primary.empty())
        {
            HINSTANCE rc = ShellExecuteW(m_hwnd, L"open", primary.c_str(),
                nullptr, nullptr, SW_SHOW);
            if ((INT_PTR)rc <= 32 && !fallback.empty() && fallback != primary)
                rc = ShellExecuteW(m_hwnd, L"open", fallback.c_str(),
                    nullptr, nullptr, SW_SHOW);
            // Молчаливое закрытие попапа при ошибке запуска — баг: чиним
            // проверкой результата и сообщением, попап не сносим.
            if ((INT_PTR)rc <= 32)
            {
                WCHAR buf[512];
                swprintf_s(buf, Lang::Get(Str::W_LaunchFailMsg), primary.c_str(), (int)(INT_PTR)rc);
                MessageBoxW(m_hwnd, buf, Lang::Get(Str::W_LaunchFailCaption),
                    MB_OK | MB_ICONERROR | MB_TOPMOST);
                return;
            }
            // Учёт запуска — только успешные: неудачи портили статистику
            // и сортировку «Недавние сверху».
            if (GetGlobalManager()) GetGlobalManager()->RecordShortcutLaunch(m_groupId, idx);
        }
        Destroy();
    }
}

void PopupWindow::OnKeyDown(WPARAM vk)
{
    if (m_closing || !m_renderer) return;
    // Во время ресайза/перетаскивания/драга клавиши не трогают список.
    if (m_resizing || m_moving || m_dragging)
    {
        if (vk == VK_ESCAPE && m_resizing)
            EndResize();
        return;
    }
    int n = (int)m_group.shortcuts.size();
    switch (vk)
    {
    case VK_ESCAPE:
        CloseAnimated();
        break;
    case VK_UP:
    case VK_DOWN:
        if (n > 0)
        {
            int idx = m_hovered < 0 ? 0 : m_hovered;
            idx = (vk == VK_DOWN) ? (idx + 1) % n : (idx - 1 + n) % n;
            if (idx != m_hovered)
            {
                m_hovered = idx;
                // Ховер всегда видим (подкручиваем скролл при нужде).
                int top = idx * POPUP_ITEM_H;
                if (top < m_scrollY) m_scrollY = top;
                else
                {
                    int vis = VisListH();
                    if (top + POPUP_ITEM_H > m_scrollY + vis)
                        m_scrollY = top + POPUP_ITEM_H - vis;
                }
                ClampScroll();
                UpdateBitmap();
            }
        }
        break;
    case VK_HOME:
        if (n > 0 && m_hovered != 0)
        {
            m_hovered = 0;
            m_scrollY = 0;
            ClampScroll();
            UpdateBitmap();
        }
        break;
    case VK_END:
        if (n > 0 && m_hovered != n - 1)
        {
            m_hovered = n - 1;
            m_scrollY = INT_MAX;
            ClampScroll();
            UpdateBitmap();
        }
        break;
    case VK_RETURN:
    case VK_SPACE:
        if (m_hovered >= 0 && m_hovered < n)
            LaunchShortcut(m_hovered);
        break;
    }
}

void PopupWindow::OnRButtonUp(int x, int y)
{
    if (m_closing || m_resizing || m_moving || m_dragging || !m_renderer) return;
    int idx = m_renderer->HitTestPopup(m_group, x, y, m_scrollY);
    if (idx < 0 || idx >= (int)m_group.shortcuts.size()) return;
    POINT pt;
    GetCursorPos(&pt);
    ShowShellMenu(idx, pt);
}

void PopupWindow::OnContextMenu(POINT ptScreen, bool keyboard)
{
    if (m_closing || m_resizing || !m_renderer) return;
    if (keyboard)
    {
        // Клавиатура (Shift+F10 / Menu): пункт под ховером, позиция — его центр.
        int idx = m_hovered;
        if (idx < 0 || idx >= (int)m_group.shortcuts.size()) return;
        POINT pt = { 40, POPUP_HEADER_H + POPUP_PAD + idx * POPUP_ITEM_H -
            m_scrollY + POPUP_ITEM_H / 2 };
        ClientToScreen(m_hwnd, &pt);
        ShowShellMenu(idx, pt);
        return;
    }
    POINT pt = ptScreen;
    ScreenToClient(m_hwnd, &pt);
    int idx = m_renderer->HitTestPopup(m_group, pt.x, pt.y, m_scrollY);
    if (idx < 0 || idx >= (int)m_group.shortcuts.size()) return;
    ShowShellMenu(idx, ptScreen);
}

// Запасной вариант, если shell не отдал IContextMenu: минимум через запуск.
// ---------------------------------------------------------------------------
// Новое контекстное меню как в Windows 11 (картинка 1): командная панель сверху,
// иконки shell, хоткеи справа, подменю, системная тема. Данные — настоящие
// глаголы shell для файла (весь список, дубли не нужны).
// ---------------------------------------------------------------------------

static const int SHELL_CMD_CUT = 10;
static const int SHELL_CMD_COPY = 11;
static const int SHELL_CMD_RENAME = 12;
static const int SHELL_CMD_DELETE = 13;
static const int SHELL_VERB_BASE = 100;

// Глифы Segoe MDL2 Assets для командной панели.
static const wchar_t GLYPH_CUT = L'\uE8C6';
static const wchar_t GLYPH_COPY = L'\uE8C8';
static const wchar_t GLYPH_RENAME = L'\uE70F';
static const wchar_t GLYPH_DELETE = L'\uE74D';
// Крестик «убрать приложение» (тот же шрифт, что крестик окон).
static const wchar_t GLYPH_UNINSTALL = L'\uE711';

struct ShellNode {
    bool separator = false;
    std::wstring text;     // без хоткея и &
    std::wstring shortcut; // часть после \t
    HBITMAP icon = nullptr; // НЕ владеем здесь (владеет ShellMenuData::icons)
    UINT offset = 0;       // idCmd для Invoke (id - idMin)
    std::string verb;      // канонический глагол (может быть пустым)
    std::vector<ShellNode> children; // подменю
};

struct ShellMenuData {
    IContextMenu* pcm = nullptr;
    std::vector<ShellNode> nodes;
    std::vector<HBITMAP> icons; // наши копии — удалить после Show
    std::vector<UINT> verbOffsets; // menu id (>=100) -> shell offset
    std::vector<std::unique_ptr<ModernMenu>> subs; // подменю — живут весь Show
};

static std::wstring StripAccelerator(std::wstring s)
{
    std::wstring r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == L'&')
        {
            if (i + 1 < s.size() && s[i + 1] == L'&') { r += L'&'; i++; }
            continue;
        }
        r += s[i];
    }
    // Хвостовые пробелы (остатки выравнивания)
    while (!r.empty() && (r.back() == L' ' || r.back() == L'\t'))
        r.pop_back();
    return r;
}

static HBITMAP CopyMenuBitmap(HBITMAP hbm)
{
    if (!hbm || hbm == HBMMENU_CALLBACK || hbm == HBMMENU_SYSTEM) return nullptr;
    if ((UINT_PTR)hbm < 16) return nullptr; // HBMMENU_MBAR_* и прочее системное
    return (HBITMAP)CopyImage(hbm, IMAGE_BITMAP, 0, 0, LR_COPYRETURNORG);
}

static void CollectShellNodes(IContextMenu* pcm, HMENU hMenu, UINT idMin, UINT idMax,
    std::vector<ShellNode>& out, std::vector<HBITMAP>& icons, int depth)
{
    int n = GetMenuItemCount(hMenu);
    for (int i = 0; i < n; i++)
    {
        MENUITEMINFOW mii = { sizeof(mii) };
        WCHAR buf[256] = {};
        mii.fMask = MIIM_ID | MIIM_FTYPE | MIIM_SUBMENU | MIIM_BITMAP | MIIM_STRING;
        mii.dwTypeData = buf;
        mii.cch = _countof(buf);
        if (!GetMenuItemInfoW(hMenu, i, TRUE, &mii)) continue;
        if (mii.fType & MFT_SEPARATOR)
        {
            // Схлопываем дублирующиеся разделители.
            if (!out.empty() && !out.back().separator)
            {
                ShellNode s;
                s.separator = true;
                out.push_back(std::move(s));
            }
            continue;
        }
        std::wstring text = StripAccelerator(buf);
        std::wstring shortcut;
        size_t tab = text.find(L'\t');
        if (tab != std::wstring::npos)
        {
            shortcut = text.substr(tab + 1);
            text = StripAccelerator(text.substr(0, tab));
        }
        if (text.empty()) continue;

        ShellNode node;
        node.text = text;
        node.shortcut = shortcut;
        if (mii.hbmpItem)
        {
            node.icon = CopyMenuBitmap(mii.hbmpItem);
            if (node.icon) icons.push_back(node.icon);
        }
        if (mii.hSubMenu && depth < 3)
        {
            CollectShellNodes(pcm, mii.hSubMenu, idMin, idMax,
                node.children, icons, depth + 1);
            while (!node.children.empty() && node.children.back().separator)
                node.children.pop_back();
            if (node.children.empty()) continue; // пустое подменю — пропуск
        }
        else if (mii.wID >= idMin && mii.wID <= idMax)
        {
            node.offset = mii.wID - idMin;
            CHAR va[128] = {};
            if (SUCCEEDED(pcm->GetCommandString((UINT_PTR)node.offset, GCS_VERBA,
                    nullptr, va, sizeof(va))) && va[0])
                node.verb = va;
        }
        else
        {
            continue; // системный id без подменю — пропуск
        }
        out.push_back(std::move(node));
    }
    while (!out.empty() && out.back().separator)
        out.pop_back();
}

static bool HasShellVerb(const std::vector<ShellNode>& nodes, const char* verb)
{
    for (const auto& n : nodes)
    {
        if (!n.separator && n.children.empty() && n.verb == verb) return true;
        if (!n.children.empty() && HasShellVerb(n.children, verb)) return true;
    }
    return false;
}

// Глаголы командной панели — в подменю их не дублируем.
static bool IsCmdRowVerb(const std::string& v)
{
    return v == "cut" || v == "copy" || v == "delete" || v == "rename";
}

static void BuildShellItems(ModernMenu& menu, const std::vector<ShellNode>& nodes,
    ShellMenuData& data)
{
    bool lastSep = true; // ведущие разделители давим
    for (const auto& n : nodes)
    {
        if (n.separator)
        {
            if (!lastSep)
            {
                ModernMenuItem sep;
                sep.separator = true;
                menu.items.push_back(sep);
                lastSep = true;
            }
            continue;
        }
        lastSep = false;
        if (!n.children.empty())
        {
            data.subs.push_back(std::make_unique<ModernMenu>(menu.m_renderer));
            ModernMenu* sub = data.subs.back().get();
            BuildShellItems(*sub, n.children, data);
            ModernMenuItem mi;
            mi.text = n.text;
            mi.icon = n.icon;
            mi.shortcut = n.shortcut;
            mi.submenu = sub;
            menu.items.push_back(mi);
            continue;
        }
        if (IsCmdRowVerb(n.verb)) continue; // уже в командной панели сверху
        ModernMenuItem mi;
        mi.text = n.text;
        mi.id = SHELL_VERB_BASE + (int)data.verbOffsets.size();
        mi.icon = n.icon;
        mi.shortcut = n.shortcut;
        menu.items.push_back(mi);
        data.verbOffsets.push_back(n.offset);
    }
    while (!menu.items.empty() && menu.items.back().separator)
        menu.items.pop_back();
}

// Маленькая 16px иконка самого файла для пункта «Открыть»
// (как в новом меню Windows 11). Возвращает HBITMAP с премультипликацией,
// удаляет вызыватель.
static HBITMAP FileIconBitmap16(const std::wstring& path)
{
    if (path.empty()) return nullptr;
    SHFILEINFOW sfi = {};
    if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
            SHGFI_ICON | SHGFI_SMALLICON) || !sfi.hIcon)
        return nullptr;

    HBITMAP hbmp = nullptr;
    HDC hdc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(hdc);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = 16;
    bmi.bmiHeader.biHeight = -16;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    hbmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbmp && bits)
    {
        memset(bits, 0, 16 * 16 * 4);
        HGDIOBJ old = SelectObject(mem, hbmp);
        DrawIconEx(mem, 0, 0, sfi.hIcon, 16, 16, 0, nullptr, DI_NORMAL);
        SelectObject(mem, old);
        // Премультипликация для layered-меню (иначе ореолы на тёмной теме).
        BYTE* px = (BYTE*)bits;
        for (int i = 0; i < 16 * 16; i++, px += 4)
        {
            BYTE a = px[3];
            px[0] = (BYTE)(px[0] * a / 255);
            px[1] = (BYTE)(px[1] * a / 255);
            px[2] = (BYTE)(px[2] * a / 255);
        }
    }
    else if (hbmp)
    {
        DeleteObject(hbmp);
        hbmp = nullptr;
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, hdc);
    DestroyIcon(sfi.hIcon);
    return hbmp;
}

static void BuildShellMenu(ModernMenu& menu, ShellMenuData& data, const ShortcutInfo& si,
    bool showUninstall)
{
    // Командная панель как в новом меню Windows 11 (подписи усечены как в оригинале).
    auto addCmd = [&](int id, wchar_t glyph, const wchar_t* label, const char* verb, bool alwaysOn) {
        ModernMenuItem c;
        c.id = id;
        c.glyph = glyph;
        c.text = label;
        c.disabled = !(alwaysOn || (verb && HasShellVerb(data.nodes, verb)));
        menu.commands.push_back(c);
    };
    addCmd(SHELL_CMD_CUT, GLYPH_CUT, Lang::Get(Str::W_CmdCut), "cut", false);
    addCmd(SHELL_CMD_COPY, GLYPH_COPY, Lang::Get(Str::W_CmdCopy), "copy", false);
    addCmd(SHELL_CMD_RENAME, GLYPH_RENAME, Lang::Get(Str::W_CmdRename), "rename", true);
    addCmd(SHELL_CMD_DELETE, GLYPH_DELETE, Lang::Get(Str::W_CmdDelete), "delete", false);
    // «Удалить приложение» — пятой кнопкой панели, рядом с «Удалить».
    // Крестик вместо корзины, чтобы не сливалось с соседней кнопкой.
    if (showUninstall)
        addCmd(SHELL_CMD_UNINSTALL_APP, GLYPH_UNINSTALL,
            Lang::Get(Str::W_UninstallApp), "", true);

    // Группировка как в новом меню: сначала известные глаголы в порядке shell,
    // затем сторонние (ChatGPT, Code...), разделители пересобираем.
    std::vector<ShellNode> ordered;
    auto isKnown = [](const ShellNode& n) {
        if (!n.children.empty()) return true; // подменю остаются на месте
        static const char* known[] = { "open", "runas", "edit", "print",
            "copyaspath", "properties", "pintohome", "pintohomefile", "link",
            "share", "Windows.ModernShare", "PreviousVersions",
            "opencontaining", nullptr };
        for (int i = 0; known[i]; i++)
            if (n.verb == known[i]) return true;
        return false;
    };
    for (const auto& n : data.nodes)
        if (!n.separator && isKnown(n)) ordered.push_back(n);
    bool hasUnknown = false;
    for (const auto& n : data.nodes)
        if (!n.separator && !isKnown(n)) { hasUnknown = true; break; }
    if (hasUnknown && !ordered.empty())
    {
        ShellNode sep;
        sep.separator = true;
        ordered.push_back(std::move(sep));
    }
    for (const auto& n : data.nodes)
        if (!n.separator && !isKnown(n)) ordered.push_back(n);
    // NOTE: иконки ShellNode копируются по хэндлу — владение у data.icons,
    // поверхностное копирование нод безопасно.

    // Иконка самого файла для «Открыть» — как в новом меню Windows 11.
    HBITMAP openIcon = FileIconBitmap16(si.lnkPath);
    if (openIcon) data.icons.push_back(openIcon);
    for (auto& n : ordered)
        if (n.verb == "open" && n.children.empty() && !n.icon)
            n.icon = openIcon;

    BuildShellItems(menu, ordered, data);
}

static bool InvokeShellOffset(IContextMenu* pcm, HWND hwnd, UINT offset)
{
    if (!pcm) return false;
    CMINVOKECOMMANDINFOEX ici = {};
    ici.cbSize = sizeof(ici);
    ici.fMask = CMIC_MASK_UNICODE;
    ici.hwnd = hwnd;
    ici.lpVerb = MAKEINTRESOURCEA(offset);
    ici.lpVerbW = MAKEINTRESOURCEW(offset);
    ici.nShow = SW_SHOWNORMAL;
    return SUCCEEDED(pcm->InvokeCommand((CMINVOKECOMMANDINFO*)&ici));
}

static bool InvokeShellName(IContextMenu* pcm, HWND hwnd, const char* verbA)
{
    if (!pcm || !verbA) return false;
    WCHAR verbW[64] = {};
    // Без проверки возврата здесь оказывался пустой verb (тихий no-op).
    if (MultiByteToWideChar(CP_ACP, 0, verbA, -1, verbW, _countof(verbW)) == 0)
        return false;
    CMINVOKECOMMANDINFOEX ici = {};
    ici.cbSize = sizeof(ici);
    ici.fMask = CMIC_MASK_UNICODE;
    ici.hwnd = hwnd;
    ici.lpVerb = verbA;
    ici.lpVerbW = verbW;
    ici.nShow = SW_SHOWNORMAL;
    return SUCCEEDED(pcm->InvokeCommand((CMINVOKECOMMANDINFO*)&ici));
}

static void FallbackShellMenu(WidgetRenderer* renderer, const ShortcutInfo& si, POINT ptScreen)
{
    if (!renderer) return;
    ModernMenu menu(renderer);
    std::wstring title = si.name.empty() ? Lang::Get(Str::W_File) : si.name;
    menu.items.push_back({ title, 0, false, false, false, true });
    menu.items.push_back({ L"", 0, false, false, true });
    menu.items.push_back({ Lang::Get(Str::W_Open), 1 });
    menu.items.push_back({ Lang::Get(Str::W_OpenContaining), 2 });
    int cmd = menu.Show(ptScreen.x, ptScreen.y);
    if (cmd == 1)
    {
        // Как и в LaunchShortcut: сначала сам .lnk, потом цель, с проверкой.
        std::wstring primary = si.lnkPath.empty() ? si.targetPath : si.lnkPath;
        std::wstring fallback = si.lnkPath.empty() ? L"" : si.targetPath;
        if (!primary.empty())
        {
            HINSTANCE rc = ShellExecuteW(nullptr, L"open", primary.c_str(),
                nullptr, nullptr, SW_SHOW);
            if ((INT_PTR)rc <= 32 && !fallback.empty() && fallback != primary)
                rc = ShellExecuteW(nullptr, L"open", fallback.c_str(),
                    nullptr, nullptr, SW_SHOW);
            if ((INT_PTR)rc <= 32)
            {
                WCHAR buf[512];
                swprintf_s(buf, Lang::Get(Str::W_LaunchFailMsg), primary.c_str(), (int)(INT_PTR)rc);
                MessageBoxW(nullptr, buf, Lang::Get(Str::W_LaunchFailCaption),
                    MB_OK | MB_ICONERROR | MB_TOPMOST);
            }
        }
    }
    else if (cmd == 2 && !si.lnkPath.empty())
    {
        std::wstring par = L"/select,\"" + si.lnkPath + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", par.c_str(), nullptr, SW_SHOWNORMAL);
    }
}


void PopupWindow::ShowShellMenu(int index, POINT ptScreen)
{
    if (index < 0 || index >= (int)m_group.shortcuts.size()) return;
    if (!m_hwnd) return;
    ClearUninstallCache();
    const ShortcutInfo si = m_group.shortcuts[index]; // копия: группа ниже может измениться
    if (si.lnkPath.empty() ||
        GetFileAttributesW(si.lnkPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(si.lnkPath.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
    {
        FallbackShellMenu(m_renderer, si, ptScreen);
        return;
    }

    IShellFolder* psf = nullptr;
    PCUITEMID_CHILD pidlChild = nullptr;
    HRESULT hr = SHBindToParent(pidl, IID_PPV_ARGS(&psf), &pidlChild);
    if (FAILED(hr) || !psf || !pidlChild)
    {
        if (psf) psf->Release();
        CoTaskMemFree(pidl);
        FallbackShellMenu(m_renderer, si, ptScreen);
        return;
    }

    IContextMenu* pcm = nullptr;
    hr = psf->GetUIObjectOf(m_hwnd, 1, &pidlChild, IID_IContextMenu, nullptr, (void**)&pcm);
    if (FAILED(hr) || !pcm)
    {
        psf->Release();
        CoTaskMemFree(pidl);
        FallbackShellMenu(m_renderer, si, ptScreen);
        return;
    }

    // Новое меню как в Windows 11: командная панель сверху, иконки, хоткеи справа,
    // подменю, тёмная тема. Данные — настоящие глаголы shell для этого файла.
    ShellMenuData data;
    data.pcm = pcm;
    {
        HMENU hTmp = CreatePopupMenu();
        if (hTmp && SUCCEEDED(pcm->QueryContextMenu(hTmp, 0, 1, 0x7FFF, CMF_NORMAL)))
            CollectShellNodes(pcm, hTmp, 1, 0x7FFF, data.nodes, data.icons, 0);
        if (hTmp) DestroyMenu(hTmp);
    }
    ModernMenu menu(m_renderer);

    // «Удалить приложение» — нужно знать до построения панели.
    UninstallInfo uninst;
    bool hasUninst = FindUninstallerForTarget(
        si.targetPath.empty() ? si.lnkPath : si.targetPath, uninst);
    BuildShellMenu(menu, data, si, hasUninst);

    int cmd = menu.Show(ptScreen.x, ptScreen.y);
    // Копии иконок жили только на время Show.
    for (HBITMAP h : data.icons)
        if (h) DeleteObject(h);
    data.icons.clear();
    // Результат shell-команд проверяем: раньше bool игнорировался и провал
    // был тихим (пользователь не понимал, что cut/copy/delete не сработали).
    auto reportVerbFail = [&](const wchar_t* verb) {
        WCHAR buf[256];
        swprintf_s(buf, Lang::Get(Str::W_ShellVerbFail), verb);
        MessageBoxW(m_hwnd, buf, Lang::Get(Str::W_ErrorCaption),
            MB_OK | MB_ICONERROR | MB_TOPMOST);
    };
    if (cmd >= SHELL_VERB_BASE)
    {
        size_t k = (size_t)(cmd - SHELL_VERB_BASE);
        if (k < data.verbOffsets.size())
        {
            if (!InvokeShellOffset(pcm, m_hwnd, data.verbOffsets[k]))
                reportVerbFail(L"#offset");
        }
    }
    else switch (cmd)
    {
    case SHELL_CMD_CUT:
        if (!InvokeShellName(pcm, m_hwnd, "cut"))
            reportVerbFail(L"cut");
        break;
    case SHELL_CMD_COPY:
        if (!InvokeShellName(pcm, m_hwnd, "copy"))
            reportVerbFail(L"copy");
        break;
    case SHELL_CMD_DELETE:
        if (!InvokeShellName(pcm, m_hwnd, "delete"))
            reportVerbFail(L"delete");
        break;
    case SHELL_CMD_UNINSTALL_APP:
        if (hasUninst)
            DoUninstallApp(m_hwnd, si, uninst, true);
        break;
    case SHELL_CMD_RENAME:
    {
        WidgetManager* mgr0 = GetGlobalManager();
        if (mgr0)
        {
            PCWSTR fn = PathFindFileNameW(si.lnkPath.c_str());
            std::wstring cur = fn ? fn : L"";
            std::wstring nn;
            if (!cur.empty() &&
                mgr0->ShowFileRenameDialog(m_hwnd, cur, nn) && nn != cur)
            {
                std::wstring dir = si.lnkPath.substr(0,
                    si.lnkPath.size() - cur.size());
                if (!MoveFileW(si.lnkPath.c_str(), (dir + nn).c_str()))
                    MessageBoxW(m_hwnd, Lang::Get(Str::W_RenameError),
                        Lang::Get(Str::W_ErrorCaption), MB_OK | MB_ICONERROR | MB_TOPMOST);
            }
        }
        break;
    }
    }
    pcm->Release();

    // Из меню файл могли удалить/переименовать — синхронизируем группу.
    // NOTE: группу могли снести целиком — this мёртв, выходим сразу.
    // Освобождение ДО return: ранний выход пропускал Release/CoTaskMemFree.
    WidgetManager* mgr = GetGlobalManager();
    std::wstring gid = m_groupId;
    if (mgr && mgr->RefreshGroupFromDisk(gid))
    {
        psf->Release();
        CoTaskMemFree(pidl);
        return;
    }
    psf->Release();
    CoTaskMemFree(pidl);
}

void PopupWindow::OnMouseMove(int x, int y)
{
    if (m_closing) return;
    if (!m_renderer) return;

    if (m_resizing)
    {
        UpdateResize();
        return;
    }

    if (m_moving)
    {
        // Живое перемещение без перерисовки: слой двигает DWM,
        // фон обновится один раз при отпускании кнопки
        POINT pt;
        GetCursorPos(&pt);
        SetWindowPos(m_hwnd, nullptr, pt.x - m_moveDX, pt.y - m_moveDY,
            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        return;
    }

    // Курсор ресайза над границами, move — над шапкой, как у обычного окна
    {
        static HCURSOR s_cursors[10] = {};
        ResizeMode rm = HitBorder(x, y);
        LPCWSTR id = IDC_ARROW;
        int ci = 0;
        if (rm != ResizeMode::None)
        {
            id = CursorForResizeMode(rm);
            ci = (int)rm; // 1..8
        }
        else if (y < POPUP_HEADER_H)
        {
            id = IDC_SIZEALL;
            ci = 9;
        }
        if (!s_cursors[ci])
            s_cursors[ci] = LoadCursorW(nullptr, id);
        SetCursor(s_cursors[ci]);
    }

    if (m_dragging && !m_moved)
    {
        POINT pt;
        GetCursorPos(&pt);
        int dx = abs(pt.x - m_pressAbsX);
        int dy = abs(pt.y - m_pressAbsY);
        if (dx > 3 || dy > 3)
        {
            m_moved = true;
            OutputDebugStringW(L"Popup drag started, calling DoDragDrop\n");

            if (m_dragIndex >= 0 && m_dragIndex < (int)m_group.shortcuts.size())
            {
                const auto& si = m_group.shortcuts[m_dragIndex];
                std::wstring lnkPath = si.lnkPath;
                std::wstring name = si.name.empty() ? m_group.name : si.name;
                std::wstring groupName = m_group.name;

                ReleaseCapture();
                m_dragging = false;

                LnkJunkDataObject* pDataObj = new LnkJunkDataObject(lnkPath, m_groupId, m_dragIndex);
                LnkJunkDropSource* pDropSrc = new LnkJunkDropSource();
                DWORD dwEffect = DROPEFFECT_COPY;

                popupDragging = true;
                popupDragGroupId = m_groupId;
                popupDragIndex = m_dragIndex;
                popupDragGroupName = groupName;
                m_dropInside = false;
                m_dropIndex = -1;

                HRESULT hr = DoDragDrop(pDataObj, pDropSrc, DROPEFFECT_COPY, &dwEffect);
                pDataObj->Release();
                pDropSrc->Release();

                WCHAR dbuf[256];
                wsprintfW(dbuf, L"DoDragDrop hr=0x%08X effect=%d\n", (unsigned)hr, (int)dwEffect);
                OutputDebugStringW(dbuf);

                if (hr == DRAGDROP_S_DROP && popupDragGroupId == m_groupId && popupDragIndex == m_dragIndex)
                {
                    WidgetManager* mgr = GetGlobalManager();
                    if (mgr)
                    {
                        if (m_dropInside)
                        {
                            // Дроп внутри попапа — меняем порядок ярлыков
                            OutputDebugStringW(L"DoDragDrop: reorder inside popup\n");
                            int to = (m_dropIndex >= 0) ? m_dropIndex : INT_MAX;
                            mgr->MoveShortcutInGroup(m_groupId, m_dragIndex, to);

                            const auto& groups = mgr->GetGroups();
                            for (const auto& g : groups)
                            {
                                if (g.id == m_groupId)
                                {
                                    Update(g);
                                    break;
                                }
                            }
                        }
                        else
                        {
                            OutputDebugStringW(L"DoDragDrop SUCCEEDED, removing shortcut from group\n");
                            mgr->RemoveShortcutFromGroup(m_groupId, m_dragIndex);

                            const auto& groups = mgr->GetGroups();
                            for (const auto& g : groups)
                            {
                                if (g.id == m_groupId)
                                {
                                    Update(g);
                                    break;
                                }
                            }
                        }
                    }
                }

                m_dropInside = false;
                m_dropIndex = -1;
                popupDragging = false;
                popupDragGroupId.clear();
                popupDragIndex = -1;
                popupDragGroupName.clear();
                return;
            }
        }
    }

    int idx = m_renderer->HitTestPopup(m_group, x, y, m_scrollY);
    if (idx != m_hovered)
    {
        m_hovered = idx;
        UpdateBitmap();
    }
}
