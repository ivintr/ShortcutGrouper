#pragma once
#include <Windows.h>
#include <string>
#include <vector>

class WidgetRenderer;

// Один пункт меню в стиле Windows 11 (Fluent).
// id == 0        -> не выбирается (заголовок / шапка подменю)
// separator=true -> разделитель
// submenu!=null  -> пункт открывает подменю
// swatch>=0      -> цветной квадрат 0xRRGGBB (пункт "Цвет")
// title=true     -> шапка меню: полужирный некликабельный заголовок
// icon!=null     -> HBITMAP иконки слева (владением управляет вызыватель)
// shortcut       -> подсказка хоткея справа ("Ctrl+Shift+C")
// glyph!=0       -> глиф Segoe MDL2 (командная панель нового меню)
struct ModernMenuItem {
    std::wstring text;
    int id = 0; // 0 = not selectable (title, submenu header)
    bool checked = false;
    bool disabled = false;
    bool separator = false;
    bool title = false;
    int swatch = -1; // 0xRRGGBB or -1
    HBITMAP icon = nullptr;
    std::wstring shortcut;
    wchar_t glyph = 0;
    class ModernMenu* submenu = nullptr;
};

// Контекстное меню в стиле Windows 11: layered-окно с frosted-стеклом,
// скругление 8px, тень, светлая/тёмная тема из персонализации,
// шрифт Segoe UI Variable, hover-пилюля, подменю, клавиатура.
// Корневое меню запускает модальный цикл в Show() и возвращает id (0 = закрыто).
class ModernMenu {
public:
    explicit ModernMenu(WidgetRenderer* renderer);
    ~ModernMenu() = default;

    std::vector<ModernMenuItem> items;
    // Командная панель нового меню Windows 11 (верхний ряд кнопок с иконками).
    // Пусто = обычное меню без панели.
    std::vector<ModernMenuItem> commands;

    int Show(int x, int y);
    void CloseTree(int result);

    bool IsSelectable(int index) const;
    bool IsCommandSelectable(int index) const;
    int HitTest(int y) const;
    int HitTestCmd(int x, int y) const;
    int ItemsTop() const;
    // Активация по точке отпускания через всё дерево (press-drag-release
    // как в настоящих меню). Возвращает true, если что-то сработало.
    bool ActivateAt(POINT ptScreen);

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
    // Та же логика с явной точкой отпускания (тестируемость + press-drag).
    void OnLButtonUpAt(int x, int y, POINT ptScreen);
    void OnKey(int vk);
    void Render();

    WidgetRenderer* m_renderer = nullptr;
    HWND m_hwnd = nullptr;
    HBITMAP m_hBitmap = nullptr;
    int m_menuW = 0;
    int m_menuH = 0;
    bool m_dark = false;
    int m_hovered = -1;
    int m_pressed = -1;
    int m_cmdHovered = -1;
    int m_cmdPressed = -1;
    ModernMenu* m_parent = nullptr;
    ModernMenu* m_child = nullptr;
    int m_childFor = -1;
    int m_result = 0;
    bool m_done = false;
    int m_animAlpha = 255; // fade-in при появлении

    static ModernMenu* s_modalRoot;
    static HHOOK s_mouseHook;
    static HHOOK s_kbdHook;
    static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KbdProc(int nCode, WPARAM wParam, LPARAM lParam);
    static void InstallHooks();
    static void UninstallHooks();
    static bool ReadDarkMode();

private:
    ModernMenu* Deepest();
    bool PointInTree(POINT pt) const;
    void CloseChild();
    void OpenChildFor(int index);
    void CreateAndShow(int x, int y, int w, int h);
    void DestroySelf();
    void DestroyTreeWindows();
    void MoveHover(int dir);
    void ActivateHovered();
    void TrackLeave();
    void ApplyModernStyle();
    static void PositionIntoWorkArea(int* px, int* py, int w, int h);
};

// Reads HKCU .../Personalize/AppsUseLightTheme (true = dark mode active).
bool ModernMenuIsDarkMode();
