#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include "WidgetTypes.h"
#include "Renderer.h"
#include "UninstallHelper.h"

class WidgetManager;

// Раскрывающееся окно группы (было в DesktopWidget.h).
class PopupWindow {
public:
    enum class ResizeMode {
        None, Left, Right, Top, Bottom,
        TopLeft, TopRight, BottomLeft, BottomRight
    };

    bool Create(HWND hParent, const GroupData& group, WidgetRenderer* renderer);
    void Destroy();
    void Update(const GroupData& group);
    void ShowNear(int cx, int cy);
    bool IsVisible() const { return m_visible; }
    HWND GetHwnd() const { return m_hwnd; }
    const std::wstring& GetGroupId() const { return m_groupId; }

    static void CloseAll();
    void CloseAnimated();
    void CancelClose();
    bool IsClosing() const { return m_closing; }
    void OnTimer();
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void OnPaint();
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnKeyDown(WPARAM vk);
    void LaunchShortcut(int idx);
    void OnRButtonUp(int x, int y);
    void OnContextMenu(POINT ptScreen, bool keyboard);
    void ShowShellMenu(int index, POINT ptScreen);
    void OnMouseMove(int x, int y);
    void OnMouseWheel(int delta);
    void UpdateBitmap();
    void UpdateLayered();

    int FullListH() const;
    int MaxScroll() const;
    void ClampScroll();
    int VisibleH() const;
    int VisListH() const;
    ResizeMode HitBorder(int x, int y) const;
    void BeginResize(ResizeMode mode);
    void UpdateResize();
    void EndResize();

    HWND              m_hwnd = nullptr;
    std::wstring      m_groupId;
    GroupData         m_group;
    WidgetRenderer*   m_renderer = nullptr;
    HBITMAP           m_hBitmap  = nullptr;
    IDropTarget*      m_dropTarget = nullptr;
    int               m_dropIndex = -1;
    bool              m_dropInside = false;
    bool              m_visible  = false;
    int               m_hovered  = -1;
    int               m_scrollY  = 0;   // смещение списка в px
    int               m_maxListH = -1;  // макс. видимая высота списка (-1 = весь)
    int               m_popupW   = POPUP_W; // текущая ширина окна
    int               m_customListH = -1;   // ручная высота списка (-1 = авто)
    bool              m_resizing = false;
    ResizeMode        m_resizeMode = ResizeMode::None;
    bool              m_moving = false;  // перетаскивание окна за шапку
    int               m_moveDX = 0;      // сдвиг курсора от левого края окна
    int               m_moveDY = 0;      // сдвиг курсора от верхнего края окна
    bool              m_closing = false; // идёт fade-out закрытия
    int               m_animAlpha = 255;
    int               m_animX = 0;
    int               m_animY = 0;
    int               m_animTargetY = 0;
    UINT_PTR          m_animTimer = 0;
    POINT             m_resizeStartPt = {};
    int               m_resizeStartX = 0;
    int               m_resizeStartY = 0;
    int               m_resizeStartW = 0;
    int               m_resizeStartListH = 0;
    bool              m_dragging = false;
    int               m_dragIndex = -1;
    int               m_pressX   = 0;
    int               m_pressY   = 0;
    int               m_pressAbsX = 0;
    int               m_pressAbsY = 0;
    bool              m_moved    = false;
};

// Кнопка панели shell-меню + bulk-меню (реализация в PopupWindow.cpp,
// нужна и ShowBulkMenu из DesktopWidget.cpp).
constexpr int SHELL_CMD_UNINSTALL_APP = 14;
