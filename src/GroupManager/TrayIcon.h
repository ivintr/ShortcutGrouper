#pragma once
#include <Windows.h>

#define IDM_SETTINGS  1001
#define IDM_EXIT      1002
#define IDM_REFRESH   1003
#define IDM_SEARCH    1004
#define IDM_TOGGLE_VIS 1005

class WidgetRenderer;

class TrayIcon
{
public:
    void Create(HINSTANCE hInst, HWND hOwner, UINT msgId);
    void Remove();
    // Повторный NIM_ADD после рестарта Explorer (см. TaskbarCreated).
    void ReAdd();
    // Всплывающая подсказка трея (уведомления вроде автоудаления группы).
    void ShowBalloon(const wchar_t* title, const wchar_t* text);
    void ShowMenu(HWND hWnd, bool widgetsHidden);
    // Новое меню в стиле Windows 11 (Fluent). Возвращает id, WM_COMMAND
    // дополнительно постится владельцу для совместимости.
    int ShowModernMenu(HWND hWnd, WidgetRenderer* renderer, bool widgetsHidden);

private:
    void* m_nid = nullptr;
    UINT m_msgId = 0;
};
