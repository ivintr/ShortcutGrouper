#pragma once
#include <Windows.h>

bool ApplyAcrylicBlur(HWND hwnd, BYTE alpha = 0xCC);
bool RemoveBlur(HWND hwnd);
HWND FindDesktopWorkerW();
