#include <Windows.h>
#include <Shlwapi.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <cctype>
#include <new>
#include "SettingsDialog.h"
#include "WidgetManager.h"
#include "ModernMenu.h"
#include "Lang.h"
#include "Reg.h"
#include "DpiHelper.h"
#include "../../resources/resource.h"

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")

static const wchar_t* REG_APP = L"Software\\DesktopGroupManager";
static const wchar_t* SETTINGS_CLASS = L"GMSettingsWnd";

static const wchar_t* REG_RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* REG_RUN_VALUE = L"DesktopGroupManager";
// Windows 10+: Диспетчер задач / Параметры > Автозагрузка хранит своё
// согласие отдельно. Первый байт: 0x02 = разрешено, 0x03 = запрещено.
// Пока там 0x03, Explorer игнорирует значение в Run, даже если оно есть.
static const wchar_t* REG_APPROVED_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

static const int IDC_BTN_CLOSE = 2003;
static const int IDC_ROW_AUTOSTART = 2004;
static const int IDC_ROW_SNAP = 2005;
static const int IDC_ROW_PRUNE = 2006;
static const int IDC_ROW_DEFGRID = 2007;
static const int IDC_ROW_LANG = 2008;
static const int IDC_ROW_HOTSEARCH = 2009;
static const int IDC_ROW_HOTTOGGLE = 2010;
static const int IDC_ROW_OVERFLOW = 2011;

static WidgetManager* s_settingsManager = nullptr;
static HWND s_hSettingsWnd = nullptr;

void SettingsDialog::SetManager(WidgetManager* manager)
{
    s_settingsManager = manager;
}

static DWORD RegGetDword(const wchar_t* valueName, DWORD def)
{
    return Reg::GetDwordAt(HKEY_CURRENT_USER, REG_APP, valueName, def);
}

static void RegSetDword(const wchar_t* valueName, DWORD v)
{
    Reg::SetDwordAt(HKEY_CURRENT_USER, REG_APP, valueName, v);
}

bool Settings::IsSnapToGrid()
{
    return RegGetDword(L"SnapToGrid", 0) != 0;
}

void Settings::SetSnapToGrid(bool enable)
{
    RegSetDword(L"SnapToGrid", enable ? 1 : 0);
}

bool Settings::IsAutoPruneDead()
{
    return RegGetDword(L"AutoPruneDead", 1) != 0;
}

void Settings::SetAutoPruneDead(bool enable)
{
    RegSetDword(L"AutoPruneDead", enable ? 1 : 0);
}

int Settings::GetDefaultGrid()
{
    int v = (int)RegGetDword(L"DefaultGrid", 2);
    if (v < 2 || v > 3) v = 2;
    return v;
}

void Settings::SetDefaultGrid(int grid)
{
    if (grid < 2) grid = 2;
    if (grid > 3) grid = 3;
    RegSetDword(L"DefaultGrid", (DWORD)grid);
}

DWORD Settings::PackHotkey(UINT mods, UINT vk)
{
    return ((vk & 0xFFFF) << 16) | (mods & 0xFFFF);
}

void Settings::UnpackHotkey(DWORD hotkey, UINT& mods, UINT& vk)
{
    mods = hotkey & 0xFFFF;
    vk = (hotkey >> 16) & 0xFFFF;
}

DWORD Settings::GetHotkeySearch()
{
    return RegGetDword(L"HotkeySearch",
        PackHotkey(MOD_CONTROL | MOD_SHIFT, VK_SPACE));
}

void Settings::SetHotkeySearch(DWORD hotkey)
{
    RegSetDword(L"HotkeySearch", hotkey);
}

DWORD Settings::GetHotkeyToggle()
{
    return RegGetDword(L"HotkeyToggle",
        PackHotkey(MOD_CONTROL | MOD_SHIFT, 'H'));
}

void Settings::SetHotkeyToggle(DWORD hotkey)
{
    RegSetDword(L"HotkeyToggle", hotkey);
}

bool Settings::IsShowOverflow()
{
    return RegGetDword(L"ShowOverflow", 1) != 0;
}

void Settings::SetShowOverflow(bool enable)
{
    RegSetDword(L"ShowOverflow", enable ? 1 : 0);
}

bool Settings::ConsumeFirstRun()
{
    if (RegGetDword(L"OnboardShown", 0) != 0)
        return false;
    RegSetDword(L"OnboardShown", 1);
    return true;
}

// ---------------------------------------------------------------------------
// Autostart
// ---------------------------------------------------------------------------

std::wstring Autostart::GetExePath()
{
    // MAX_PATH (260) мало для длинных путей: запрашиваем динамически.
    std::vector<WCHAR> buf(1024);
    for (;;)
    {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return L"";
        if (n < buf.size() - 1) return std::wstring(buf.data(), n);
        if (buf.size() >= 32768) return L"";
        buf.resize(buf.size() * 2);
    }
}

// Выделяет путь к exe из значения Run (там могут быть кавычки и аргументы:
// `"C:\path\app.exe" --flag`). Возвращает путь без кавычек и аргументов.
static std::wstring ExtractExeFromRunValue(const std::wstring& v)
{
    if (v.empty()) return L"";
    size_t i = 0;
    while (i < v.size() && (v[i] == L' ' || v[i] == L'\t')) i++;
    if (i >= v.size()) return L"";
    if (v[i] == L'"')
    {
        size_t end = v.find(L'"', i + 1);
        if (end == std::wstring::npos)
            return v.substr(i + 1);
        return v.substr(i + 1, end - i - 1);
    }
    size_t end = v.find_first_of(L" \t", i);
    return v.substr(i, end == std::wstring::npos ? end : end - i);
}

static bool IsStartupApprovedDisabled()
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_APPROVED_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false; // записи нет — запрета нет
    BYTE data[16] = {};
    DWORD cb = sizeof(data);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(hKey, REG_RUN_VALUE, nullptr, &type, data, &cb);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_BINARY || cb < 1)
        return false;
    return data[0] == 0x03; // 0x02 = разрешено, 0x03 = запрещено пользователем
}

static void ClearStartupApproved()
{
    // Удаляем запись согласия: без неё Explorer считает Run разрешённым.
    // Именно это чинит «тумблер в настройках ON, а Windows не запускает».
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_APPROVED_KEY, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(hKey, REG_RUN_VALUE);
    RegCloseKey(hKey);
}

bool Autostart::IsEnabled()
{
    std::wstring exe = GetExePath();
    if (exe.empty()) return false;

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    // Динамический буфер: при ERROR_MORE_DATA старое чтение возвращало false,
    // а Ensure() перезаписывал чужое/длинное значение своим.
    std::wstring val;
    {
        DWORD cb = 0;
        DWORD type = 0;
        LONG rc = RegQueryValueExW(hKey, REG_RUN_VALUE, nullptr, &type, nullptr, &cb);
        if (rc != ERROR_SUCCESS && rc != ERROR_MORE_DATA)
        {
            RegCloseKey(hKey);
            return false;
        }
        if (type != REG_SZ && type != REG_EXPAND_SZ)
        {
            RegCloseKey(hKey);
            return false;
        }
        if (cb < sizeof(wchar_t) || cb > 256 * 1024)
        {
            RegCloseKey(hKey);
            return false;
        }
        std::vector<BYTE> buf(cb);
        rc = RegQueryValueExW(hKey, REG_RUN_VALUE, nullptr, &type, buf.data(), &cb);
        RegCloseKey(hKey);
        if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
            return false;
        size_t chars = cb / sizeof(wchar_t);
        if (chars == 0) return false;
        // Гарантируем терминатор в пределах прочитанного.
        if (((wchar_t*)buf.data())[chars - 1] != L'\0')
        {
            if (cb % sizeof(wchar_t) != 0) return false;
            val.assign((wchar_t*)buf.data(), chars);
        }
        else
            val = (wchar_t*)buf.data();
    }

    std::wstring curExe = ExtractExeFromRunValue(val);
    if (curExe.empty() || _wcsicmp(curExe.c_str(), exe.c_str()) != 0)
        return false;

    // Значение в Run есть, но пользователь отключил нас в Диспетчере задач /
    // Параметрах > Автозагрузка — фактически автозапуска нет.
    if (IsStartupApprovedDisabled())
        return false;

    return true;
}

bool Autostart::SetEnabled(bool enable)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return false;

    bool ok = false;
    if (enable)
    {
        std::wstring exe = GetExePath();
        if (!exe.empty())
        {
            std::wstring quoted = L'"' + exe + L'"';
            ok = RegSetValueExW(hKey, REG_RUN_VALUE, 0, REG_SZ,
                (const BYTE*)quoted.c_str(),
                (DWORD)((quoted.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        }
    }
    else
    {
        LONG rc = RegDeleteValueW(hKey, REG_RUN_VALUE);
        ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(hKey);
    if (!ok)
        return false;

    // Сбрасываем запрет из Диспетчера задач, иначе Explorer проигнорирует Run.
    ClearStartupApproved();
    return true;
}

bool Autostart::IsDisabledByUser()
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_APP, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD v = 0;
    DWORD cb = sizeof(v);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(hKey, L"AutostartDisabled", nullptr, &type, (BYTE*)&v, &cb);
    RegCloseKey(hKey);
    return rc == ERROR_SUCCESS && type == REG_DWORD && v != 0;
}

void Autostart::SetDisabledByUser(bool disabled)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_APP, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;
    DWORD v = disabled ? 1 : 0;
    RegSetValueExW(hKey, L"AutostartDisabled", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(hKey);
}

void Autostart::Ensure()
{
    if (IsDisabledByUser())
        return;
    if (!IsEnabled())
        SetEnabled(true);
}

// ---------------------------------------------------------------------------
// Окно настроек в стиле «Параметров» Windows 11: акриловое стекло,
// скругление, тёмная тема из персонализации, Segoe UI Variable, тумблеры.
// Тексты и тумблеры рисуются родителем, кнопка — owner-drawn:
// никаких белых подложек и классических фокусных рамок.
// ---------------------------------------------------------------------------

struct SettingsRowUi {
    int settingId = 0;
    int section = 0; // 0 = Общие, 1 = Группы
    const wchar_t* title = nullptr;
    const wchar_t* desc = nullptr;
    bool checked = false;
    bool isChoice = false; // строка-список (кнопка со значениями), а не тумблер
    RECT rowRect = {};
    RECT toggleRect = {};
    RECT comboRect = {}; // кнопка списка (только для isChoice)
    RECT titleRect = {};
    RECT descRect = {};
};

struct SettingsWnd {
    HWND hWnd = nullptr;
    HWND hBtnClose = nullptr;
    std::vector<const wchar_t*> headerTexts;
    std::vector<RECT> headerRects;
    std::vector<SettingsRowUi> rows;
    HFONT hBody = nullptr;
    HFONT hSemibold = nullptr;
    HFONT hSection = nullptr;
    HFONT hCloseGlyph = nullptr; // Segoe MDL2 Assets, крестик заголовка
    UINT dpi = 96;
    bool dark = false;
    bool glass = false; // акрил применился
    COLORREF accent = RGB(0, 103, 192);
    bool btnHover = false;
    RECT titleRect = {};
    RECT closeRect = {};
    bool closeHover = false;
    bool closePressed = false;
    int hoverRow = -1;
    int pressedRow = -1;
    int focusedRow = -1; // строка под клавиатурным фокусом
    int capturingRow = -1; // строка захвата хоткея (ждём нажатие клавиш)
    int focusIdx = 4;    // индекс фокуса: строки + кнопка Закрыть в конце
    int clientW = 440;
    int clientH = 378;
    int sepY = 0;
};

// Стиль окна: без стандартного заголовка — свой стеклянный
// (перетаскивание за него, крестик справа). Таскбар и Alt+F4 — через SYSMENU.
static const DWORD kSetStyle = WS_POPUP | WS_SYSMENU | WS_CLIPCHILDREN;
static const DWORD kSetExStyle = WS_EX_APPWINDOW;
static const int kTitleH = 44;   // высота своего заголовка
static const int kCloseW = 46;   // ширина кнопки ✕
static const int kCloseH = 32;   // высота кнопки ✕
static const int kGlassAlpha = 175; // прозрачность акрила: больше стекла

static int Dpx(const SettingsWnd* st, int x) { return MulDiv(x, (int)st->dpi, 96); }

static COLORREF MixColors(COLORREF a, COLORREF b, int t) // t: 0..255 доля b
{
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t / 255,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t / 255,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t / 255);
}

static COLORREF ThemeText(bool dark) { return dark ? RGB(255, 255, 255) : RGB(27, 27, 27); }
static COLORREF ThemeDesc(bool dark) { return dark ? RGB(173, 173, 173) : RGB(96, 94, 92); }
static COLORREF ThemeSep(bool dark)  { return dark ? RGB(45, 45, 45) : RGB(225, 225, 225); }
static COLORREF ThemeBg(bool dark)   { return dark ? RGB(32, 32, 32) : RGB(243, 243, 243); }

static int CALLBACK FontEnumProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lParam)
{
    *(bool*)lParam = true;
    return 0;
}

static const wchar_t* UiFontFace()
{
    static int cached = -1;
    if (cached < 0)
    {
        bool found = false;
        HDC hdc = GetDC(nullptr);
        LOGFONTW lf = {};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Segoe UI Variable Text");
        EnumFontFamiliesExW(hdc, &lf, FontEnumProc, (LPARAM)&found, 0);
        ReleaseDC(nullptr, hdc);
        cached = found ? 1 : 0;
    }
    return cached ? L"Segoe UI Variable Text" : L"Segoe UI";
}

static HFONT MakeUiFont(UINT dpi, int pt, bool semibold, const wchar_t* face = nullptr)
{
    return CreateFontW(-MulDiv(pt, (int)dpi, 72), 0, 0, 0,
        semibold ? FW_SEMIBOLD : FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        face ? face : UiFontFace());
}

static COLORREF SystemAccent()
{
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque)))
        return RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
    return RGB(0, 103, 192);
}

static bool ReadRowValue(int id)
{
    switch (id)
    {
    case IDC_ROW_AUTOSTART: return Autostart::IsEnabled();
    case IDC_ROW_SNAP:      return Settings::IsSnapToGrid();
    case IDC_ROW_PRUNE:     return Settings::IsAutoPruneDead();
    case IDC_ROW_OVERFLOW:  return Settings::IsShowOverflow();
    // IDC_ROW_DEFGRID — строка-список, тумблера у неё нет.
    }
    return false;
}

static void ApplyRowValue(int id, bool on)
{
    switch (id)
    {
    case IDC_ROW_AUTOSTART:
        Autostart::SetDisabledByUser(!on);
        Autostart::SetEnabled(on);
        break;
    case IDC_ROW_SNAP:
        Settings::SetSnapToGrid(on);
        if (on && s_settingsManager)
            s_settingsManager->SnapAllWidgetsToGrid();
        break;
    case IDC_ROW_PRUNE:
        Settings::SetAutoPruneDead(on);
        break;
    case IDC_ROW_OVERFLOW:
        Settings::SetShowOverflow(on);
        if (s_settingsManager)
            s_settingsManager->RefreshWidgetIcons(); // перерисовать подписи
        break;
    // IDC_ROW_DEFGRID — строка-список, задаётся через OpenGridDropdown.
    }
}

// ---------------------------------------------------------------------------
// Строки хоткеев: текст кнопки, захват комбинации, уведомление main.
// ---------------------------------------------------------------------------

static bool IsHotkeyRow(int settingId)
{
    return settingId == IDC_ROW_HOTSEARCH || settingId == IDC_ROW_HOTTOGGLE;
}

// Допустимая «главная» клавиша хоткея (модификаторы отдельно).
static bool IsHotkeyMainKey(UINT vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return true;
    if (vk >= VK_F1 && vk <= VK_F24)
        return true;
    switch (vk)
    {
    case VK_SPACE:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
        return true;
    default:
        return false;
    }
}

// "Ctrl+Shift+Space". Пишет в out, возвращает out.
static const wchar_t* FormatHotkey(UINT mods, UINT vk, wchar_t* out, int outLen)
{
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_SHIFT) s += L"Shift+";
    if (mods & MOD_WIN) s += L"Win+";
    wchar_t key[16] = {};
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
    {
        key[0] = (wchar_t)vk;
    }
    else if (vk >= VK_F1 && vk <= VK_F24)
    {
        swprintf_s(key, L"F%d", (int)(vk - VK_F1 + 1));
    }
    else switch (vk)
    {
    case VK_SPACE:  wcscpy_s(key, L"Space"); break;
    case VK_INSERT: wcscpy_s(key, L"Ins"); break;
    case VK_DELETE: wcscpy_s(key, L"Del"); break;
    case VK_HOME:   wcscpy_s(key, L"Home"); break;
    case VK_END:    wcscpy_s(key, L"End"); break;
    case VK_PRIOR:  wcscpy_s(key, L"PgUp"); break;
    case VK_NEXT:   wcscpy_s(key, L"PgDn"); break;
    case VK_LEFT:   wcscpy_s(key, L"Left"); break;
    case VK_UP:     wcscpy_s(key, L"Up"); break;
    case VK_RIGHT:  wcscpy_s(key, L"Right"); break;
    case VK_DOWN:   wcscpy_s(key, L"Down"); break;
    default:        wcscpy_s(key, L"?"); break;
    }
    s += key;
    wcsncpy_s(out, outLen, s.c_str(), _TRUNCATE);
    return out;
}

static DWORD HotkeySettingValue(int settingId)
{
    if (settingId == IDC_ROW_HOTSEARCH) return Settings::GetHotkeySearch();
    return Settings::GetHotkeyToggle();
}

static void SaveHotkeySetting(int settingId, DWORD v)
{
    if (settingId == IDC_ROW_HOTSEARCH) Settings::SetHotkeySearch(v);
    else Settings::SetHotkeyToggle(v);
}

// Просим живой процесс перечитать хоткеи (IPC-окно знает main).
static void NotifyHotkeysChanged()
{
    HWND hIpc = FindWindowW(kGroupManagerIpcClass, nullptr);
    if (hIpc)
        PostMessageW(hIpc, WM_USER + 205 /*IPC_APPLY_HOTKEYS*/, 0, 0);
}

// Обработка нажатия в режиме захвата. Возвращает true, если клавиша
// поглощена (захват продолжается или завершён).
static bool CaptureHotkey(SettingsWnd* st, WPARAM vk)
{
    if (!st || st->capturingRow < 0 || st->capturingRow >= (int)st->rows.size())
        return false;
    int row = st->capturingRow;
    if (vk == VK_ESCAPE)
    {
        st->capturingRow = -1;
        InvalidateRect(st->hWnd, nullptr, TRUE);
        return true;
    }
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
        vk == VK_LWIN || vk == VK_RWIN)
        return true; // голые модификаторы — ждём главную клавишу
    if (!IsHotkeyMainKey((UINT)vk))
        return true; // неподходящая — ждём дальше, захват не срываем
    UINT mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
        mods |= MOD_WIN;
    if ((mods & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) == 0)
        return true; // без модификаторов нельзя — ждём дальше
    SaveHotkeySetting(st->rows[row].settingId,
        Settings::PackHotkey(mods, (UINT)vk));
    st->capturingRow = -1;
    NotifyHotkeysChanged();
    InvalidateRect(st->hWnd, nullptr, TRUE);
    return true;
}

static void ApplyFluentStyle(SettingsWnd* st)
{    st->dark = ModernMenuIsDarkMode();
    st->glass = false;
    HMODULE hDwm = GetModuleHandleW(L"dwmapi.dll");
    if (hDwm)
    {
        using FnAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto fn = (FnAttr)GetProcAddress(hDwm, "DwmSetWindowAttribute");
        if (fn)
        {
            int corner = DWMWCP_ROUND;
            fn(st->hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
            BOOL dark = st->dark ? TRUE : FALSE;
            fn(st->hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        }
    }
    // Акрил вместо системного Mica: тинт задаём сами в цвет темы,
    // поэтому фон всегда совпадает с текстом (Mica на части сборок
    // рисуется светлым даже в тёмной теме). Заливка поверх акрила —
    // в WM_ERASEBKGND.
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser)
    {
        using FnComp = BOOL(WINAPI*)(HWND, void*);
        auto fnComp = (FnComp)GetProcAddress(hUser, "SetWindowCompositionAttribute");
        if (fnComp)
        {
            struct AccentPolicy { int State, Flags, GradientColor, AnimId; };
            struct CompAttrData { int Attr; void* Data; int Size; };
            COLORREF tint = ThemeBg(st->dark);
            int argb = (kGlassAlpha << 24) |
                (GetRValue(tint) | (GetGValue(tint) << 8) | (GetBValue(tint) << 16));
            AccentPolicy ap = { 4 /*ACRYLICBLURBEHIND*/, 2, argb, 0 };
            CompAttrData cd = { 19 /*WCA_ACCENT_POLICY*/, &ap, sizeof(ap) };
            st->glass = (fnComp(st->hWnd, &cd) == TRUE);
        }
    }
    st->accent = SystemAccent();
}

static void ApplyRowFonts(SettingsWnd* st)
{
    if (st->hBtnClose)
        SendMessageW(st->hBtnClose, WM_SETFONT, (WPARAM)st->hBody, TRUE);
}

static void PlaceRow(SettingsWnd* st, size_t i, int pad, int y, int W)
{
    SettingsRowUi& r = st->rows[i];
    int rh = Dpx(st, 72);
    r.rowRect = { pad, y, W - pad, y + rh };
    if (r.isChoice)
    {
        // Кнопка списка справа вместо тумблера.
        int bw = Dpx(st, 120), bh = Dpx(st, 32);
        r.comboRect = { W - pad - bw, y + (rh - bh) / 2, W - pad, y + (rh - bh) / 2 + bh };
        int textW = W - 2 * pad - bw - Dpx(st, 12);
        r.titleRect = { pad, y + Dpx(st, 8), pad + textW, y + Dpx(st, 8) + Dpx(st, 22) };
        r.descRect = { pad, y + Dpx(st, 32), pad + textW, y + Dpx(st, 32) + Dpx(st, 36) };
        return;
    }
    int tw = Dpx(st, 46), th = Dpx(st, 24);
    r.toggleRect = { W - pad - tw, y + (rh - th) / 2, W - pad, y + (rh - th) / 2 + th };
    int textW = W - 2 * pad - tw - Dpx(st, 12);
    r.titleRect = { pad, y + Dpx(st, 8), pad + textW, y + Dpx(st, 8) + Dpx(st, 22) };
    r.descRect = { pad, y + Dpx(st, 32), pad + textW, y + Dpx(st, 32) + Dpx(st, 36) };
}

static void PlaceHeader(SettingsWnd* st, size_t i, int pad, int y, int W)
{
    st->headerRects[i] = { pad, y + Dpx(st, 2), W - pad, y + Dpx(st, 2) + Dpx(st, 22) };
}

static void LayoutSettings(SettingsWnd* st)
{
    int W = Dpx(st, 440), pad = Dpx(st, 20);
    // Свой стеклянный заголовок сверху: текст слева, крестик справа.
    st->titleRect = { Dpx(st, 16), 0, W - Dpx(st, 60), Dpx(st, kTitleH) };
    st->closeRect = { W - Dpx(st, kCloseW), 0, W, Dpx(st, kCloseH) };
    int y = Dpx(st, kTitleH) + Dpx(st, 4);
    int section = -1;
    for (size_t i = 0; i < st->rows.size(); i++)
    {
        if (st->rows[i].section != section)
        {
            section = st->rows[i].section;
            if (i > 0) y += Dpx(st, 6);
            PlaceHeader(st, (size_t)section, pad, y, W);
            y += Dpx(st, 30);
        }
        PlaceRow(st, i, pad, y, W); y += Dpx(st, 72);
    }
    y += Dpx(st, 10);
    st->sepY = y;
    y += Dpx(st, 14);
    SetWindowPos(st->hBtnClose, nullptr, W - pad - Dpx(st, 110), y, Dpx(st, 110), Dpx(st, 32),
        SWP_NOZORDER | SWP_NOACTIVATE);
    y += Dpx(st, 32);
    y += pad;
    st->clientW = W;
    st->clientH = y;
}

static void FitWindowToContent(SettingsWnd* st)
{
    RECT rc = { 0, 0, st->clientW, st->clientH };
    AdjustWindowRectEx(&rc, kSetStyle, FALSE, kSetExStyle);
    SetWindowPos(st->hWnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void PaintToggle(HDC hdc, RECT rc, bool on, bool hover, bool pressed,
    bool focused, bool dark, COLORREF accent, UINT dpi)
{
    int h = rc.bottom - rc.top;
    int edgeW = dpi >= 192 ? 2 : 1;
    COLORREF fill = accent, edge = accent, knob = RGB(255, 255, 255);
    if (on)
    {
        if (hover) fill = MixColors(fill, RGB(255, 255, 255), 30);
        if (pressed) { fill = MixColors(accent, RGB(0, 0, 0), 35); }
        edge = fill;
    }
    else
    {
        edge = dark ? RGB(157, 157, 157) : RGB(110, 110, 110);
        if (hover) edge = dark ? RGB(205, 205, 205) : RGB(60, 60, 60);
        knob = edge;
    }

    if (on)
    {
        HBRUSH br = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, edgeW, edge);
        HGDIOBJ ob = SelectObject(hdc, br);
        HGDIOBJ op = SelectObject(hdc, pen);
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, h, h);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(br);
        DeleteObject(pen);
    }
    else
    {
        HPEN pen = CreatePen(PS_SOLID, edgeW, edge);
        HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        HGDIOBJ op = SelectObject(hdc, pen);
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, h, h);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(pen);
    }

    int kd = h - MulDiv(11, (int)dpi, 96);
    if (kd < 8) kd = 8;
    int m = MulDiv(6, (int)dpi, 96);
    int kx = on ? rc.right - m - kd : rc.left + m;
    int ky = rc.top + (h - kd) / 2;
    HBRUSH kbr = CreateSolidBrush(knob);
    HPEN kpen = CreatePen(PS_SOLID, 1, knob);
    HGDIOBJ ob = SelectObject(hdc, kbr);
    HGDIOBJ op = SelectObject(hdc, kpen);
    Ellipse(hdc, kx, ky, kx + kd, ky + kd);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(kbr);
    DeleteObject(kpen);

    if (focused)
    {
        HPEN fpen = CreatePen(PS_SOLID, 2, accent);
        HGDIOBJ fb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        HGDIOBJ fp = SelectObject(hdc, fpen);
        int f = MulDiv(3, (int)dpi, 96);
        RoundRect(hdc, rc.left - f, rc.top - f, rc.right + f, rc.bottom + f, h + 6, h + 6);
        SelectObject(hdc, fb);
        SelectObject(hdc, fp);
        DeleteObject(fpen);
    }
}

static int HitPill(SettingsWnd* st, POINT pt)
{
    for (size_t i = 0; i < st->rows.size(); i++)
    {
        if (st->rows[i].isChoice) continue; // у списка нет тумблера
        RECT r = st->rows[i].toggleRect;
        InflateRect(&r, Dpx(st, 5), Dpx(st, 6));
        if (PtInRect(&r, pt)) return (int)i;
    }
    return -1;
}

static int HitCombo(SettingsWnd* st, POINT pt)
{
    for (size_t i = 0; i < st->rows.size(); i++)
        if (st->rows[i].isChoice && PtInRect(&st->rows[i].comboRect, pt))
            return (int)i;
    return -1;
}

static int HitRow(SettingsWnd* st, POINT pt)
{
    for (size_t i = 0; i < st->rows.size(); i++)
        if (PtInRect(&st->rows[i].rowRect, pt)) return (int)i;
    return -1;
}

static void ToggleRow(SettingsWnd* st, int i)
{
    if (i < 0 || i >= (int)st->rows.size()) return;
    if (st->rows[i].isChoice) return; // список переключается через OpenGridDropdown
    st->rows[i].checked = !st->rows[i].checked;
    ApplyRowValue(st->rows[i].settingId, st->rows[i].checked);
    InvalidateRect(st->hWnd, nullptr, FALSE);
}

// Текст кнопки списка для строки.
static const wchar_t* ChoiceValueText(const SettingsRowUi& r)
{
    if (IsHotkeyRow(r.settingId))
    {
        // Хоткеи-строки рисуются через HotkeyButtonText (нужен режим захвата).
        return L"";
    }
    if (r.settingId == IDC_ROW_LANG)
    {
        switch (Lang::GetOverride())
        {
        case LangId::Russian: return Lang::Get(Str::S_LangRussian);
        case LangId::English: return Lang::Get(Str::S_LangEnglish);
        default: break;
        }
        return Lang::Get(Str::S_LangAuto);
    }
    return Settings::GetDefaultGrid() == 3 ? L"3×3" : L"2×2";
}

static void RefreshLanguage(SettingsWnd* st)
{
    if (!st || !st->hWnd) return;
    // Тексты хранятся указателями на литералы — перепривязываем к новому языку.
    for (auto& r : st->rows)
    {
        switch (r.settingId)
        {
        case IDC_ROW_AUTOSTART:
            r.title = Lang::Get(Str::S_Autostart);
            r.desc = Lang::Get(Str::S_AutostartDesc);
            break;
        case IDC_ROW_SNAP:
            r.title = Lang::Get(Str::S_Snap);
            r.desc = Lang::Get(Str::S_SnapDesc);
            break;
        case IDC_ROW_HOTSEARCH:
            r.title = Lang::Get(Str::S_HotSearch);
            r.desc = Lang::Get(Str::S_HotSearchDesc);
            break;
        case IDC_ROW_HOTTOGGLE:
            r.title = Lang::Get(Str::S_HotToggle);
            r.desc = Lang::Get(Str::S_HotToggleDesc);
            break;
        case IDC_ROW_LANG:
            r.title = Lang::Get(Str::S_Language);
            r.desc = Lang::Get(Str::S_LanguageDesc);
            break;
        case IDC_ROW_PRUNE:
            r.title = Lang::Get(Str::S_Prune);
            r.desc = Lang::Get(Str::S_PruneDesc);
            break;
        case IDC_ROW_DEFGRID:
            r.title = Lang::Get(Str::S_DefGrid);
            r.desc = Lang::Get(Str::S_DefGridDesc);
            break;
        case IDC_ROW_OVERFLOW:
            r.title = Lang::Get(Str::S_Overflow);
            r.desc = Lang::Get(Str::S_OverflowDesc);
            break;
        }
    }
    st->headerTexts = { Lang::Get(Str::S_General), Lang::Get(Str::S_Groups) };
    SetWindowTextW(st->hWnd, Lang::Get(Str::S_Title));
    if (st->hBtnClose)
        SetWindowTextW(st->hBtnClose, Lang::Get(Str::S_Close));
    // Со стиранием: тексты рисуются прозрачным GDI поверх старых пикселей,
    // без erase новый язык ляжет поверх старого (каша как на скриншоте).
    InvalidateRect(st->hWnd, nullptr, TRUE);
}

// Выпадающий список строки (наш ModernMenu — в теме системы).
static void OpenChoiceDropdown(SettingsWnd* st, int rowIndex)
{
    if (!st || !st->hWnd || !s_settingsManager) return;
    if (rowIndex < 0 || rowIndex >= (int)st->rows.size()) return;
    if (!st->rows[rowIndex].isChoice) return;
    WidgetRenderer* renderer = s_settingsManager->GetRenderer();
    if (!renderer) return;
    const SettingsRowUi& row = st->rows[rowIndex];

    ModernMenu menu(renderer);
    if (row.settingId == IDC_ROW_LANG)
    {
        LangId cur = Lang::GetOverride();
        menu.items.push_back({ Lang::Get(Str::S_LangAuto), 10, cur == LangId::Auto });
        menu.items.push_back({ Lang::Get(Str::S_LangRussian), 11, cur == LangId::Russian });
        menu.items.push_back({ Lang::Get(Str::S_LangEnglish), 12, cur == LangId::English });
    }
    else
    {
        int cur = Settings::GetDefaultGrid();
        menu.items.push_back({ L"2×2", 2, cur == 2 });
        menu.items.push_back({ L"3×3", 3, cur == 3 });
    }

    POINT pt = { row.comboRect.left, row.comboRect.bottom + 4 };
    ClientToScreen(st->hWnd, &pt);
    int cmd = menu.Show(pt.x, pt.y);
    if (row.settingId == IDC_ROW_LANG)
    {
        if (cmd == 10) Lang::SetOverride(LangId::Auto);
        else if (cmd == 11) Lang::SetOverride(LangId::Russian);
        else if (cmd == 12) Lang::SetOverride(LangId::English);
        else return;
        RefreshLanguage(st); // сразу, без перезапуска
        return;
    }
    if (cmd == 2 || cmd == 3)
    {
        Settings::SetDefaultGrid(cmd);
        // Со стиранием: значение-кнопка тоже текст (см. RefreshLanguage).
        InvalidateRect(st->hWnd, nullptr, TRUE);
    }
}

static void ActivateRow(SettingsWnd* st, int i)
{
    if (i < 0 || i >= (int)st->rows.size()) return;
    if (IsHotkeyRow(st->rows[i].settingId))
    {
        // Клик/Enter по строке хоткея — начать захват комбинации.
        st->capturingRow = i;
        InvalidateRect(st->hWnd, nullptr, TRUE);
        return;
    }
    if (st->rows[i].isChoice) { OpenChoiceDropdown(st, i); return; }
    ToggleRow(st, i);
}

// Ховер кнопки «Закрыть» (для owner-draw): состояние храним в SettingsWnd.
static LRESULT CALLBACK BtnSubclassProc(HWND h, UINT m, WPARAM w, LPARAM l,
    UINT_PTR, DWORD_PTR ref)
{
    SettingsWnd* st = (SettingsWnd*)ref;
    switch (m)
    {
    case WM_MOUSEMOVE:
        if (st && !st->btnHover)
        {
            st->btnHover = true;
            InvalidateRect(h, nullptr, FALSE);
        }
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (st && st->btnHover)
        {
            st->btnHover = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, BtnSubclassProc, 1);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

// Кнопка в стиле Win11 primary: акцентная заливка, белый текст,
// светлое кольцо фокуса вместо классического пунктира.
static void PaintFluentButton(const DRAWITEMSTRUCT* di, SettingsWnd* st)
{
    HDC hdc = di->hDC;
    RECT rc = di->rcItem;
    bool pressed = (di->itemState & ODS_SELECTED) != 0;
    bool focused = (di->itemState & ODS_FOCUS) != 0;
    bool hover = st->btnHover || pressed;

    COLORREF fill = st->accent;
    if (pressed)
        fill = MixColors(st->accent, RGB(0, 0, 0), 45);
    else if (hover)
        fill = MixColors(st->accent, RGB(255, 255, 255), 35);
    COLORREF edge = MixColors(st->accent, RGB(0, 0, 0), 55);

    int diam = Dpx(st, 8);
    // Углы вне скругления заливаем фоном окна, иначе там остаётся белое.
    {
        HBRUSH bg = CreateSolidBrush(ThemeBg(st->dark));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
    }
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, diam, diam);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);

    if (focused)
    {
        HPEN fpen = CreatePen(PS_SOLID, 2, MixColors(st->accent, RGB(255, 255, 255), 120));
        HGDIOBJ fb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        HGDIOBJ fp = SelectObject(hdc, fpen);
        RoundRect(hdc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1, diam, diam);
        SelectObject(hdc, fb);
        SelectObject(hdc, fp);
        DeleteObject(fpen);
    }

    WCHAR text[64] = {};
    GetWindowTextW(di->hwndItem, text, _countof(text));
    HGDIOBJ of = SelectObject(hdc, st->hBody);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, text, -1, const_cast<RECT*>(&rc),
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, of);
}

// Кнопка списка (выбор размера сетки) в стиле Win11: заливка/рамка под тему,
// текущее значение слева, стрелка справа, акцентное кольцо фокуса.
static void PaintComboButton(HDC hdc, const SettingsRowUi& r, const wchar_t* value,
    bool hover, bool pressed, bool focused, bool dark, COLORREF accent, HFONT hFont)
{
    RECT rc = r.comboRect;
    COLORREF fill = dark ? RGB(45, 45, 45) : RGB(255, 255, 255);
    COLORREF edge = dark ? RGB(63, 63, 63) : RGB(229, 229, 229);
    if (pressed)
        fill = dark ? RGB(40, 40, 40) : MixColors(RGB(255, 255, 255), RGB(0, 0, 0), 22);
    else if (hover)
        fill = dark ? RGB(56, 56, 56) : MixColors(RGB(255, 255, 255), RGB(0, 0, 0), 10);

    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);

    if (focused)
    {
        HPEN fpen = CreatePen(PS_SOLID, 2, MixColors(accent, RGB(255, 255, 255), 120));
        HGDIOBJ fb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        HGDIOBJ fp = SelectObject(hdc, fpen);
        RoundRect(hdc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1, 8, 8);
        SelectObject(hdc, fb);
        SelectObject(hdc, fp);
        DeleteObject(fpen);
    }

    HGDIOBJ of = SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ThemeText(dark));
    RECT tr = rc;
    tr.left += 12;
    tr.right -= 28;
    DrawTextW(hdc, value, -1, &tr,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    // Стрелка-треугольник справа.
    COLORREF gc = ThemeDesc(dark);
    HBRUSH gbr = CreateSolidBrush(gc);
    HPEN gpen = CreatePen(PS_SOLID, 1, gc);
    HGDIOBJ gob = SelectObject(hdc, gbr);
    HGDIOBJ gop = SelectObject(hdc, gpen);
    int cx = rc.right - 14, cy = (rc.top + rc.bottom) / 2;
    POINT pts[3] = { { cx - 4, cy - 2 }, { cx + 4, cy - 2 }, { cx, cy + 3 } };
    Polygon(hdc, pts, 3);
    SelectObject(hdc, gob);
    SelectObject(hdc, gop);
    DeleteObject(gbr);
    DeleteObject(gpen);
    SelectObject(hdc, of);
}

static LRESULT CALLBACK SettingsProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    SettingsWnd* st = (SettingsWnd*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);

    switch (uMsg)
    {
    case WM_CREATE:
    {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        st = (SettingsWnd*)cs->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)st);
        st->hWnd = hWnd;
        st->dpi = GetDpiForWindowCompat(hWnd);
        if (!st->dpi) st->dpi = 96;
        st->hBody = MakeUiFont(st->dpi, 9, false);
        st->hSemibold = MakeUiFont(st->dpi, 9, true);
        st->hSection = MakeUiFont(st->dpi, 11, true);
        st->hCloseGlyph = MakeUiFont(st->dpi, 10, false, L"Segoe MDL2 Assets");
        ApplyFluentStyle(st);
        {
            // Иконка приложения для таскбара/Alt+Tab (ресурсная, не дефолтная).
            HICON hAppIcon = LoadIconW(GetModuleHandleW(nullptr),
                MAKEINTRESOURCEW(IDI_APP_ICON));
            if (hAppIcon)
            {
                SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hAppIcon);
                SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hAppIcon);
            }
        }

        HINSTANCE hInst = GetModuleHandleW(nullptr);
        // Тексты рисуем сами в WM_PAINT (DrawText): STATIC-контролы на части
        // систем красят под собой белую подложку, игнорируя WM_CTLCOLORSTATIC.
        st->headerTexts = { Lang::Get(Str::S_General), Lang::Get(Str::S_Groups) };
        st->headerRects.resize(2);
        st->hBtnClose = CreateWindowExW(0, L"BUTTON", Lang::Get(Str::S_Close),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_OWNERDRAW,
            0, 0, 10, 10, hWnd, (HMENU)(INT_PTR)IDC_BTN_CLOSE, hInst, nullptr);
        SetWindowSubclass(st->hBtnClose, BtnSubclassProc, 1, (DWORD_PTR)st);

        ApplyRowFonts(st);
        LayoutSettings(st);
        FitWindowToContent(st);
        return 0;
    }

    case WM_DPICHANGED:
    {
        if (!st) break;
        st->dpi = LOWORD(wParam);
        if (!st->dpi) st->dpi = 96;
        if (st->hBody) DeleteObject(st->hBody);
        if (st->hSemibold) DeleteObject(st->hSemibold);
        if (st->hSection) DeleteObject(st->hSection);
        if (st->hCloseGlyph) DeleteObject(st->hCloseGlyph);
        st->hBody = MakeUiFont(st->dpi, 9, false);
        st->hSemibold = MakeUiFont(st->dpi, 9, true);
        st->hSection = MakeUiFont(st->dpi, 11, true);
        st->hCloseGlyph = MakeUiFont(st->dpi, 10, false, L"Segoe MDL2 Assets");
        ApplyRowFonts(st);
        LayoutSettings(st);
        RECT* suggested = (RECT*)lParam;
        RECT rc = { 0, 0, st->clientW, st->clientH };
        AdjustWindowRectEx(&rc, kSetStyle, FALSE, kSetExStyle);
        SetWindowPos(hWnd, nullptr, suggested->left, suggested->top,
            rc.right - rc.left, rc.bottom - rc.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_ERASEBKGND:
    {
        if (!st) break;
        // Заливка всегда своя, в цвет темы: см. комментарий в ApplyFluentStyle.
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hWnd, &rc);
        HBRUSH br = CreateSolidBrush(ThemeBg(st->dark));
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        return 1;
    }

    case WM_PAINT:
    {
        if (!st) break;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        SetBkMode(hdc, TRANSPARENT);
        // Заголовки секций
        HGDIOBJ oldFont = SelectObject(hdc, st->hSection);
        SetTextColor(hdc, ThemeText(st->dark));
        for (size_t i = 0; i < st->headerTexts.size() && i < st->headerRects.size(); i++)
            DrawTextW(hdc, st->headerTexts[i], -1, &st->headerRects[i],
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        // Названия и описания настроек
        for (size_t i = 0; i < st->rows.size(); i++)
        {
            const auto& r = st->rows[i];
            SelectObject(hdc, st->hSemibold);
            SetTextColor(hdc, ThemeText(st->dark));
            DrawTextW(hdc, r.title, -1, const_cast<RECT*>(&r.titleRect),
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(hdc, st->hBody);
            SetTextColor(hdc, ThemeDesc(st->dark));
            DrawTextW(hdc, r.desc, -1, const_cast<RECT*>(&r.descRect),
                DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        }
        SelectObject(hdc, oldFont);
        // Свой стеклянный заголовок: текст слева, крестик справа.
        // Фон не красим — там то же стекло, что и у всего окна.
        SelectObject(hdc, st->hSemibold);
        SetTextColor(hdc, ThemeText(st->dark));
        DrawTextW(hdc, Lang::Get(Str::S_Title), -1, &st->titleRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (st->closeHover || st->closePressed)
        {
            COLORREF cb = st->closePressed ? RGB(139, 30, 20) : RGB(196, 43, 28);
            HBRUSH cbr = CreateSolidBrush(cb);
            FillRect(hdc, &st->closeRect, cbr);
            DeleteObject(cbr);
        }
        SelectObject(hdc, st->hCloseGlyph);
        SetTextColor(hdc, (st->closeHover || st->closePressed)
            ? RGB(255, 255, 255) : ThemeText(st->dark));
        DrawTextW(hdc, L"\uE711", -1, const_cast<RECT*>(&st->closeRect),
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, oldFont);
        // Разделитель над футером
        HPEN pen = CreatePen(PS_SOLID, 1, ThemeSep(st->dark));
        HGDIOBJ op = SelectObject(hdc, pen);
        MoveToEx(hdc, Dpx(st, 20), st->sepY, nullptr);
        LineTo(hdc, st->clientW - Dpx(st, 20), st->sepY);
        SelectObject(hdc, op);
        DeleteObject(pen);
        // Тумблеры и кнопки списков
        for (size_t i = 0; i < st->rows.size(); i++)
        {
            const auto& r = st->rows[i];
            if (r.isChoice)
            {
                const wchar_t* value = ChoiceValueText(r);
                // Хоткей: в захвате — подсказка, иначе текущая комбинация.
                wchar_t hotBuf[48] = {};
                if (IsHotkeyRow(r.settingId))
                {
                    if (st->capturingRow == (int)i)
                        value = Lang::Get(Str::S_HotPress);
                    else
                    {
                        UINT mods = 0, vk = 0;
                        Settings::UnpackHotkey(HotkeySettingValue(r.settingId), mods, vk);
                        value = FormatHotkey(mods, vk, hotBuf, _countof(hotBuf));
                    }
                }
                PaintComboButton(hdc, r, value,
                    st->hoverRow == (int)i, st->pressedRow == (int)i,
                    st->focusedRow == (int)i || st->capturingRow == (int)i,
                    st->dark, st->accent, st->hBody);
                continue;
            }
            PaintToggle(hdc, r.toggleRect, r.checked,
                st->hoverRow == (int)i, st->pressedRow == (int)i,
                st->focusedRow == (int)i, st->dark, st->accent, st->dpi);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_NCHITTEST:
    {
        if (!st) break;
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hWnd, &pt);
        if (PtInRect(&st->closeRect, pt)) return HTCLIENT; // крестик — кликаем сами
        if (pt.y < Dpx(st, kTitleH)) return HTCAPTION;     // шапка — перетаскивание
        return HTCLIENT;
    }

    case WM_MOUSEMOVE:
    {
        if (!st) break;
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        bool onClose = PtInRect(&st->closeRect, pt) != FALSE;
        if (onClose != st->closeHover && st->pressedRow < 0)
        {
            st->closeHover = onClose;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (st->pressedRow >= 0)
        {
            // Тянем с зажатой кнопкой — подсветка не нужна, выходим
            break;
        }
        int hitC = HitCombo(st, pt);
        int hit = (hitC >= 0) ? hitC : HitPill(st, pt);
        if (hit != st->hoverRow)
        {
            st->hoverRow = hit;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (st && (st->hoverRow != -1 || st->closeHover) && st->pressedRow < 0)
        {
            st->hoverRow = -1;
            st->closeHover = false;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        if (!st) break;
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        // Клик мимо захватываемой кнопки — отмена захвата (дальше обычный клик).
        if (st->capturingRow >= 0 && HitCombo(st, pt) != st->capturingRow)
        {
            st->capturingRow = -1;
            InvalidateRect(hWnd, nullptr, TRUE);
        }
        if (PtInRect(&st->closeRect, pt))
        {
            st->closePressed = true;
            SetCapture(hWnd);
            SetFocus(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        int hitC = HitCombo(st, pt);
        int hit = (hitC >= 0) ? hitC : HitPill(st, pt);
        if (hit < 0) hit = HitRow(st, pt); // клик по тексту — только фокус
        if (hit >= 0)
        {
            st->focusedRow = hit;
            if (HitCombo(st, pt) >= 0 || HitPill(st, pt) >= 0)
            {
                st->pressedRow = hit;
                SetCapture(hWnd);
            }
            SetFocus(hWnd);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        if (!st) break;
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        if (st->closePressed)
        {
            st->closePressed = false;
            if (GetCapture() == hWnd) ReleaseCapture();
            if (PtInRect(&st->closeRect, pt))
            {
                DestroyWindow(hWnd);
                return 0;
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        int pressed = st->pressedRow;
        st->pressedRow = -1;
        if (GetCapture() == hWnd) ReleaseCapture();
        if (pressed >= 0 && pressed < (int)st->rows.size() &&
            st->rows[pressed].isChoice && HitCombo(st, pt) == pressed)
        {
            if (IsHotkeyRow(st->rows[pressed].settingId))
            {
                st->capturingRow = pressed;
                InvalidateRect(hWnd, nullptr, TRUE);
            }
            else
                OpenChoiceDropdown(st, pressed);
        }
        else if (pressed >= 0 && HitPill(st, pt) == pressed)
            ToggleRow(st, pressed);
        else
            InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN:
    {
        if (!st) break;
        if (GetFocus() == st->hBtnClose) break; // кнопке — стандартное поведение
        switch (wParam)
        {
        case VK_UP:
            st->focusedRow = (st->focusedRow <= 0) ? (int)st->rows.size() - 1 : st->focusedRow - 1;
            st->focusIdx = st->focusedRow;
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        case VK_DOWN:
            st->focusedRow = (st->focusedRow < 0 || st->focusedRow >= (int)st->rows.size() - 1)
                ? 0 : st->focusedRow + 1;
            st->focusIdx = st->focusedRow;
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        case VK_SPACE:
        case VK_RETURN:
            if (st->focusedRow >= 0)
            {
                ActivateRow(st, st->focusedRow);
                return 0;
            }
            break;
        }
        break;
    }

    case WM_ACTIVATE:
        if (st && LOWORD(wParam) == WA_INACTIVE && st->capturingRow >= 0)
        {
            // Фокус ушёл посреди захвата — отменяем, чтобы не залипнуть.
            st->capturingRow = -1;
            InvalidateRect(hWnd, nullptr, TRUE);
        }
        break;

    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDC_BTN_CLOSE)
        {
            DestroyWindow(hWnd);
            return 0;
        }
        return 0;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lParam;
        if (di && di->CtlID == (UINT)IDC_BTN_CLOSE && st)
        {
            PaintFluentButton(di, st);
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        // БЕЗ PostQuitMessage: цикл вложенный модальный, а WM_QUIT отравил бы
        // все внешние циклы вплоть до главного (тихое завершение приложения).
        // Выход из цикла — по IsWindow ниже; будим его dummy-сообщением,
        // иначе GetMessage заблокируется навсегда на мёртвом окне.
        PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
        return 0;

    case WM_NCDESTROY:
        if (st)
        {
            if (st->hBody) DeleteObject(st->hBody);
            if (st->hSemibold) DeleteObject(st->hSemibold);
            if (st->hSection) DeleteObject(st->hSection);
            if (st->hCloseGlyph) DeleteObject(st->hCloseGlyph);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
            delete st;
        }
        if (hWnd == s_hSettingsWnd) s_hSettingsWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static void CycleSettingsFocus(SettingsWnd* st, bool backward)
{
    if (!st) return;
    int n = (int)st->rows.size() + 1; // строки + кнопка
    int cur = (GetFocus() == st->hBtnClose) ? (int)st->rows.size() : st->focusedRow;
    if (cur < 0) cur = backward ? 0 : n - 1;
    int next = backward ? (cur - 1 + n) % n : (cur + 1) % n;
    st->focusIdx = next;
    if (next >= (int)st->rows.size())
    {
        st->focusedRow = -1;
        SetFocus(st->hBtnClose);
    }
    else
    {
        st->focusedRow = next;
        SetFocus(st->hWnd);
    }
    InvalidateRect(st->hWnd, nullptr, FALSE);
}

// Поднять окно настроек: рывок наверх без залипания в топмосте,
// затем честная попытка фокуса; не дался — мигаем таскбаром.
static void BringSettingsToFront(HWND hWnd)
{
    if (!IsWindow(hWnd)) return;
    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // Простого SetForegroundWindow мало — foreground lock гасит его
    // для фонового процесса, поэтому временно связываем ввод.
    HWND hFore = GetForegroundWindow();
    if (hFore && hFore != hWnd)
    {
        DWORD foreTid = GetWindowThreadProcessId(hFore, nullptr);
        DWORD thisTid = GetCurrentThreadId();
        BOOL attached = FALSE;
        if (foreTid && foreTid != thisTid)
            attached = AttachThreadInput(thisTid, foreTid, TRUE);
        SetForegroundWindow(hWnd);
        SetFocus(hWnd);
        if (attached)
            AttachThreadInput(thisTid, foreTid, FALSE);
    }
    else
    {
        SetForegroundWindow(hWnd);
        SetFocus(hWnd);
    }
    if (GetForegroundWindow() != hWnd)
    {
        // Фокус не дался — мигаем кнопкой в таскбаре, как все приложения.
        FLASHWINFO fi = { sizeof(fi), hWnd,
            FLASHW_ALL | FLASHW_TIMERNOFG, 3, 0 };
        FlashWindowEx(&fi);
    }
    // Фокус на окне, а не на кнопке: без классического пунктира при открытии.
    // До кнопки — один Tab.
}

void SettingsDialog::Show(HWND hParent)
{
    // Синглтон: повторный клик поднимает уже открытое окно, а не плодит.
    if (IsWindow(s_hSettingsWnd))
    {
        BringSettingsToFront(s_hSettingsWnd);
        return;
    }
    (void)hParent; // без владельца: предсказуемый z-order и таскбар
    // Класс регистрируем один раз: повторный RegisterClassExW при каждом
    // Show() — лишний вызов (ошибка ERROR_CLASS_ALREADY_EXISTS игнорировалась
    // молча, но это мусор в логике).
    {
        static bool s_registered = false;
        if (!s_registered)
        {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.lpfnWndProc = SettingsProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = SETTINGS_CLASS;
            wc.hbrBackground = nullptr;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            if (RegisterClassExW(&wc) != 0 ||
                GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
                s_registered = true;
        }
    }

    SettingsWnd* st = new (std::nothrow) SettingsWnd();
    if (!st) return;
    st->rows = {
        { IDC_ROW_AUTOSTART, 0, Lang::Get(Str::S_Autostart),
          Lang::Get(Str::S_AutostartDesc),
          Autostart::IsEnabled() },
        { IDC_ROW_SNAP, 0, Lang::Get(Str::S_Snap),
          Lang::Get(Str::S_SnapDesc),
          Settings::IsSnapToGrid() },
        { IDC_ROW_HOTSEARCH, 0, Lang::Get(Str::S_HotSearch),
          Lang::Get(Str::S_HotSearchDesc),
          false },
        { IDC_ROW_HOTTOGGLE, 0, Lang::Get(Str::S_HotToggle),
          Lang::Get(Str::S_HotToggleDesc),
          false },
        { IDC_ROW_LANG, 0, Lang::Get(Str::S_Language),
          Lang::Get(Str::S_LanguageDesc),
          false },
        { IDC_ROW_PRUNE, 1, Lang::Get(Str::S_Prune),
          Lang::Get(Str::S_PruneDesc),
          Settings::IsAutoPruneDead() },
        { IDC_ROW_DEFGRID, 1, Lang::Get(Str::S_DefGrid),
          Lang::Get(Str::S_DefGridDesc),
          false },
        { IDC_ROW_OVERFLOW, 1, Lang::Get(Str::S_Overflow),
          Lang::Get(Str::S_OverflowDesc),
          Settings::IsShowOverflow() },
    };
    st->rows[2].isChoice = true; // хоткей поиска
    st->rows[3].isChoice = true; // хоткей виджетов
    st->rows[4].isChoice = true; // язык
    st->rows[6].isChoice = true; // размер сетки

    HWND hWnd = CreateWindowExW(kSetExStyle, SETTINGS_CLASS, Lang::Get(Str::S_Title),
        kSetStyle, CW_USEDEFAULT, 0, 100, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), st);

    if (!hWnd)
    {
        delete st; return;
    }
    s_hSettingsWnd = hWnd;
    // Центр экрана
    RECT wr = {};
    GetWindowRect(hWnd, &wr);
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    SetWindowPos(hWnd, HWND_TOP,
        work.left + (work.right - work.left - ww) / 2,
        work.top + (work.bottom - work.top - wh) / 2,
        0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    BringSettingsToFront(hWnd);
    // Фокус на окне, а не на кнопке: без классического пунктира при открытии.
    // До кнопки — один Tab.

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        if (!IsWindow(hWnd)) break; // окно снесли — выходим (без PostQuitMessage)
        // Захват хоткея — раньше всего: Esc отменяет захват (а не окно),
        // Tab/Space/стрелки идут в комбинацию, а не в навигацию.
        if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN))
        {
            SettingsWnd* cap = IsWindow(hWnd)
                ? (SettingsWnd*)GetWindowLongPtrW(hWnd, GWLP_USERDATA) : nullptr;
            if (cap && cap->capturingRow >= 0)
            {
                CaptureHotkey(cap, msg.wParam);
                continue;
            }
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
        {
            if (IsWindow(hWnd)) DestroyWindow(hWnd);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB)
        {
            SettingsWnd* cur = IsWindow(hWnd)
                ? (SettingsWnd*)GetWindowLongPtrW(hWnd, GWLP_USERDATA) : nullptr;
            if (cur)
            {
                bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                CycleSettingsFocus(cur, back);
                continue;
            }
        }
        if (msg.message == WM_KEYDOWN &&
            (msg.wParam == VK_SPACE || msg.wParam == VK_RETURN))
        {
            SettingsWnd* cur = IsWindow(hWnd)
                ? (SettingsWnd*)GetWindowLongPtrW(hWnd, GWLP_USERDATA) : nullptr;
            if (cur && msg.hwnd != cur->hBtnClose && GetFocus() != cur->hBtnClose &&
                cur->focusedRow >= 0)
            {
                ActivateRow(cur, cur->focusedRow);
                continue;
            }
        }
        if (IsDialogMessageW(hWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
