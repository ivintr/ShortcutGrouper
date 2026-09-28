#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <map>

class WidgetManager;

// Глобальный поиск по ярлыкам всех групп: строка ввода + живой список.
// Стиль — тёмное/светлое стекло под систему, иконки берёт shell.
class SearchWindow {
public:
    // Показать (создать при первом вызове), обновить список, взять фокус.
    void Show(WidgetManager* manager);
    void Hide();
    bool IsVisible() const { return m_hwnd && IsWindowVisible(m_hwnd); }
    void Destroy();

private:
    struct Result {
        std::wstring groupId;
        std::wstring groupName;
        std::wstring name;
        std::wstring lnkPath;
        std::wstring targetPath;
        int rank = 0; // 0 = префикс имени, 1 = подстрока, 2 = совпала группа
    };

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void Rebuild();      // собрать все ярлыки менеджера + применить фильтр
    void ApplyFilter();  // отфильтровать m_all по m_query
    void Layout();       // высота окна под число строк
    void Paint();        // GDI-рендер в layered-слой
    void Launch(int idx);
    int RowAt(int y) const;
    void SetHovered(int idx);
    void EnsureVisible(int idx);
    void OnChar(wchar_t ch);
    void OnKeyDown(WPARAM vk);
    UINT Dpi() const;
    int Px(int v) const { return MulDiv(v, (int)Dpi(), 96); }
    static std::wstring Lower(std::wstring s);

    HICON FetchIcon(const std::wstring& lnkPath);
    void ClearIcons();

    HWND m_hwnd = nullptr;
    HFONT m_hFont = nullptr;      // 15px имена + ввод
    HFONT m_hSubFont = nullptr;   // 12px группы
    WidgetManager* m_manager = nullptr;
    std::wstring m_query;
    int m_caretPos = 0;           // позиция каретки в m_query
    bool m_caretOn = true;        // фаза мигания
    std::vector<Result> m_all;
    std::vector<Result> m_hits;
    std::map<std::wstring, HICON> m_icons;
    int m_hovered = -1;
    int m_scrollY = 0;
    bool m_dark = false;
};
