// UninstallHelper: поиск и запуск деинсталляторов (вынесено из PopupWindow.cpp).
#include "UninstallHelper.h"
#include "WidgetManager.h"
#include "Lang.h"
#include "Reg.h"
#include "ShellQuote.h"
#include <ShlObj.h>
#include <Shlwapi.h>
#include <shellapi.h>
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <cwctype>
#include <cstdio>

#pragma comment(lib, "Shlwapi.lib")

// ---------------------------------------------------------------------------
// «Удалить приложение»: пункт рядом с обычным «Удалить ярлык».
// Steam-игры — через steam://uninstall, классика — через UninstallString
// из реестра. Нет uninstaller'а (документ, папка, URL, Store) — пункта нет.
// ---------------------------------------------------------------------------

static std::wstring ExpandEnvStr(const std::wstring& s)
{
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (n == 0 || n > 32768) return s;
    std::wstring r(n, L'\0');
    if (!ExpandEnvironmentStringsW(s.c_str(), r.data(), n)) return s;
    r.resize(wcslen(r.c_str()));
    return r;
}

static std::wstring NormalizeSep(std::wstring s)
{
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    return s;
}

// DisplayIcon вида "путь,индекс" -> чистый путь.
static std::wstring StripIconIndex(const std::wstring& s)
{
    size_t c = s.find_last_of(L',');
    if (c == std::wstring::npos || c + 1 >= s.size()) return s;
    for (size_t i = c + 1; i < s.size(); i++)
        if (!iswdigit(s[i]) && !(i == c + 1 && s[i] == L'-'))
            return s;
    std::wstring r = s.substr(0, c);
    while (!r.empty() && iswspace(r.back())) r.pop_back();
    return r;
}

static bool SamePath(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(NormalizeSep(ExpandEnvStr(a)).c_str(),
        NormalizeSep(ExpandEnvStr(b)).c_str()) == 0;
}

// Поиск uninstaller'а цели в одной ветке Uninstall. Лучший по очкам:
// 100 — DisplayIcon точно указывает на цель,
// 60 — цель лежит в InstallLocation приложения.
static void FindUninstallerInHive(HKEY root, const wchar_t* uninstallPath,
    REGSAM samExtra,
    const std::wstring& target, UninstallInfo& best, int& bestScore)
{
    HKEY hUn = nullptr;
    // Явный флаг вида вместо пути ...\WOW6432Node\... в лоб: WOW-перенаправление
    // так не работает для прямых путей, 32-битные деинсталляторы терялись.
    if (RegOpenKeyExW(root, uninstallPath, 0, KEY_READ | samExtra, &hUn) != ERROR_SUCCESS)
        return;
    for (DWORD i = 0; ; i++)
    {
        WCHAR sub[256] = {};
        DWORD cch = _countof(sub);
        if (RegEnumKeyExW(hUn, i, sub, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        HKEY hApp = nullptr;
        if (RegOpenKeyExW(hUn, sub, 0, KEY_READ, &hApp) != ERROR_SUCCESS)
            continue;
        std::wstring disp = Reg::GetString(hApp, L"DisplayName");
        std::wstring uninst = Reg::GetString(hApp, L"UninstallString");
        std::wstring instDir = Reg::GetString(hApp, L"InstallLocation");
        std::wstring icon = Reg::GetString(hApp, L"DisplayIcon");
        std::wstring ver = Reg::GetString(hApp, L"DisplayVersion");
        DWORD estKB = Reg::GetDword(hApp, L"EstimatedSize", 0);
        DWORD sysComp = Reg::GetDword(hApp, L"SystemComponent", 0);
        DWORD noRemove = Reg::GetDword(hApp, L"NoRemove", 0);
        RegCloseKey(hApp);
        if (disp.empty() || uninst.empty()) continue;
        if (sysComp || noRemove) continue;

        int score = 0;
        std::wstring iconPath = StripIconIndex(icon);
        if (!iconPath.empty() && SamePath(iconPath, target))
        {
            score = 100;
        }
        else if (!instDir.empty())
        {
            std::wstring dir = NormalizeSep(ExpandEnvStr(instDir));
            while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
            std::wstring t = NormalizeSep(target);
            if (dir.size() + 1 < t.size() &&
                _wcsnicmp(t.c_str(), dir.c_str(), dir.size()) == 0 &&
                t[dir.size()] == L'\\')
                score = 60;
        }
        if (score > bestScore)
        {
            bestScore = score;
            best.displayName = disp;
            best.command = uninst;
            best.version = ver;
            best.sizeKB = estKB;
            best.isSteam = false;
            best.isAppx = false;
            best.steamAppId = 0;
        }
    }
    RegCloseKey(hUn);
}

static bool FindUninstallerUncached(const std::wstring& target, UninstallInfo& out)
{
    if (target.empty()) return false;
    // Steam-игра: удаление через клиент (свой диалог покажет сам Steam).
    DWORD appId = 0;
    if (WidgetManager::ParseSteamAppId(target, appId))
    {
        out = {};
        out.isSteam = true;
        out.steamAppId = appId;
        return true;
    }
    // Store-приложение (shell:AppsFolder\Семейство!Приложение): удаление
    // через Remove-AppxPackage для текущего пользователя.
    {
        std::wstring t = target;
        const wchar_t kAppsFolder[] = L"shell:AppsFolder\\";
        if (_wcsnicmp(t.c_str(), kAppsFolder, _countof(kAppsFolder) - 1) == 0)
            t = t.substr(_countof(kAppsFolder) - 1);
        size_t bang = t.find(L'!');
        // FamilyName!AppId, без путей и расширений.
        if (bang != std::wstring::npos && bang > 0 &&
            t.find_first_of(L"\\/.:") == std::wstring::npos)
        {
            out = {};
            out.isAppx = true;
            out.appxFamily = t.substr(0, bang);
            return true;
        }
    }
    // Прочие URI/протоколы — не приложения.
    if (target.find(L"://") != std::wstring::npos) return false;
    // Только .exe: документы, папки и скрипты как приложения не удаляем.
    PCWSTR ext = PathFindExtensionW(target.c_str());
    if (!ext || _wcsicmp(ext, L".exe") != 0) return false;
    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    static const struct { HKEY root; const wchar_t* path; REGSAM view; } kHives[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0 },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", KEY_WOW64_32KEY },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0 },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", KEY_WOW64_32KEY },
    };
    UninstallInfo best;
    int bestScore = 0;
    for (const auto& h : kHives)
        FindUninstallerInHive(h.root, h.path, h.view, target, best, bestScore);
    if (bestScore <= 0) return false;
    out = best;
    return true;
}

// Кэш поиска uninstaller'а на время одного открытого меню:
// в пачке один и тот же exe встречается многократно. Сбрасывается
// при каждом открытии меню (реестр мог измениться).
static std::map<std::wstring, std::pair<bool, UninstallInfo>> s_uninstCache;

void ClearUninstallCache()
{
    s_uninstCache.clear();
}

bool FindUninstallerForTarget(const std::wstring& target, UninstallInfo& out)
{
    if (target.empty()) return false;
    // Кэш статический — гард на случай вызовов с разных потоков.
    static std::mutex s_cacheMutex;
    std::lock_guard<std::mutex> lk(s_cacheMutex);
    std::wstring key = target;
    for (auto& c : key) c = towlower(c);
    auto it = s_uninstCache.find(key);
    if (it != s_uninstCache.end())
    {
        if (!it->second.first) return false;
        out = it->second.second;
        return true;
    }
    bool ok = FindUninstallerUncached(target, out);
    s_uninstCache[key] = { ok, out };
    return ok;
}

// "exe" + аргументы из UninstallString (кавычки и пути с пробелами — ок).
// Голые имена (MsiExec.exe, RunDll32.exe) резолвим через SearchPath:
// как файл в текущей папке их нет, но они лежат в System32/PATH.
static bool SplitFileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
        return true;
    if (p.find_first_of(L"\\/") == std::wstring::npos)
    {
        WCHAR full[MAX_PATH] = {};
        if (SearchPathW(nullptr, p.c_str(), nullptr, MAX_PATH, full, nullptr) > 0)
            return true;
    }
    return false;
}

static bool SplitUninstallCommand(const std::wstring& cmd,
    std::wstring& exe, std::wstring& args)
{
    size_t p = 0;
    while (p < cmd.size() && iswspace(cmd[p])) p++;
    if (p >= cmd.size()) return false;
    if (cmd[p] == L'"')
    {
        size_t q = cmd.find(L'"', p + 1);
        if (q == std::wstring::npos) return false;
        std::wstring inner = cmd.substr(p + 1, q - p - 1);
        size_t a = q + 1;
        while (a < cmd.size() && iswspace(cmd[a])) a++;
        // Бывает, что в кавычки взята вся команда целиком
        // ("MsiExec.exe /X{...}"): тогда разбираем внутренность заново.
        if (SplitFileExists(inner))
        {
            exe = inner;
            args = cmd.substr(a);
        }
        else if (!SplitUninstallCommand(inner, exe, args))
        {
            return false;
        }
    }
    else
    {
        // Без кавычек: самый длинный существующий префикс-путь.
        size_t end = cmd.size();
        while (end > p)
        {
            size_t e = end;
            while (e > p && iswspace(cmd[e - 1])) e--;
            if (e <= p) break;
            std::wstring cand = cmd.substr(p, e - p);
            if (SplitFileExists(cand))
            {
                exe = cand;
                size_t a = end;
                while (a < cmd.size() && iswspace(cmd[a])) a++;
                args = cmd.substr(a);
                break;
            }
            size_t sp = cmd.find_last_of(L" \t", e - 1);
            if (sp == std::wstring::npos || sp < p) break;
            end = sp;
        }
        if (exe.empty()) return false;
    }
    return !exe.empty();
}

static void ShowUninstallError(HWND hwnd, const std::wstring& detail)
{
    WCHAR buf[512];
    swprintf_s(buf, Lang::Get(Str::W_UninstallError), detail.c_str());
    MessageBoxW(hwnd, buf,
        Lang::Get(Str::W_ErrorCaption), MB_OK | MB_ICONERROR | MB_TOPMOST);
}

// "~350 МБ" / "~1,2 ГБ" / "800 КБ" из EstimatedSize (там килобайты).
static std::wstring FormatSizeKB(unsigned long kb)
{
    if (kb == 0) return {};
    WCHAR buf[64] = {};
    bool ru = Lang::IsRussian();
    if (kb >= 1024 * 1024)
        swprintf_s(buf, ru ? L"~%g ГБ" : L"~%g GB", kb / (1024.0 * 1024.0));
    else if (kb >= 1024)
        swprintf_s(buf, ru ? L"~%g МБ" : L"~%g MB", kb / 1024.0);
    else
        swprintf_s(buf, ru ? L"%lu КБ" : L"%lu KB", kb);
    return buf;
}

// Имя для диалога: "Chrome 120.0 (~350 МБ)" — что знаем, то показываем.
static std::wstring UninstallDisplayName(const ShortcutInfo& si, const UninstallInfo& ui)
{
    std::wstring name = !ui.displayName.empty() ? ui.displayName
        : (!si.name.empty() ? si.name : si.lnkPath);
    if (!ui.version.empty())
        name += L" " + ui.version;
    std::wstring sz = FormatSizeKB(ui.sizeKB);
    if (!sz.empty())
        name += L" (" + sz + L")";
    return name;
}

// Сколько осталось пакетов семейства (проверка результата Remove-AppxPackage).
// Строго точное имя (без wildcard-префикса): 'Family*' цепляло чужие
// семейства с общим префиксом, и Remove-AppxPackage сносил лишнее.
// @(...) обязателен: у одиночного пакета .Count равен $null.
static int CountAppxFamily(const std::wstring& family)
{
    if (family.empty()) return -1;
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -Command "
        L"\"@(Get-AppxPackage -Name '" + EscapePsSingleQuotes(family) + L"').Count\"";
    FILE* p = _wpopen(cmd.c_str(), L"r");
    if (!p) return -1;
    char out[64] = {};
    size_t n = fread(out, 1, sizeof(out) - 1, p);
    _pclose(p);
    if (n == 0) return -1;
    // Строго цифры: atoi хрупок к мусору/локали, парсим вручную.
    int v = 0;
    bool any = false;
    for (size_t i = 0; i < n; i++)
    {
        if (out[i] >= '0' && out[i] <= '9') { v = v * 10 + (out[i] - '0'); any = true; }
        else if (any) break;
    }
    return any ? v : -1;
}

void DoUninstallApp(HWND hwnd, const ShortcutInfo& si, const UninstallInfo& ui,
    bool confirm)
{
    if (confirm)
    {
        WCHAR buf[512];
        swprintf_s(buf, Lang::Get(Str::W_UninstallConfirm),
            UninstallDisplayName(si, ui).c_str());
        if (MessageBoxW(hwnd, buf, Lang::Get(Str::W_UninstallCaption),
                MB_YESNO | MB_ICONWARNING | MB_TOPMOST) != IDYES)
            return;
    }

    if (ui.isSteam)
    {
        WCHAR url[64];
        swprintf_s(url, L"steam://uninstall/%lu", (unsigned long)ui.steamAppId);
        HINSTANCE rc = ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOW);
        if ((UINT_PTR)rc <= 32)
            ShowUninstallError(hwnd, url);
        return;
    }

    if (ui.isAppx)
    {
        // Только текущий пользователь (системные пакеты так не снять).
        // Точное имя без wildcard (см. CountAppxFamily).
        std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden "
            L"-Command \"Get-AppxPackage -Name '" + EscapePsSingleQuotes(ui.appxFamily) + L"' | Remove-AppxPackage\"";
        STARTUPINFOW sui = { sizeof(sui) };
        PROCESS_INFORMATION pi = {};
        sui.dwFlags = STARTF_USESHOWWINDOW;
        sui.wShowWindow = SW_HIDE;
        // Динамический буфер: WCHAR[1024]+_TRUNCATE тихо резал длинные команды,
        // а wildcard мог зацепить чужое семейство.
        std::vector<wchar_t> cmdLine(cmd.size() + 1);
        wcscpy_s(cmdLine.data(), cmdLine.size(), cmd.c_str());
        BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &sui, &pi);
        if (!ok)
        {
            ShowUninstallError(hwnd, L"powershell.exe");
            return;
        }
        // По таймауту процесс НЕ убиваем (можно порвать незавершённое удаление):
        // закрываем хэндлы и проверяем результат по факту.
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        // Проверка: пакетов семейства не осталось?
        int left = CountAppxFamily(ui.appxFamily);
        if (left != 0)
            ShowUninstallError(hwnd, ui.appxFamily);
        return;
    }

    std::wstring exe, args;
    if (!SplitUninstallCommand(ui.command, exe, args) || exe.empty())
    {
        ShowUninstallError(hwnd, ui.command);
        return;
    }
    // MsiExec /I{...} открывает изменение, а не удаление — правим на /X.
    PCWSTR fn = PathFindFileNameW(exe.c_str());
    if (fn && _wcsicmp(fn, L"msiexec.exe") == 0)
    {
        for (size_t i = 0; i + 1 < args.size(); i++)
        {
            if ((args[i] == L'/' || args[i] == L'-') &&
                (args[i + 1] == L'I' || args[i + 1] == L'i'))
            {
                args[i + 1] = (args[i + 1] == L'I') ? L'X' : L'x';
                break;
            }
        }
    }
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", exe.c_str(),
        args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOW);
    if ((UINT_PTR)rc <= 32)
        ShowUninstallError(hwnd, exe);
    // Дальше — дело штатного uninstaller'а (админку запросит сам при нужде).
    // Ярлык подчистится позже через автопрунинг битых целей.
}