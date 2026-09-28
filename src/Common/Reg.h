#pragma once
// Общие хелперы реестра: раньше RegGetDword/RegSetDword/RegReadStrVal/
// RegReadDwordVal дублировались в SettingsDialog.cpp и PopupWindow.cpp
// с разными багами (фиксированные буферы, игнор ERROR_MORE_DATA).
#include <Windows.h>
#include <string>
#include <vector>

namespace Reg {

// DWORD из открытого ключа; def при отсутствии/неверном типе.
inline DWORD GetDword(HKEY hKey, const wchar_t* name, DWORD def)
{
    DWORD v = def;
    DWORD cb = sizeof(v);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, name, nullptr, &type, (BYTE*)&v, &cb) != ERROR_SUCCESS ||
        type != REG_DWORD)
        return def;
    return v;
}

// DWORD в открытый ключ.
inline bool SetDword(HKEY hKey, const wchar_t* name, DWORD v)
{
    return RegSetValueExW(hKey, name, 0, REG_DWORD,
        (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS;
}

// DWORD по корню+подключу (создаёт/открывает, читает/пишет, закрывает).
inline DWORD GetDwordAt(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD def)
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return def;
    DWORD v = GetDword(hKey, name, def);
    RegCloseKey(hKey);
    return v;
}

inline bool SetDwordAt(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD v)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(root, sub, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = SetDword(hKey, name, v);
    RegCloseKey(hKey);
    return ok;
}

// Строка двухпроходным чтением (без обрезки ERROR_MORE_DATA).
// Экспандим только REG_EXPAND_SZ: буквальный % в REG_SZ остаётся как есть.
inline std::wstring GetString(HKEY hKey, const wchar_t* name)
{
    DWORD cb = 0;
    DWORD type = 0;
    LONG rc = RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &cb);
    if ((rc != ERROR_SUCCESS && rc != ERROR_MORE_DATA) ||
        (type != REG_SZ && type != REG_EXPAND_SZ) ||
        cb < sizeof(wchar_t) || cb > 256 * 1024)
        return {};
    std::vector<BYTE> buf(cb + sizeof(wchar_t), 0);
    DWORD cb2 = cb;
    if (RegQueryValueExW(hKey, name, nullptr, &type, buf.data(), &cb2) != ERROR_SUCCESS)
        return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ) return {};
    buf[cb] = 0;
    buf[cb + 1] = 0;
    std::wstring s = (wchar_t*)buf.data();
    if (type == REG_EXPAND_SZ)
    {
        DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
        if (n > 1 && n <= 32768)
        {
            std::wstring r(n, L'\0');
            if (ExpandEnvironmentStringsW(s.c_str(), r.data(), n))
                r.resize(wcslen(r.c_str()));
            else
                r = s;
            return r;
        }
    }
    return s;
}

} // namespace Reg
