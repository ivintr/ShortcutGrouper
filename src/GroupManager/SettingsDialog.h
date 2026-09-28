#pragma once
#include <Windows.h>
#include <string>

class WidgetManager;

class Settings {
public:
    static bool IsSnapToGrid();
    static void SetSnapToGrid(bool enable);
    // Автоудаление ярлыков удалённых приложений (включая игры Steam).
    // По умолчанию включено.
    static bool IsAutoPruneDead();
    static void SetAutoPruneDead(bool enable);
    // Сетка новых групп по умолчанию: 2 (2x2) или 3 (3x3).
    static int GetDefaultGrid();
    static void SetDefaultGrid(int grid);
    // Глобальные горячие клавиши, упакованные DWORD: младшее слово —
    // модификаторы (MOD_CONTROL и т.д.), старшее — VK-код.
    static DWORD GetHotkeySearch();
    static void SetHotkeySearch(DWORD hotkey);
    static DWORD GetHotkeyToggle();
    static void SetHotkeyToggle(DWORD hotkey);
    static DWORD PackHotkey(UINT mods, UINT vk);
    static void UnpackHotkey(DWORD hotkey, UINT& mods, UINT& vk);
    // Счётчик переполнения «+N» в подписи групп. По умолчанию показан.
    static bool IsShowOverflow();
    static void SetShowOverflow(bool enable);
    // true один раз за всё время (показ онбординга), дальше false.
    static bool ConsumeFirstRun();
};

// Автозапуск вместе с системой (HKCU\...\Run, без прав администратора).
namespace Autostart {
    std::wstring GetExePath();
    bool IsEnabled();
    bool SetEnabled(bool enable);
    bool IsDisabledByUser();
    void SetDisabledByUser(bool disabled);
    // Прописывает автозапуск, если пользователь его не отключал.
    // Заодно чинит путь после перемещения/обновления exe.
    void Ensure();
}

namespace SettingsDialog {
    void SetManager(::WidgetManager* manager);
    void Show(HWND hParent);
}
