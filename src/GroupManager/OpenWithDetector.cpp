#include "OpenWithDetector.h"
#include "WidgetTypes.h"
#include <Windows.h>
#include <CommCtrl.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <ShObjIdl.h>
#include <OleAcc.h>
#include <cstdlib>
#include <psapi.h>
#include <strsafe.h>
#include <string>

#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Oleacc.lib")

static bool IsDesktopWindow(HWND hWnd)
{
    if (!hWnd) return false;
    HWND hTop = hWnd;
    while (GetParent(hTop)) hTop = GetParent(hTop);
    WCHAR cls[64] = {};
    GetClassNameW(hTop, cls, 64);
    if (wcscmp(cls, L"Progman") == 0) return true;
    if (wcscmp(cls, L"WorkerW") == 0) return true;
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0) return true;
    return false;
}

std::wstring DndGetAccessibleName(POINT ptScreen)
{
    IAccessible* pAcc = nullptr;
    VARIANT varChild;
    VariantInit(&varChild);
    HRESULT hr = AccessibleObjectFromPoint(ptScreen, &pAcc, &varChild);
    if (FAILED(hr) || !pAcc) { VariantClear(&varChild); return {}; }
    BSTR bstrName = nullptr;
    hr = pAcc->get_accName(varChild, &bstrName);
    std::wstring name;
    if (SUCCEEDED(hr) && bstrName) { name = bstrName; SysFreeString(bstrName); }
    pAcc->Release();
    VariantClear(&varChild);
    return name;
}

std::wstring DndFindLnkByDisplayName(const std::wstring& displayName)
{
    // Ищем и на личном, и на общем рабочем столе (Public) — например,
    // Microsoft Edge и Яндекс лежат именно там
    WCHAR userDesktop[MAX_PATH] = {};
    WCHAR publicDesktop[MAX_PATH] = {};
    bool hasUser = SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY,
        nullptr, 0, userDesktop));
    bool hasPublic = SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_DESKTOPDIRECTORY,
        nullptr, 0, publicDesktop));

    const wchar_t* dirs[2] = { userDesktop, publicDesktop };
    bool has[2] = { hasUser, hasPublic };
    const wchar_t* patterns[3] = { L"\\*.lnk", L"\\*.url", L"\\*" };
    for (int di = 0; di < 2; di++)
    {
        if (!has[di]) continue;
        for (int pi = 0; pi < 3; pi++)
        {
    std::wstring searchPath = std::wstring(dirs[di]) + patterns[pi];
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) continue;
    do
    {
        std::wstring fileName = fd.cFileName;
        if (fileName == L"." || fileName == L"..") continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        std::wstring nameNoExt = fileName;
        if (!isDir)
        {
            size_t dot = nameNoExt.rfind(L'.');
            if (dot != std::wstring::npos) nameNoExt.erase(dot);
        }
        if (_wcsicmp(nameNoExt.c_str(), displayName.c_str()) == 0 ||
            _wcsicmp(fileName.c_str(), displayName.c_str()) == 0)
        {
            FindClose(hFind);
            return std::wstring(dirs[di]) + L"\\" + fileName;
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Hook — writes to globals only
// ---------------------------------------------------------------------------
static HHOOK  g_hHook    = nullptr;
static bool   g_downFired = false;
static POINT  g_downPt   = {};
static bool   g_upFired   = false;
static POINT  g_upPt     = {};
static HWND   g_hIpcWnd  = nullptr;

static LRESULT CALLBACK DndHookProc(int nCode, WPARAM wp, LPARAM lp)
{
    if (nCode >= 0)
    {
        MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lp;
        if (wp == WM_LBUTTONDOWN)
        {
            g_downFired = true;
            g_downPt = ms->pt;
            g_upFired = false;
        }
        else if (wp == WM_LBUTTONUP)
        {
            g_upFired = true;
            g_upPt = ms->pt;
        }
    }
    return CallNextHookEx(g_hHook, nCode, wp, lp);
}

// ---------------------------------------------------------------------------
// Timer — lightweight check, defers to IpcWndProc via PostMessage
// ---------------------------------------------------------------------------
static void CALLBACK DndTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    if (!g_upFired) return;
    POINT ptDown = g_downPt;
    POINT ptUp = g_upPt;
    g_upFired = false;
    g_downFired = false;

    HWND hWndDown = WindowFromPoint(ptDown);
    if (!IsDesktopWindow(hWndDown)) return;
    if (!g_hIpcWnd) return;

    struct DndData { POINT down; POINT up; };
    DndData* pData = new(std::nothrow) DndData{ ptDown, ptUp };
    // Без проверки PostMessage — утечка pData при переполнении очереди.
    if (pData && !PostMessageW(g_hIpcWnd, WM_DND_DROP, 0, (LPARAM)pData))
        delete pData;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool OpenWithDetector_Start(HWND hIpcWnd)
{
    g_hIpcWnd = hIpcWnd;

    OutputDebugStringW(L"[DnD] SetHook...\n");
    g_hHook = SetWindowsHookExW(WH_MOUSE_LL, DndHookProc, nullptr, 0);
    OutputDebugStringW(g_hHook ? L"[DnD] Hook OK\n" : L"[DnD] Hook FAILED\n");

    UINT_PTR tid = SetTimer(hIpcWnd, 1001, 50, DndTimerProc);
    wchar_t buf[64]; swprintf_s(buf, L"[DnD] Timer=%llu\n", (ULONGLONG)tid);
    OutputDebugStringW(buf);

    return g_hHook != nullptr;
}

void OpenWithDetector_Stop()
{
    if (g_hHook) { UnhookWindowsHookEx(g_hHook); g_hHook = nullptr; }
    if (g_hIpcWnd) KillTimer(g_hIpcWnd, 1001);
}
