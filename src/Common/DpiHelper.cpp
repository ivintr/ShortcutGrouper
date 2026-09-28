#include "DpiHelper.h"
#include <Windows.h>

UINT GetDpiForWindowCompat(HWND hwnd)
{
    using FnDpi = UINT(WINAPI*)(HWND);
    static FnDpi fn = nullptr;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        HMODULE hUser = GetModuleHandleW(L"user32.dll");
        if (hUser)
            fn = (FnDpi)GetProcAddress(hUser, "GetDpiForWindow");
    }
    if (fn && hwnd)
    {
        UINT dpi = fn(hwnd);
        if (dpi >= 48 && dpi <= 960)
            return dpi;
    }
    HDC hdc = GetDC(nullptr);
    UINT fallback = 96;
    if (hdc)
    {
        int dx = GetDeviceCaps(hdc, LOGPIXELSX);
        if (dx >= 48 && dx <= 960) fallback = (UINT)dx;
        ReleaseDC(nullptr, hdc);
    }
    return fallback;
}
