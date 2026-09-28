#pragma once
// Пути приложения: Roaming AppData через KnownFolder API.
// GetEnvironmentVariableW(MAX_PATH) резал длинные пути и игнорировал ошибки
// (ломались пути данных и логов).
#include <Windows.h>
#include <Shlobj.h>
#include <string>

inline std::wstring GetRoamingAppData()
{
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) || !p)
        return {};
    std::wstring r = p;
    CoTaskMemFree(p);
    return r;
}

// Каталог данных приложения (%APPDATA%\DesktopGroupManager), создаёт при нужде.
// Пусто, если путь недоступен — вызывающий обязан проверить.
inline std::wstring GetAppDataDir()
{
    std::wstring base = GetRoamingAppData();
    if (base.empty()) return {};
    std::wstring dir = base + L"\\DesktopGroupManager";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
