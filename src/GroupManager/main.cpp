// GroupManager.exe — Desktop folder widget manager
// Renders iOS/Android-style folder widgets on the Windows desktop.

#include <Windows.h>
#include <CommCtrl.h>
#include <ShObjIdl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <new>
#include <wchar.h>
#include "WidgetManager.h"
#include "Logger.h"
#include "DesktopGrid.h"
#include "Renderer.h"
#include "OpenWithDetector.h"
#include "BlurHelper.h"
#include "TrayIcon.h"
#include "SettingsDialog.h"
#include "Updater.h"
#include "Lang.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")

static const wchar_t* IPC_CLASS_NAME = kGroupManagerIpcClass;
static const UINT WM_TRAYICON = WM_USER + 200;
static const UINT WM_SETTINGS_TOGGLE = WM_USER + 201;
static const UINT WM_ASYNC_REFRESH = WM_USER + 202;
static const UINT WM_ASYNC_RESCUE = WM_USER + 203;
static const UINT WM_ASYNC_DUMP = WM_USER + 204;
static const UINT IPC_APPLY_HOTKEYS = WM_USER + 205;
// 206 занят WM_TRAY_BALLOON_MSG (см. WidgetTypes.h) — не пересекаться!
static const UINT WM_ASYNC_GROUPCMD = WM_USER + 207;
// WM_TRAY_BALLOON_MSG — общий из WidgetTypes.h, алиас не нужен.

// Глобальные горячие клавиши (фиксированные комбинации):
// Ctrl+Shift+Space — поиск, Ctrl+Shift+H — показать/скрыть виджеты.
static const int HOTKEY_SEARCH = 1;
static const int HOTKEY_TOGGLE_WIDGETS = 2;
static void ApplyAppHotkeys();
// Таймеры стартового пути: 1 = повторный поиск WorkerW (Explorer ещё
// не готов при автозапуске), 2 = отложенная догрузка иконок.
static const UINT_PTR TIMER_WORKER_RETRY = 1;
static const UINT_PTR TIMER_LOAD_ICONS = 2;
static int s_workerRetries = 0;

static WidgetRenderer g_renderer;
static WidgetManager  g_manager;
static TrayIcon       g_tray;
// Explorer перезапустился — пересоздать иконку трея.
static UINT s_uTaskbarCreated = 0;

static ULONGLONG g_startTick = 0;

static void DebugLog(const wchar_t* fmt, ...);

static void StageLog(const wchar_t* stage)
{
    ULONGLONG ms = GetTickCount64() - g_startTick;
    WCHAR buf[256];
    swprintf_s(buf, L"T+%llums %s", ms, stage);
    DebugLog(L"%s", buf);
}

static void DebugLog(const wchar_t* fmt, ...)
{
    WCHAR buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, 1024, _TRUNCATE, fmt, args);
    va_end(args);

    AppLog(nullptr, L"%s", buf);
}

// ---------------------------------------------------------------------------
// IPC hidden window
// ---------------------------------------------------------------------------
static HWND g_hIpcWnd = nullptr;
// Мьютекс синглтона живёт весь процесс (не локальный HANDLE).
static HANDLE g_hSingletonMutex = nullptr;

static LRESULT CALLBACK IpcWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (s_uTaskbarCreated != 0 && uMsg == s_uTaskbarCreated)
    {
        g_tray.ReAdd();
        return 0;
    }

    if (uMsg == WM_COPYDATA)
    {
        DebugLog(L"IPC: WM_COPYDATA received");
        COPYDATASTRUCT* cds = (COPYDATASTRUCT*)lParam;
        // Валидируем чужой буфер ДО чтения: проверяем размер и наличие
        // терминатора внутри cbData, иначе читаем чужую память.
        // Лимит 256 КБ: командная строка всё равно ограничена ~32К символов,
        // а безлимитное копирование — лёгкий DoS через чужой SendMessage.
        if (cds && cds->dwData == 1 && cds->lpData &&
            cds->cbData >= sizeof(wchar_t) &&
            cds->cbData <= 256 * 1024)
        {
            size_t maxChars = cds->cbData / sizeof(wchar_t);
            const wchar_t* src = (const wchar_t*)cds->lpData;
            size_t len = wcsnlen(src, maxChars);
            if (len >= maxChars)
            {
                DebugLog(L"IPC: unterminated COPYDATA, ignored");
                return TRUE;
            }
            // Rescue выполняет живой процесс асинхронно: внутри SendMessage
            // запрещены исходящие COM-вызовы, а rescue весь на shell COM.
            if (wcscmp(src, L"--rescue-icons") == 0)
            {
                DebugLog(L"IPC: rescue command, deferring...");
                PostMessageW(hWnd, WM_ASYNC_RESCUE, 0, 0);
                return TRUE;
            }
            if (wcscmp(src, L"--dump-windows") == 0)
            {
                DebugLog(L"IPC: dump command, deferring...");
                PostMessageW(hWnd, WM_ASYNC_DUMP, 0, 0);
                return TRUE;
            }
            // Групповые команды тоже асинхронно: OnGroupCommand делает
            // файловый IO и shell COM, внутри SendMessage это дедлок
            // отправителя и RPC_E_CANTCALLOUT_ININPUTSYNCCALL у нас.
            DebugLog(L"IPC cmd: %s", src);
            wchar_t* copy = new (std::nothrow) wchar_t[len + 1];
            if (copy)
            {
                wcscpy_s(copy, len + 1, src);
                // Очередь может быть переполнена — тогда чистим копию сами,
                // иначе утечка + тихая потеря команды.
                if (!PostMessageW(hWnd, WM_ASYNC_GROUPCMD, (WPARAM)copy, 0))
                {
                    delete[] copy;
                    DebugLog(L"IPC: PostMessage failed, command dropped");
                }
            }
            DebugLog(L"IPC: OnGroupCommand deferred");
        }
        return TRUE;
    }

    if (uMsg == WM_ASYNC_GROUPCMD)
    {
        wchar_t* cmd = (wchar_t*)wParam;
        if (cmd)
        {
            // Служебные команды обновления (CLI --check-for-updates /
            // --install-update): не grouping, выполняет Updater.
            if (wcscmp(cmd, L"--check-for-updates") == 0)
            {
                DebugLog(L"IPC: update check requested");
                Updater::CheckAsync(true);
            }
            else if (wcscmp(cmd, L"--install-update") == 0)
            {
                DebugLog(L"IPC: update install requested");
                if (!Updater::InstallUpdate(g_hIpcWnd))
                    Updater::CheckAsync(true);
            }
            else
            {
                g_manager.OnGroupCommand(cmd);
                DebugLog(L"IPC: OnGroupCommand done");
            }
            delete[] cmd;
        }
        return 0;
    }

    if (uMsg == IPC_CLASS_MSG)
    {
        // Асинхронно: внутри SendMessage обработчика запрещены исходящие
        // COM-вызовы (RPC_E_CANTCALLOUT_ININPUTSYNCCALL), а Refresh двигает
        // иконки через shell COM. Постим себе и выходим сразу.
        DebugLog(L"IPC: Refresh signal received, deferring...");
        PostMessageW(hWnd, WM_ASYNC_REFRESH, 0, 0);
        return 0;
    }

    if (uMsg == WM_ASYNC_REFRESH)
    {
        DebugLog(L"IPC: Async refresh, refreshing widgets...");
        g_manager.RefreshWidgets();
        DebugLog(L"IPC: Refresh done");
        return 0;
    }

    if (uMsg == WM_ASYNC_RESCUE)
    {
        DebugLog(L"IPC: Async rescue, rescuing icons...");
        g_manager.RescueDesktopIcons();
        DebugLog(L"IPC: Rescue done");
        return 0;
    }

    if (uMsg == WM_ASYNC_DUMP)
    {
        DebugLog(L"IPC: Async dump...");
        DumpDesktopWindows();
        DebugLog(L"IPC: Dump done");
        return 0;
    }

    if (uMsg == IPC_APPLY_HOTKEYS)
    {
        ApplyAppHotkeys();
        return 0;
    }

    if (uMsg == WM_TRAY_BALLOON_MSG)
    {
        TrayBalloon* b = (TrayBalloon*)lParam;
        if (b)
        {
            g_tray.ShowBalloon(b->title.c_str(), b->text.c_str());
            delete b;
        }
        return 0;
    }

    if (uMsg == WM_DND_DROP)
    {
        struct DndData { POINT down; POINT up; };
        DndData* pData = (DndData*)lParam;
        if (pData)
        {
            DebugLog(L"DnD: drop (%d,%d)->(%d,%d)", pData->down.x, pData->down.y, pData->up.x, pData->up.y);
            std::wstring name1 = DndGetAccessibleName(pData->down);
            DebugLog(L"DnD: name1=%s", name1.empty() ? L"-" : name1.c_str());

            std::wstring widgetGroupId = g_manager.FindGroupAtPoint(pData->up);
            DebugLog(L"DnD: widgetGroup=%s", widgetGroupId.empty() ? L"-" : widgetGroupId.c_str());

            if (!widgetGroupId.empty() && !name1.empty())
            {
                std::wstring src = DndFindLnkByDisplayName(name1);
                DebugLog(L"DnD: src=%s", src.empty() ? L"-" : src.c_str());
                if (!src.empty())
                {
                    DebugLog(L"DnD: adding %s to group %s", src.c_str(), widgetGroupId.c_str());
                    g_manager.AddShortcutToGroup(widgetGroupId, src);
                }
            }
            else
            {
                std::wstring name2 = DndGetAccessibleName(pData->up);
                DebugLog(L"DnD: name2=%s", name2.empty() ? L"-" : name2.c_str());
                if (!name1.empty() && !name2.empty())
                {
                    DebugLog(L"DnD: looking up lnk for name1=%s", name1.c_str());
                    std::wstring src = DndFindLnkByDisplayName(name1);
                    DebugLog(L"DnD: src=%s", src.empty() ? L"-" : src.c_str());
                    DebugLog(L"DnD: looking up lnk for name2=%s", name2.c_str());
                    std::wstring dst = DndFindLnkByDisplayName(name2);
                    DebugLog(L"DnD: dst=%s", dst.empty() ? L"-" : dst.c_str());
                    if (!src.empty() && !dst.empty() && _wcsicmp(src.c_str(), dst.c_str()) != 0)
                    {
                        DebugLog(L"DnD: grouping %s -> %s", src.c_str(), dst.c_str());
                        std::wstring cmd = L"\"" + src + L"\" \"" + dst + L"\"";
                        g_manager.OnGroupCommand(cmd.c_str());
                    }
                }
            }
            delete pData;
        }
        return 0;
    }

    if (uMsg == WM_HOTKEY)
    {
        if (wParam == HOTKEY_SEARCH)
            g_manager.ToggleSearch();
        else if (wParam == HOTKEY_TOGGLE_WIDGETS)
            g_manager.ToggleWidgetsVisible();
        return 0;
    }

    if (uMsg == WM_DESTROY)
    {
        UnregisterHotKey(hWnd, HOTKEY_SEARCH);
        UnregisterHotKey(hWnd, HOTKEY_TOGGLE_WIDGETS);
        g_tray.Remove();
        // БЕЗ DestroyWindow: мы уже внутри WM_DESTROY. Выходим из цикла
        // GetMessage через WM_QUIT, иначе выход из трея зависает.
        PostQuitMessage(0);
        return 0;
    }

    if (uMsg == WM_TRAYICON)
    {
        if (lParam == WM_RBUTTONUP)
        {
            g_tray.ShowModernMenu(hWnd, &g_renderer, g_manager.IsWidgetsHidden());
        }
        return 0;
    }

    if (uMsg == WM_COMMAND)
    {
        WORD id = LOWORD(wParam);
        if (id == IDM_SETTINGS)
        {
            g_manager.OnSettingsToggle();
        }
        else if (id == IDM_SEARCH)
        {
            g_manager.ShowSearch();
        }
        else if (id == IDM_TOGGLE_VIS)
        {
            g_manager.ToggleWidgetsVisible();
        }
        else if (id == IDM_REFRESH)
        {
            g_manager.RefreshWidgets();
        }
        else if (id == IDM_UPDATE)
        {
            // Обновление известно — ставим; иначе проверяем прямо сейчас.
            if (!Updater::InstallUpdate(g_hIpcWnd))
                Updater::CheckAsync(true);
        }
        else if (id == IDM_EXIT)
        {
            // Remove трея — в WM_DESTROY, здесь только закрываем окно.
            DestroyWindow(hWnd);
        }
        return 0;
    }

    if (uMsg == WM_SETTINGS_TOGGLE)
    {
        g_manager.OnSettingsToggle();
        return 0;
    }

    if (uMsg == WM_TIMER)
    {
        if (wParam == TIMER_WORKER_RETRY)
        {
            // Explorer наконец поднялся — создаём виджеты и грузим иконки.
            HWND hW = FindDesktopWorkerW();
            if (hW)
            {
                KillTimer(hWnd, TIMER_WORKER_RETRY);
                s_workerRetries = 0;
                g_manager.CreateAllWidgets(hW);
                StageLog(L"widgets created (retry)");
                SetTimer(hWnd, TIMER_LOAD_ICONS, 800, nullptr);
            }
            else if (++s_workerRetries > 30)
            {
                // Не сдаёмся навсегда (иначе при позднем старте Explorer
                // виджеты не появятся никогда): переходим на редкий опрос.
                // Флаг отложки иконок НЕ снимаем — виджетов пока нет.
                KillTimer(hWnd, TIMER_WORKER_RETRY);
                SetTimer(hWnd, TIMER_WORKER_RETRY, 10000, nullptr);
            }
            return 0;
        }
        if (wParam == TIMER_LOAD_ICONS)
        {
            KillTimer(hWnd, TIMER_LOAD_ICONS);
            g_renderer.SetDeferIcons(false);
            g_manager.RefreshWidgetIcons();
            StageLog(L"icons loaded");
            return 0;
        }
        return 0;
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Читает комбинации из настроек и перерегистрирует. Безопасно звать
// повторно (снятие + новая регистрация). Занятые — тихо пропускаем в лог.
static void ApplyAppHotkeys()
{
    if (!g_hIpcWnd) return;
    UnregisterHotKey(g_hIpcWnd, HOTKEY_SEARCH);
    UnregisterHotKey(g_hIpcWnd, HOTKEY_TOGGLE_WIDGETS);
    UINT mods = 0, vk = 0;
    Settings::UnpackHotkey(Settings::GetHotkeySearch(), mods, vk);
    if (vk != 0)
    {
        if (!RegisterHotKey(g_hIpcWnd, HOTKEY_SEARCH, mods & 0xFFFF, vk))
            DebugLog(L"Hotkey search busy: %d", GetLastError());
    }
    Settings::UnpackHotkey(Settings::GetHotkeyToggle(), mods, vk);
    if (vk != 0)
    {
        if (!RegisterHotKey(g_hIpcWnd, HOTKEY_TOGGLE_WIDGETS, mods & 0xFFFF, vk))
            DebugLog(L"Hotkey toggle busy: %d", GetLastError());
    }
}

static bool CreateIpcWindow(HINSTANCE hInst)
{    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = IpcWndProc;
    wc.hInstance      = hInst;
    wc.lpszClassName  = IPC_CLASS_NAME;
    RegisterClassExW(&wc);

    g_hIpcWnd = CreateWindowExW(WS_EX_TOOLWINDOW, IPC_CLASS_NAME, L"", 0,
        -32000, -32000, 1, 1, nullptr, nullptr, hInst, nullptr);
    return g_hIpcWnd != nullptr;
}

// ---------------------------------------------------------------------------
// Send command to existing instance
// ---------------------------------------------------------------------------
static bool SendToExistingInstance(LPCWSTR cmdLine)
{
    HWND hExisting = FindWindowW(IPC_CLASS_NAME, nullptr);
    if (!hExisting) return false;

    COPYDATASTRUCT cds = {};
    cds.dwData = 1;
    cds.cbData = (DWORD)((wcslen(cmdLine) + 1) * sizeof(wchar_t));
    cds.lpData = (void*)cmdLine;
    // ABORTIFHUNG: не виснем, если живой процесс завис; провал честно
    // возвращаем вызывающему вместо вечного «true».
    LRESULT res = SendMessageTimeoutW(hExisting, WM_COPYDATA, 0, (LPARAM)&cds,
        SMTO_NORMAL | SMTO_ABORTIFHUNG, 5000, nullptr);
    return res != 0;
}

// Разбор командной строки через CommandLineToArgvW: сырой lpCmdLine
// ломается на кавычках/пробелах ("--rescue-icons", лишние аргументы).
static bool CmdLineHasArg(LPCWSTR lpCmdLine, LPCWSTR name)
{
    if (!lpCmdLine || !lpCmdLine[0]) return false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool found = false;
    for (int i = 1; i < argc; i++)
    {
        if (_wcsicmp(argv[i], name) == 0) { found = true; break; }
    }
    LocalFree(argv);
    return found;
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int)
{
    // Per-monitor DPI awareness BEFORE any window is created.
    // Without this Windows bitmap-scales our layered windows on displays
    // with >100% scaling and all rendered text looks blurry.
    {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32)
        {
            typedef BOOL(WINAPI* SetDpiCtxFn)(PVOID);
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((PVOID)-4)
#endif
            SetDpiCtxFn fn = (SetDpiCtxFn)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
            if (fn)
                fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            else
                SetProcessDPIAware();
        }
    }

    DebugLog(L"=== GroupManager starting ===");
    DebugLog(L"CmdLine: %s", lpCmdLine ? lpCmdLine : L"(empty)");
    Lang::Initialize();
    g_startTick = GetTickCount64();

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    DebugLog(L"CoInitialize: 0x%08X", hrCo);
    StageLog(L"CoInitialize done");

    // Тёмная тема для СИСТЕМНЫХ меню (включая настоящее shell-меню Windows
    // в попапе): без этого TrackPopupMenu всегда светлый даже в тёмной теме.
    // AllowDark = следовать системной теме, ничего не навязываем.
    {
        HMODULE hUx = GetModuleHandleW(L"uxtheme.dll");
        if (!hUx) hUx = LoadLibraryW(L"uxtheme.dll");
        if (hUx)
        {
            using FnMode = int(WINAPI*)(int); // SetPreferredAppMode, ordinal 135
            using FnRefresh = void(WINAPI*)(); // RefreshImmersiveColorPolicyState, 104
            auto fnMode = (FnMode)GetProcAddress(hUx, MAKEINTRESOURCEA(135));
            auto fnRefresh = (FnRefresh)GetProcAddress(hUx, MAKEINTRESOURCEA(104));
            if (fnMode) fnMode(1 /*AllowDark*/);
            if (fnRefresh) fnRefresh();
        }
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    bool hasArgs = (lpCmdLine && lpCmdLine[0] != L'\0');
    DebugLog(L"HasArgs: %d", hasArgs);

    // Rescue/dump выполняет живой процесс (у него видимые окна и виджеты):
    // пересылаем команду ему через COPYDATA, сами выходим.
    if (hasArgs && (CmdLineHasArg(lpCmdLine, L"--rescue-icons") ||
                    CmdLineHasArg(lpCmdLine, L"--dump-windows")))
    {
        if (SendToExistingInstance(CmdLineHasArg(lpCmdLine, L"--rescue-icons") ?
                L"--rescue-icons" : L"--dump-windows"))
        {
            DebugLog(L"Command forwarded to running instance");
            if (hrCo == S_OK || hrCo == S_FALSE) CoUninitialize();
            return 0;
        }
        DebugLog(L"No running instance, handling locally");
    }

    // Команды обновления (CLI --check-for-updates / --install-update):
    // выполняет только живой процесс; без него делать нечего (состояния
    // обновлений нет) — точно не скармливаем их OnGroupCommand как пути.
    if (hasArgs && (CmdLineHasArg(lpCmdLine, L"--check-for-updates") ||
                    CmdLineHasArg(lpCmdLine, L"--install-update")))
    {
        const wchar_t* cmd = CmdLineHasArg(lpCmdLine, L"--check-for-updates") ?
            L"--check-for-updates" : L"--install-update";
        if (SendToExistingInstance(cmd))
            DebugLog(L"Update command forwarded to running instance");
        else
            DebugLog(L"No running instance for update command");
        if (hrCo == S_OK || hrCo == S_FALSE) CoUninitialize();
        return 0;
    }

    // Синглтон для полного запуска: второй полный инстанс плодил бы
    // параллельные сдвиги иконок и виджетов. Мьютекс держим в static до
    // выхода процесса (локальный HANDLE умирал бы вместе со scope).
    g_hSingletonMutex = CreateMutexW(nullptr, FALSE, L"DesktopGroupManager_Singleton");
    bool alreadyRunning = g_hSingletonMutex &&
        GetLastError() == ERROR_ALREADY_EXISTS;
    if (!hasArgs && alreadyRunning)
    {
        HWND hExisting = FindWindowW(IPC_CLASS_NAME, nullptr);
        if (hExisting)
            SendMessageTimeoutW(hExisting, IPC_CLASS_MSG, 0, 0,
                SMTO_NORMAL | SMTO_ABORTIFHUNG, 3000, nullptr);
        DebugLog(L"Another instance is running, exiting");
        CloseHandle(g_hSingletonMutex);
        g_hSingletonMutex = nullptr;
        if (hrCo == S_OK || hrCo == S_FALSE) CoUninitialize();
        return 0;
    }

    if (hasArgs)
    {
        HWND hExisting = FindWindowW(IPC_CLASS_NAME, nullptr);
        DebugLog(L"Existing IPC window: %p", hExisting);

        // Живой процесс есть — команду выполняет он (у него виджеты и
        // актуальное состояние). Локально не обрабатываем, иначе два
        // процесса параллельно правят groups.json и двигают .lnk.
        if (hExisting && SendToExistingInstance(lpCmdLine))
        {
            DebugLog(L"Group command forwarded to running instance");
            SendMessageTimeoutW(hExisting, IPC_CLASS_MSG, 0, 0,
                SMTO_NORMAL | SMTO_ABORTIFHUNG, 3000, nullptr);
            CloseHandle(g_hSingletonMutex);
            g_hSingletonMutex = nullptr;
            if (hrCo == S_OK || hrCo == S_FALSE) CoUninitialize();
            return 0;
        }

        // Живого процесса нет — обрабатываем сами, сериализуясь с другими
        // такими же временными процессами именованным мьютексом.
        HANDLE hCmdSerial = CreateMutexW(nullptr, FALSE,
            L"DesktopGroupManager_CmdSerial");
        if (hCmdSerial)
            WaitForSingleObject(hCmdSerial, 30000);

        // Lightweight init: only need JSON/file operations, no D2D or widgets
        g_renderer.Initialize();
        SetGlobalRenderer(&g_renderer);
        SetGlobalManager(&g_manager);
        // Баллуны из PruneDeadShortcuts иначе теряются (окна у нас нет):
        // шлём их живому процессу, если он есть.
        if (hExisting)
            g_manager.SetNotifyWindow(hExisting);
        g_manager.Initialize(&g_renderer);

        g_manager.OnGroupCommand(lpCmdLine);
        DebugLog(L"Group command processed");

        // Signal the running instance to refresh widgets
        if (hExisting)
        {
            SendMessageTimeoutW(hExisting, IPC_CLASS_MSG, 0, 0,
                SMTO_NORMAL | SMTO_ABORTIFHUNG, 3000, nullptr);
            DebugLog(L"Signaled existing instance to refresh");
        }

        // Do NOT call Shutdown — we never created any widgets
        g_renderer.Shutdown();
        if (hCmdSerial)
        {
            ReleaseMutex(hCmdSerial);
            CloseHandle(hCmdSerial);
        }
        // g_hSingletonMutex закрывается на выходе процесса
        if (hrCo == S_OK || hrCo == S_FALSE) CoUninitialize();
        return 0;
    }

    if (!CreateIpcWindow(hInst))
    {
        DebugLog(L"Failed to create IPC window");
        if (g_hSingletonMutex) { CloseHandle(g_hSingletonMutex); g_hSingletonMutex = nullptr; }
        if (hrCo == S_OK || hrCo == S_FALSE) CoUninitialize();
        return 1;
    }
    DebugLog(L"IPC window created");

    // Менеджеру — окно для баллунов трея (уведомления из логики).
    g_manager.SetNotifyWindow(g_hIpcWnd);
    Updater::SetNotifyWindow(g_hIpcWnd);

    // Глобальные хоткеи из настроек (тихо пропускаем занятые комбинации).
    ApplyAppHotkeys();

    s_uTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_tray.Create(hInst, g_hIpcWnd, WM_TRAYICON);
    DebugLog(L"Tray icon created");
    StageLog(L"tray created");

    Autostart::Ensure();
    DebugLog(L"Autostart ensured: %d", Autostart::IsEnabled() ? 1 : 0);

    OpenWithDetector_Start(g_hIpcWnd);
    DebugLog(L"OpenWithDetector started");
    StageLog(L"openwith started");

    g_renderer.Initialize();
    SetGlobalRenderer(&g_renderer);
    SetGlobalManager(&g_manager);
    StageLog(L"renderer init done");

    g_manager.Initialize(&g_renderer);
    DebugLog(L"Manager initialized");
    StageLog(L"manager init done (incl. prune)");

    // Иконки shell грузятся дорого (десятки обращений к диску) — виджеты
    // рисуем сразу с плейсхолдерами, иконки догоняем таймером ниже.
    // Так окно и трей появляются мгновенно даже на занятой системе.
    g_renderer.SetDeferIcons(true);

    HWND hDesktopWorker = FindDesktopWorkerW();
    DebugLog(L"Desktop WorkerW: %p", hDesktopWorker);
    StageLog(L"workerW found");

    if (hDesktopWorker)
    {
        g_manager.CreateAllWidgets(hDesktopWorker);
        DebugLog(L"Widgets created");
        StageLog(L"widgets created");
        SetTimer(g_hIpcWnd, TIMER_LOAD_ICONS, 800, nullptr);
    }
    else
    {
        // Автозапуск раньше Explorer: повторяем поиск, виджеты встанут сами.
        DebugLog(L"WorkerW not ready, will retry");
        s_workerRetries = 0;
        SetTimer(g_hIpcWnd, TIMER_WORKER_RETRY, 2000, nullptr);
    }

    DebugLog(L"Entering message loop");
    // Фоновая проверка обновлений (троттлинг сутки — внутри Updater).
    Updater::CheckAsync();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_manager.Shutdown();
    OpenWithDetector_Stop();
    g_renderer.Shutdown();

    // CoUninitialize только если CoInitialize реально инициализировал COM
    // в этом потоке (S_OK/S_FALSE). При RPC_E_CHANGED_MODE (уже MTA)
    // вызов был бы разбалансирован.
    if (hrCo == S_OK || hrCo == S_FALSE) CoUninitialize();
    if (g_hSingletonMutex) { CloseHandle(g_hSingletonMutex); g_hSingletonMutex = nullptr; }
    DebugLog(L"=== GroupManager exiting ===");
    return 0;
}
