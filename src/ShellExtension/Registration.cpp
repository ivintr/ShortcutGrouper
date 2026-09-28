#include <Windows.h>
#include <ShObjIdl.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <string>
#include <vector>
#include "GroupCommand.h"
#include "DesktopOpenWithFilter.h"
#include "Lang.h"

#pragma comment(lib, "Shlwapi.lib")

// Пишем и в HKCR (машинный, нужен админ), и в HKCU\Software\Classes
// (per-user, работает без админа). Успех — хотя бы в одном корне:
// иначе отказ HKCR без админа ронял бы и per-user регистрацию тоже.
static HRESULT SetStrAll(PCWSTR sub, PCWSTR name, PCWSTR val)
{
    HRESULT hrAll = E_ACCESSDENIED;
    bool ok = false;
    for (int i = 0; i < 2; i++)
    {
        HKEY root = nullptr;
        std::wstring subKey;
        if (i == 0)
        {
            root = HKEY_CLASSES_ROOT;
            subKey = sub;
        }
        else
        {
            root = HKEY_CURRENT_USER;
            subKey = L"Software\\Classes\\";
            subKey += sub;
        }
        HKEY hk = nullptr;
        LONG r = RegCreateKeyExW(root, subKey.c_str(), 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hk, nullptr);
        if (r != ERROR_SUCCESS)
        {
            hrAll = HRESULT_FROM_WIN32(r);
            continue;
        }
        r = RegSetValueExW(hk, name, 0, REG_SZ,
            (const BYTE*)val, (DWORD)((wcslen(val) + 1) * sizeof(WCHAR)));
        RegCloseKey(hk);
        if (r != ERROR_SUCCESS)
        {
            hrAll = HRESULT_FROM_WIN32(r);
            continue;
        }
        ok = true;
    }
    return ok ? S_OK : hrAll;
}

static std::wstring GetDllPath()
{
    std::vector<WCHAR> buf(1024);
    for (;;)
    {
        DWORD n = GetModuleFileNameW(GetModuleHandle_(), buf.data(), (DWORD)buf.size());
        if (n == 0) return L"";
        if (n < buf.size() - 1) return std::wstring(buf.data(), n);
        if (buf.size() >= 32768) return L"";
        buf.resize(buf.size() * 2);
    }
}

static void DelKeyAll(PCWSTR sub)
{
    RegDeleteTreeW(HKEY_CLASSES_ROOT, sub);
    std::wstring hkcu = L"Software\\Classes\\";
    hkcu += sub;
    RegDeleteTreeW(HKEY_CURRENT_USER, hkcu.c_str());
}

// ---------------------------------------------------------------------------
// DllRegisterServer
// ---------------------------------------------------------------------------
STDAPI DllRegisterServer()
{
    WCHAR szClsid[64];
    StringFromGUID2(CLSID_GroupCommand, szClsid, 64);

    std::wstring szDll = GetDllPath();
    if (szDll.empty())
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);

    HRESULT hr = S_OK;

    // --- CLSID\{...} ---
    hr = SetStrAll(
        (L"CLSID\\" + std::wstring(szClsid)).c_str(),
        nullptr, L"Group Command Extension");
    if (FAILED(hr)) return hr;

    hr = SetStrAll(
        (L"CLSID\\" + std::wstring(szClsid) + L"\\InprocServer32").c_str(),
        nullptr, szDll.c_str());
    if (FAILED(hr)) return hr;

    hr = SetStrAll(
        (L"CLSID\\" + std::wstring(szClsid) + L"\\InprocServer32").c_str(),
        L"ThreadingModel", L"Both");
    if (FAILED(hr)) return hr;

    // --- shell\GroupShortcuts (Windows 11 command bar + legacy) ---
    // Регистрируем для ярлыков (lnkfile), всех файлов (*) и папок (Directory)
    const wchar_t* classes[] = { L"lnkfile", L"*", L"Directory" };
    // Иконка нового меню Windows 11: наш exe рядом с DLL, иначе системная.
    std::wstring icon = L"imageres.dll,-112";
    {
        std::wstring dir = szDll;
        size_t p = dir.find_last_of(L"\\/");
        if (p != std::wstring::npos) dir.resize(p);
        std::wstring exeOnly = dir + L"\\GroupManager.exe";
        if (GetFileAttributesW(exeOnly.c_str()) != INVALID_FILE_ATTRIBUTES)
            icon = exeOnly + L",0";
    }
    for (int ci = 0; ci < 3; ci++)
    {
        std::wstring verbKey = std::wstring(classes[ci]) + L"\\shell\\GroupShortcuts";
        hr = SetStrAll(verbKey.c_str(), nullptr, Lang::Get(Str::SH_Title));
        if (FAILED(hr)) return hr;

        hr = SetStrAll(verbKey.c_str(), L"MUIVerb", Lang::Get(Str::SH_Title));
        if (FAILED(hr)) return hr;

        hr = SetStrAll(verbKey.c_str(), L"Icon", icon.c_str());
        if (FAILED(hr)) return hr;

        hr = SetStrAll(verbKey.c_str(), L"ExplorerCommandHandler", szClsid);
        if (FAILED(hr)) return hr;

        // БЕЗ ветки \command с rundll32: такого экспорта нет, а ключ
        // конфликтовал бы с ExplorerCommandHandler.
        // БЕЗ shellex\ContextMenuHandlers: GroupCommand реализует только
        // IExplorerCommand, а legacy-хендлер требует IContextMenu (QI падал).
        // Апгрейд со старых версий: сносим эти хвосты, если они остались.
        DelKeyAll((verbKey + L"\\command").c_str());
        DelKeyAll((std::wstring(classes[ci]) +
            L"\\shellex\\ContextMenuHandlers\\GroupShortcuts").c_str());
    }

    // --- DesktopOpenWithFilter: register for * (all files) ---
    WCHAR szFilterClsid[64];
    StringFromGUID2(CLSID_DesktopOpenWithFilter, szFilterClsid, 64);

    hr = SetStrAll(
        (L"CLSID\\" + std::wstring(szFilterClsid)).c_str(),
        nullptr, L"Desktop OpenWith Filter");
    if (FAILED(hr)) return hr;

    hr = SetStrAll(
        (L"CLSID\\" + std::wstring(szFilterClsid) + L"\\InprocServer32").c_str(),
        nullptr, szDll.c_str());
    if (FAILED(hr)) return hr;

    hr = SetStrAll(
        (L"CLSID\\" + std::wstring(szFilterClsid) + L"\\InprocServer32").c_str(),
        L"ThreadingModel", L"Both");
    if (FAILED(hr)) return hr;

    hr = SetStrAll(
        L"*\\shellex\\ContextMenuHandlers\\DesktopOpenWithFilter", nullptr, szFilterClsid);
    if (FAILED(hr)) return hr;

    return S_OK;
}

// ---------------------------------------------------------------------------
// DllUnregisterServer
// ---------------------------------------------------------------------------
STDAPI DllUnregisterServer()
{
    WCHAR szClsid[64];
    StringFromGUID2(CLSID_GroupCommand, szClsid, 64);

    HRESULT hr = S_OK;
    auto del = [&](PCWSTR sub) {
        LONG r1 = RegDeleteTreeW(HKEY_CLASSES_ROOT, sub);
        std::wstring hkcu = L"Software\\Classes\\";
        hkcu += sub;
        LONG r2 = RegDeleteTreeW(HKEY_CURRENT_USER, hkcu.c_str());
        if ((r1 != ERROR_SUCCESS && r1 != ERROR_FILE_NOT_FOUND) ||
            (r2 != ERROR_SUCCESS && r2 != ERROR_FILE_NOT_FOUND))
            hr = HRESULT_FROM_WIN32(
                r1 != ERROR_SUCCESS && r1 != ERROR_FILE_NOT_FOUND ? r1 : r2);
    };

    del((L"CLSID\\" + std::wstring(szClsid)).c_str());
    const wchar_t* classes[] = { L"lnkfile", L"*", L"Directory" };
    for (int ci = 0; ci < 3; ci++)
    {
        del((std::wstring(classes[ci]) + L"\\shell\\GroupShortcuts").c_str());
        // Чистим и легаси-хвосты от старых версий (включая \command).
        del((std::wstring(classes[ci]) +
            L"\\shellex\\ContextMenuHandlers\\GroupShortcuts").c_str());
    }

    WCHAR szFilterClsid[64];
    StringFromGUID2(CLSID_DesktopOpenWithFilter, szFilterClsid, 64);
    del((L"CLSID\\" + std::wstring(szFilterClsid)).c_str());
    del(L"*\\shellex\\ContextMenuHandlers\\DesktopOpenWithFilter");

    return hr;
}
