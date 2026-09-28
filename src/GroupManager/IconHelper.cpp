#include "IconHelper.h"
#include <shobjidl.h>
#include <shlwapi.h>
#include <ShlObj.h>
#include <shellapi.h>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")

ShortcutInfo ExtractLnkInfo(const std::wstring& lnkPath)
{
    ShortcutInfo info;
    info.lnkPath = lnkPath;

    // Интернет-ярлык .url — текстовый INI: [InternetShortcut] URL=...
    PCWSTR ext = PathFindExtensionW(lnkPath.c_str());
    if (ext && _wcsicmp(ext, L".url") == 0)
    {
        PCWSTR fname = PathFindFileNameW(lnkPath.c_str());
        if (fname)
        {
            info.name = fname;
            PCWSTR dot = PathFindExtensionW(info.name.c_str());
            if (dot) info.name.resize(dot - info.name.c_str());
        }

        WCHAR url[2048] = {};
        GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"",
            url, (sizeof(url) / sizeof(url[0])), lnkPath.c_str());
        info.targetPath = url;
        if (info.targetPath.empty())
            info.targetPath = lnkPath; // ShellExecute открывает сам .url
        return info;
    }

    DWORD attr = GetFileAttributesW(lnkPath.c_str());
    bool isDir = (attr != INVALID_FILE_ATTRIBUTES) &&
        (attr & FILE_ATTRIBUTE_DIRECTORY);
    bool isLnk = ext && _wcsicmp(ext, L".lnk") == 0;

    // Обычный файл или папка: цель не нужна, запуск/иконка идут
    // через сам файл (lnkPath). Имя — как на рабочем столе.
    if (!isLnk)
    {
        PCWSTR fname = PathFindFileNameW(lnkPath.c_str());
        if (fname) info.name = fname;
        if (!isDir)
        {
            // Расширение прячем, как проводник по умолчанию
            PCWSTR dot = PathFindExtensionW(info.name.c_str());
            if (dot && dot != info.name.c_str()) info.name.resize(dot - info.name.c_str());
        }
        info.targetPath.clear();
        return info;
    }

    IShellLinkW* pShellLink = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_IShellLinkW, (void**)&pShellLink);
    if (FAILED(hr)) return info;

    IPersistFile* pPersist = nullptr;
    hr = pShellLink->QueryInterface(IID_IPersistFile, (void**)&pPersist);
    if (SUCCEEDED(hr) && pPersist)
    {
        hr = pPersist->Load(lnkPath.c_str(), STGM_READ);
        if (SUCCEEDED(hr))
        {
            // Буферы с запасом: MAX_PATH резал длинные цели ярлыков.
            WCHAR target[32768] = {};
            if (SUCCEEDED(pShellLink->GetPath(target, _countof(target), nullptr, 0)))
                info.targetPath = target;

            WCHAR desc[1024] = {};
            pShellLink->GetDescription(desc, _countof(desc));

            // Имя — как подпись на рабочем столе: из имени самого .lnk.
            // Description часто содержит мусор вида "Щелкните здесь...".
            PCWSTR lnkFname = PathFindFileNameW(lnkPath.c_str());
            if (lnkFname && *lnkFname)
            {
                info.name = lnkFname;
                PCWSTR dot = PathFindExtensionW(info.name.c_str());
                if (dot) info.name.resize(dot - info.name.c_str());
            }
            if (info.name.empty())
                info.name = desc;
            if (info.name.empty())
            {
                PCWSTR fname = PathFindFileNameW(target);
                if (fname)
                {
                    info.name = fname;
                    PCWSTR dot = PathFindExtensionW(info.name.c_str());
                    if (dot) info.name.resize(dot - info.name.c_str());
                }
            }
        }
        pPersist->Release();
    }
    pShellLink->Release();
    return info;
}

HICON ExtractFolderIcon(int size)
{
    SHFILEINFOW sfi = {};
    SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
        SHGFI_ICON | SHGFI_USEFILEATTRIBUTES | (size >= 48 ? SHGFI_LARGEICON : SHGFI_SMALLICON));
    return sfi.hIcon;
}

HICON IconFromLnkLocation(const std::wstring& lnkPath)
{
    if (lnkPath.empty()) return nullptr;
    PCWSTR ext = PathFindExtensionW(lnkPath.c_str());
    if (!ext || _wcsicmp(ext, L".lnk") != 0) return nullptr;

    IShellLinkW* psl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&psl))) || !psl)
        return nullptr;

    HICON hIcon = nullptr;
    IPersistFile* ppf = nullptr;
    if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARGS(&ppf))) && ppf)
    {
        if (SUCCEEDED(ppf->Load(lnkPath.c_str(), STGM_READ)))
        {
            WCHAR iconPath[32768] = {};
            int iconIndex = 0;
            if (SUCCEEDED(psl->GetIconLocation(iconPath, _countof(iconPath), &iconIndex)) &&
                iconPath[0])
            {
                WCHAR expanded[32768] = {};
                DWORD n = ExpandEnvironmentStringsW(iconPath, expanded, _countof(expanded));
                PCWSTR use = (n > 0 && n < _countof(expanded)) ? expanded : iconPath;
                if (GetFileAttributesW(use) != INVALID_FILE_ATTRIBUTES)
                {
                    HICON hDirect = nullptr;
                    if (PrivateExtractIconsW(use, iconIndex, 48, 48,
                            &hDirect, nullptr, 1, 0) == 1 && hDirect)
                        hIcon = hDirect;
                }
            }
        }
        ppf->Release();
    }
    psl->Release();
    return hIcon;
}
