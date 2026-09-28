#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include "WidgetTypes.h"
#include "Renderer.h"

struct IContextMenu;

class WidgetManager;

class DesktopWidget {
public:
    bool Create(HWND hParent, const GroupData& group, WidgetRenderer* renderer,
        WidgetManager* manager);
    void Destroy();
    void Update(const GroupData& group);
    void ShowAt(int x, int y);
    HWND GetHwnd() const { return m_hwnd; }
    const std::wstring& GetGroupId() const { return m_groupId; }

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void OnPaint();
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnRButtonUp(int x, int y);
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnTimer();
    void RefreshGlow();
    void TogglePopup();
    void ShowContextMenu();
    void ShowBulkMenu(const std::vector<std::wstring>& ids);
    void SnapToGrid();
    // Финализация дропа: снап/нудж + сохранение + слой стола + перерисовка.
    void FinishDropPosition();
    int WidgetH() const { return WidgetHeight(m_group); }
    void UpdateBitmap();
    void UpdateLayered();

    HWND              m_hwnd = nullptr;
    std::wstring      m_groupId;
    GroupData         m_group;
    WidgetRenderer*   m_renderer = nullptr;
    WidgetManager*    m_manager  = nullptr;
    HBITMAP           m_hBitmap  = nullptr;
    IDropTarget*      m_dropTarget = nullptr;
    bool              m_oleCounted = false; // этот виджет считал OleInitialize
    bool              m_dragging = false;
    int               m_pressX   = 0;
    int               m_pressY   = 0;
    int               m_pressAbsX = 0;
    int               m_pressAbsY = 0;
    bool              m_moved    = false;
    bool              m_ctrlDown = false; // Ctrl был зажат при нажатии
    bool              m_pressedOnSelected = false; // нажали на уже выделенный
    // Пиры группового перетаскивания: виджет + его точка старта.
    struct DragPeer { DesktopWidget* w = nullptr; int startX = 0; int startY = 0; };
    std::vector<DragPeer> m_dragPeers;
    int               m_glow = 0;       // свечение при наведении 0..255
    int               m_glowTarget = 0;
    UINT_PTR          m_glowTimer = 0;
};

// Классы окон виджетов/попапов (определены в DesktopWidget.cpp).
// Нужны в т.ч. Marquee.cpp для хит-тестов.
extern const wchar_t* const WIDGET_CLASS;
extern const wchar_t* const POPUP_CLASS;

class PopupWindow;

// Слой рабочего стола: обновление layered-окна из битмапа (общий для
// виджета и попапа, реализация в DesktopWidget.cpp).
void DoUpdateLayered(HWND hwnd, HBITMAP hBitmap, int w, int h, BYTE alpha = 255);
// Диагностика в debug.log (реализация там же).
void LogTiming(const wchar_t* fmt, ULONGLONG v);
// Глобалы слоя (задаются из main): читаем через геттеры.
WidgetManager* GetGlobalManager();


