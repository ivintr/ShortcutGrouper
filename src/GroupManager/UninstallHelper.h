#pragma once
// Поиск и запуск деинсталляторов приложений (было в PopupWindow.cpp).
// Используется shell-меню попапа и bulk-меню виджетов.
#include <Windows.h>
#include <string>
#include "WidgetTypes.h"

struct UninstallInfo {
    std::wstring displayName; // для диалога подтверждения
    std::wstring command;     // полная UninstallString (не Steam/Appx)
    std::wstring version;     // DisplayVersion (может быть пустым)
    unsigned long sizeKB = 0; // EstimatedSize, 0 = неизвестно
    std::wstring appxFamily;  // семейство AppX-пакета (только Appx)
    bool isSteam = false;
    bool isAppx = false;
    DWORD steamAppId = 0;
};

void ClearUninstallCache();
bool FindUninstallerForTarget(const std::wstring& target, UninstallInfo& out);
void DoUninstallApp(HWND hwnd, const ShortcutInfo& si, const UninstallInfo& ui,
    bool confirm);
