#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include "WidgetTypes.h"

HICON ExtractFolderIcon(int size = 48);
ShortcutInfo ExtractLnkInfo(const std::wstring& lnkPath);
// Явная иконка, прописанная в самом .lnk (IconLocation) — то, что
// показывает Проводник. nullptr, если её нет или файл недоступен.
HICON IconFromLnkLocation(const std::wstring& lnkPath);
