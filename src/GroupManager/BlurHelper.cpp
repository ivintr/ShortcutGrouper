#include "BlurHelper.h"
#include "Logger.h"
#include "WidgetTypes.h"
#include <dwmapi.h>
#include <cstdio>
#include <strsafe.h>

enum WINDOWCOMPOSITIONATTRIB {
    WCA_ACCENT_POLICY = 19
};

enum ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_ENABLE_HOSTBACKDROP = 5,
    ACCENT_INVALID_STATE = 6
};

struct ACCENT_POLICY {
    int AccentState;
    int AccentFlags;
    int GradientColor;
    int AnimationId;
};

struct WINDOWCOMPOSITIONATTRIBUTEDATA {
    WINDOWCOMPOSITIONATTRIB Attribute;
    PVOID Data;
    int SizeOfData;
};

using pfnSetWindowCompositionAttribute = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBUTEDATA*);

static pfnSetWindowCompositionAttribute GetSetWindowCompositionAttribute()
{
    static pfnSetWindowCompositionAttribute fn = nullptr;
    if (!fn)
    {
        HMODULE hUser = GetModuleHandleW(L"user32.dll");
        if (hUser)
            fn = (pfnSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");
    }
    return fn;
}

bool ApplyAcrylicBlur(HWND hwnd, BYTE alpha)
{
    auto fn = GetSetWindowCompositionAttribute();
    if (!fn) return false;

    ACCENT_POLICY accent = {};
    accent.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    accent.AccentFlags = 2;
    accent.GradientColor = (alpha << 24) | 0x00FFFFFF;
    accent.AnimationId = 0;

    WINDOWCOMPOSITIONATTRIBUTEDATA data = {};
    data.Attribute = WCA_ACCENT_POLICY;
    data.Data = &accent;
    data.SizeOfData = sizeof(accent);

    return fn(hwnd, &data) == TRUE;
}

bool RemoveBlur(HWND hwnd)
{
    auto fn = GetSetWindowCompositionAttribute();
    if (!fn) return false;

    ACCENT_POLICY accent = {};
    accent.AccentState = ACCENT_DISABLED;

    WINDOWCOMPOSITIONATTRIBUTEDATA data = {};
    data.Attribute = WCA_ACCENT_POLICY;
    data.Data = &accent;
    data.SizeOfData = sizeof(accent);

    return fn(hwnd, &data) == TRUE;
}

static void BL(const wchar_t* msg)
{
    AppLog(L"BL", L"%s", msg);
}

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    WCHAR cls[128] = {};
    GetClassNameW(hwnd, cls, 128);
    WCHAR buf[256];
    swprintf_s(buf, L"enum: %p class=%s", (void*)hwnd, cls);
    BL(buf);
    return TRUE;
}

HWND FindDesktopWorkerW()
{
    BL(L"FindDesktopWorkerW: searching...");

    HWND hProgman = FindWindowW(L"Progman", nullptr);
    WCHAR buf[64];
    swprintf_s(buf, L"Progman=%p", (void*)hProgman);
    BL(buf);

    if (!hProgman)
    {
        BL(L"Progman not found, enum top-level windows...");
        EnumWindows(EnumWindowsProc, 0);
        return nullptr;
    }

    SendMessageTimeoutW(hProgman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);
    BL(L"Sent 0x052C to Progman");

    HWND hDefView = FindWindowExW(hProgman, nullptr, L"SHELLDLL_DefView", nullptr);
    swprintf_s(buf, L"DefView=%p", (void*)hDefView);
    BL(buf);

    if (!hDefView)
    {
        BL(L"DefView not found, listing children of Progman...");
        HWND hChild = FindWindowExW(hProgman, nullptr, nullptr, nullptr);
        while (hChild)
        {
            WCHAR cls[128] = {};
            GetClassNameW(hChild, cls, 128);
            WCHAR cbuf[256];
            swprintf_s(cbuf, L"  child=%p class=%s", (void*)hChild, cls);
            BL(cbuf);
            hChild = FindWindowExW(hProgman, hChild, nullptr, nullptr);
        }
        return nullptr;
    }

    // Method 1: WorkerW as child of Progman (sibling after DefView)
    HWND hWorkerW = FindWindowExW(hProgman, hDefView, L"WorkerW", nullptr);
    swprintf_s(buf, L"WorkerW(child Progman after DefView)=%p", (void*)hWorkerW);
    BL(buf);

    if (!hWorkerW)
    {
        // Method 2: find any top-level WorkerW whose child is DefView
        BL(L"Trying to find WorkerW parent of DefView...");
        HWND hScan = nullptr;
        int count = 0;
        while (true)
        {
            hScan = FindWindowExW(nullptr, hScan, L"WorkerW", nullptr);
            if (!hScan) break;
            count++;
            HWND hChildDV = FindWindowExW(hScan, nullptr, L"SHELLDLL_DefView", nullptr);
            if (hChildDV == hDefView)
            {
                hWorkerW = hScan;
                swprintf_s(buf, L"Found WorkerW parent of DefView: %p", (void*)hWorkerW);
                BL(buf);
                break;
            }
        }
        swprintf_s(buf, L"Scanned %d top-level WorkerW windows", count);
        BL(buf);
    }

    if (!hWorkerW)
    {
        BL(L"Trying WorkerW as sibling of Progman...");
        hWorkerW = FindWindowExW(nullptr, hProgman, L"WorkerW", nullptr);
        swprintf_s(buf, L"WorkerW(sibling Progman)=%p", (void*)hWorkerW);
        BL(buf);
    }

    return hWorkerW;
}
