#include "WidgetManager.h"
#include "Logger.h"
#include "PopupWindow.h"
#include "IconHelper.h"
#include "Marquee.h"
#include "SearchWindow.h"
#include "SettingsDialog.h"
#include "DesktopGrid.h"
#include "Lang.h"
#include <fstream>
#include <cwctype>
#include <ctime>
#include <new>
#include <nlohmann/json.hpp>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <shellapi.h>
#include <algorithm>

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Shell32.lib")

using json = nlohmann::json;

static std::vector<PopupWindow*> s_popups;

PopupWindow* FindPopupByGroupId(const std::wstring& id)
{
    for (auto* p : s_popups)
        if (p->IsVisible() && p->GetGroupId() == id)
            return p;
    return nullptr;
}

void RegisterPopupWindow(PopupWindow* popup)
{
    if (!popup) return;
    for (auto* p : s_popups)
        if (p == popup) return;
    s_popups.push_back(popup);
}

void UnregisterPopupWindow(PopupWindow* popup)
{
    auto it = std::find(s_popups.begin(), s_popups.end(), popup);
    if (it != s_popups.end()) s_popups.erase(it);
}

static void WL(const wchar_t* msg)
{
    AppLog(L"WM", L"%s", msg);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

static std::wstring ToW(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring r(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), r.data(), len);
    return r;
}

static std::string ToA(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string r(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), r.data(), len, nullptr, nullptr);
    return r;
}

std::wstring WidgetManager::GetGroupsDir()
{
    std::wstring base = GetRoamingAppData();
    if (base.empty()) return {};
    CreateDirectoryW((base + L"\\DesktopGroupManager").c_str(), nullptr);
    std::wstring dir = base + L"\\DesktopGroupManager\\Groups";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring WidgetManager::GetJsonPath()
{
    std::wstring base = GetRoamingAppData();
    if (base.empty()) return {};
    std::wstring dir = base + L"\\DesktopGroupManager";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\groups.json";
}

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------

bool WidgetManager::Initialize(WidgetRenderer* renderer)
{
    ULONGLONG t0 = GetTickCount64();
    m_renderer = renderer;
    LoadGroups();
    WCHAR buf[128];
    swprintf_s(buf, L"Init: LoadGroups %llums", GetTickCount64() - t0);
    WL(buf);
    if (Settings::IsAutoPruneDead())
        PruneDeadShortcuts(); // сироты после удалённых приложений — сразу при старте
    swprintf_s(buf, L"Init: total %llums", GetTickCount64() - t0);
    WL(buf);
    MarqueeInit(this); // рамка выделения по пустому столу
    if (Settings::ConsumeFirstRun())
        NotifyBalloon(Lang::Get(Str::S_OnboardTitle), Lang::Get(Str::S_OnboardText));
    return true;
}

void WidgetManager::Shutdown()
{
    if (m_search)
    {
        m_search->Destroy();
        delete m_search;
        m_search = nullptr;
    }
    MarqueeShutdown();
    DestroyAllWidgets();
    SaveGroups();
    // Сбрасываем состояние: висячие указатели после Shutdown —
    // use-after-free при повторном использовании менеджера.
    m_renderer = nullptr;
    m_hDesktopParent = nullptr;
    m_hNotifyWnd = nullptr;
    m_groups.clear();
    m_selected.clear();
    SetDesktopParent(nullptr);
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

void WidgetManager::LoadGroups()
{
    m_groups.clear();

    std::wstring path = GetJsonPath();
    std::ifstream in(path);
    if (!in.is_open()) return;

    try
    {
        json root = json::parse(in);
        if (!root.contains("groups") || !root["groups"].is_array()) return;

        for (const auto& jg : root["groups"])
        {
            GroupData g;
            g.id   = ToW(jg.value("id", ""));
            g.name = ToW(jg.value("name", ""));
            g.x    = jg.value("x", 0);
            g.y    = jg.value("y", 0);
            g.popupW = jg.value("popupW", 0);
            g.popupListH = jg.value("popupListH", -1);
            g.sortMode = jg.value("sortMode", 0);
            g.glassColor = jg.value("glassColor", 0xFFFFFF);
            g.hideName = jg.value("hideName", false);
            g.showOverflow = jg.value("showOverflow", true);
            g.gridSize = jg.value("gridSize", 2);
            if (g.gridSize < 2 || g.gridSize > 3) g.gridSize = 2;

            if (jg.contains("shortcuts") && jg["shortcuts"].is_array())
            {
                for (const auto& js : jg["shortcuts"])
                {
                    ShortcutInfo si;
                    si.lnkPath    = ToW(js.value("lnkPath", ""));
                    si.targetPath = ToW(js.value("targetPath", ""));
                    si.name       = ToW(js.value("name", ""));
                    si.uses       = js.value("uses", 0);
                    si.lastUsed   = js.value("lastUsed", 0LL);
                    g.shortcuts.push_back(std::move(si));
                }
            }

            if (!g.id.empty())
                m_groups.push_back(std::move(g));
        }
    }
    catch (const json::exception&) {}
}

void WidgetManager::SaveGroups()
{
    json root;
    json jArr = json::array();

    for (const auto& g : m_groups)
    {
        json jg;
        jg["id"]   = ToA(g.id);
        jg["name"] = ToA(g.name);
        jg["x"]    = g.x;
        jg["y"]    = g.y;
        jg["popupW"] = g.popupW;
        jg["popupListH"] = g.popupListH;
        jg["sortMode"] = g.sortMode;
        jg["glassColor"] = g.glassColor;
        jg["hideName"] = g.hideName;
        jg["showOverflow"] = g.showOverflow;
        jg["gridSize"] = g.gridSize;

        json jShortcuts = json::array();
        for (const auto& s : g.shortcuts)
        {
            json js;
            js["lnkPath"]    = ToA(s.lnkPath);
            js["targetPath"] = ToA(s.targetPath);
            js["name"]       = ToA(s.name);
            js["uses"]       = s.uses;
            js["lastUsed"]   = s.lastUsed;
            jShortcuts.push_back(js);
        }
        jg["shortcuts"] = jShortcuts;
        jArr.push_back(jg);
    }

    root["groups"] = jArr;

    std::wstring path = GetJsonPath();
    std::ofstream out(path);
    if (out.is_open())
        out << root.dump(4);
}

// ---------------------------------------------------------------------------
// Проверка «живости» ярлыков: приложение удалили — ярлык убираем из группы.
// ---------------------------------------------------------------------------

static bool IsSteamGameInstalled(DWORD appId);

// URI/протокол (http://, steam://, ms-settings: ...) — не файл, не проверяем.
// Путь с буквой диска (C:\...) URI не считается.
static bool IsUriTarget(const std::wstring& t)
{
    if (t.find(L"://") != std::wstring::npos) return true;
    if (t.size() >= 3 && std::iswalpha(t[0]) && t[1] == L':' &&
        (t[2] == L'\\' || t[2] == L'/'))
        return false;
    size_t colon = t.find(L':');
    if (colon == std::wstring::npos || colon == 0) return false;
    size_t slash = t.find_first_of(L"/\\");
    if (slash != std::wstring::npos && slash < colon) return false;
    if (!std::iswalpha(t[0])) return false;
    for (size_t i = 1; i < colon; i++)
        if (!std::iswalnum(t[i]) && t[i] != L'+' && t[i] != L'-' && t[i] != L'.')
            return false;
    return true;
}

// true = файл точно мёртв: носитель на месте, а файла нет.
// Если том/шара недоступны (флешка вынута, сеть отвалилась) —
// считаем живым, чтобы не стереть рабочие ярлыки.
static bool IsDeadFileTarget(const std::wstring& target)
{
    if (target.empty()) return false;
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
        return false;
    WCHAR root[MAX_PATH] = {};
    if (GetVolumePathNameW(target.c_str(), root, MAX_PATH) && root[0])
    {
        if (GetFileAttributesW(root) == INVALID_FILE_ATTRIBUTES)
            return false; // том/шара недоступны — не трогаем
        return true; // носитель на месте, файла нет — мёртв
    }
    // Корень не определился: для буквы диска проверяем "X:\".
    if (target.size() >= 2 && std::iswalpha(target[0]) && target[1] == L':')
    {
        WCHAR drvRoot[4] = { target[0], L':', L'\\', 0 };
        if (GetFileAttributesW(drvRoot) == INVALID_FILE_ATTRIBUTES)
            return false;
        return true;
    }
    // Непонятный путь — не рискуем.
    return false;
}

bool WidgetManager::IsShortcutAlive(const ShortcutInfo& si)
{
    if (!si.lnkPath.empty() &&
        GetFileAttributesW(si.lnkPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false; // файла в хранилище уже нет
    if (si.targetPath.empty())
        return true; // обычный файл/папка в хранилище, .lnk без цели — жив
    DWORD steamAppId = 0;
    if (ParseSteamAppId(si.targetPath, steamAppId))
        return IsSteamGameInstalled(steamAppId); // Steam-игра: смотрим манифест
    if (IsUriTarget(si.targetPath))
        return true; // прочие URL/протоколы не проверяем
    return !IsDeadFileTarget(si.targetPath);
}

void WidgetManager::EraseStoredLnk(const std::wstring& groupsDir, const ShortcutInfo& si)
{
    if (si.lnkPath.empty() || groupsDir.empty()) return;
    if (_wcsnicmp(si.lnkPath.c_str(), groupsDir.c_str(), groupsDir.size()) != 0)
        return; // не наше хранилище (режим --no-move) — файл не трогаем
    DeletePathSilent(si.lnkPath);
}

// ---------------------------------------------------------------------------
// Steam: игры удаляются вместе с appmanifest_<id>.acf, а steam://-ссылка
// остаётся валидной всегда — её проверяем по манифестам библиотек.
// ---------------------------------------------------------------------------

// steam://rungameid/<appid> — единственный проверяемый формат;
// прочие steam://-ссылки (store, openurl, ...) пропускаем.
bool WidgetManager::ParseSteamAppId(const std::wstring& t, DWORD& appId)
{
    static const wchar_t kPrefix[] = L"steam://rungameid/";
    if (_wcsnicmp(t.c_str(), kPrefix, _countof(kPrefix) - 1) != 0)
        return false;
    const wchar_t* p = t.c_str() + (_countof(kPrefix) - 1);
    if (!std::iswdigit(*p)) return false;
    wchar_t* end = nullptr;
    unsigned long id = wcstoul(p, &end, 10);
    if (id == 0 || id > 0xFFFFFFFFul || !end) return false;
    if (*end != 0 && *end != L'/' && *end != L'?' && *end != L' ')
        return false;
    appId = (DWORD)id;
    return true;
}

static bool GetSteamInstallDir(std::wstring& out)
{
    WCHAR buf[MAX_PATH] = {};
    DWORD cb = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam",
            L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS && buf[0])
    {
        out = buf;
    }
    else
    {
        cb = sizeof(buf);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam",
                L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS && buf[0])
            out = buf;
        else
        {
            cb = sizeof(buf);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam",
                    L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS || !buf[0])
                return false;
            out = buf;
        }
    }
    for (auto& c : out)
        if (c == L'/') c = L'\\'; // в реестре путь с прямыми слешами
    while (out.size() > 3 && out.back() == L'\\')
        out.pop_back();
    return !out.empty();
}

// Папки библиотек из libraryfolders.vdf (+ сам каталог Steam).
static std::vector<std::wstring> GetSteamLibraryDirs(const std::wstring& steamDir)
{
    std::vector<std::wstring> libs;
    libs.push_back(steamDir);

    std::ifstream in(steamDir + L"\\steamapps\\libraryfolders.vdf", std::ios::binary);
    if (!in.is_open()) return libs;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        pos = 3; // UTF-8 BOM

    while ((pos = data.find("\"path\"", pos)) != std::string::npos)
    {
        pos += 6;
        while (pos < data.size() && isspace((unsigned char)data[pos])) pos++;
        if (pos >= data.size() || data[pos] != '"') continue;
        pos++;
        std::string val;
        while (pos < data.size() && data[pos] != '"')
        {
            if (data[pos] == '\\' && pos + 1 < data.size())
            {
                val += data[pos + 1]; // VDF-экранирование: \\ -> \, \" -> "
                pos += 2;
            }
            else
            {
                val += data[pos];
                pos++;
            }
        }
        if (val.empty()) continue;
        int len = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), nullptr, 0);
        if (len <= 0) continue;
        std::wstring w(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), w.data(), len);
        for (auto& c : w)
            if (c == L'/') c = L'\\';
        while (w.size() > 3 && w.back() == L'\\')
            w.pop_back();
        if (_wcsicmp(w.c_str(), steamDir.c_str()) != 0)
            libs.push_back(w);
    }
    return libs;
}

// true = игра установлена (есть манифест) или проверить нельзя
// (нет Steam / недоступна хоть одна библиотека — не рискуем).
static bool IsSteamGameInstalled(DWORD appId)
{
    std::wstring steamDir;
    if (!GetSteamInstallDir(steamDir)) return true;
    auto libs = GetSteamLibraryDirs(steamDir);
    WCHAR manifest[64] = {};
    swprintf_s(manifest, L"appmanifest_%lu.acf", (unsigned long)appId);
    bool allAccessible = true;
    for (const auto& lib : libs)
    {
        if (GetFileAttributesW(lib.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            allAccessible = false; // библиотека на отключённом диске — не трогаем
            continue;
        }
        if (GetFileAttributesW((lib + L"\\steamapps\\" + manifest).c_str()) != INVALID_FILE_ATTRIBUTES)
            return true;
    }
    return !allAccessible;
}

// ---------------------------------------------------------------------------
// Group operations
// ---------------------------------------------------------------------------

std::wstring WidgetManager::MoveLnkToStorage(const std::wstring& lnkPath, const std::wstring& groupId)
{
    std::wstring groupsDir = GetGroupsDir();
    std::wstring groupDir = groupsDir + L"\\" + groupId;
    CreateDirectoryW(groupDir.c_str(), nullptr);

    PCWSTR fname = PathFindFileNameW(lnkPath.c_str());
    std::wstring dst = groupDir + L"\\" + fname;

    if (MoveFileW(lnkPath.c_str(), dst.c_str()))
        return dst;

    if (CopyFileW(lnkPath.c_str(), dst.c_str(), FALSE))
        return dst;

    return {};
}

bool WidgetManager::CreateGroup(const std::wstring& name,
    const std::vector<std::wstring>& lnkPaths, int spawnX, int spawnY, bool skipMove)
{
    if (lnkPaths.empty()) return false;

    GroupData g;
    g.id = GenerateGroupId();
    if (g.id.empty()) return false; // CoCreateGuid провалился — без ID нельзя
    g.name = name;
    g.x = spawnX;
    g.y = spawnY;
    g.gridSize = Settings::GetDefaultGrid(); // размер сетки по умолчанию

    for (const auto& lnk : lnkPaths)
    {
        ShortcutInfo si = ExtractLnkInfo(lnk);
        si.lnkPath = lnk;
        g.shortcuts.push_back(std::move(si));
    }

    if (!skipMove)
    {
        for (auto& s : g.shortcuts)
        {
            std::wstring stored = MoveLnkToStorage(s.lnkPath, g.id);
            if (!stored.empty()) s.lnkPath = stored;
        }
    }

    m_groups.push_back(g);
    SaveGroups();

    if (m_hDesktopParent && m_renderer)
    {
        auto* widget = new DesktopWidget();
        if (widget->Create(m_hDesktopParent, g, m_renderer, this))
        {
            if (g.x >= 0 && g.y >= 0)
                widget->ShowAt(g.x, g.y);
            else
                widget->ShowAt(100 + (int)m_widgets.size() * 90, 100);
            m_widgets.push_back(widget);
            PushIconsOutOfWidgets();
            ApplyWidgetsVisibility();
        }
    }

    return true;
}

bool WidgetManager::AddShortcutToGroup(const std::wstring& groupId, const std::wstring& lnkPath)
{
    for (auto& g : m_groups)
    {
        if (g.id == groupId)
        {
            ShortcutInfo si = ExtractLnkInfo(lnkPath);
            if (si.lnkPath.empty()) {
                WL(L"AddShortcutToGroup: ExtractLnkInfo failed for");
                OutputDebugStringW(lnkPath.c_str());
                OutputDebugStringW(L"\n");
                return false;
            }

            std::wstring stored = MoveLnkToStorage(lnkPath, g.id);
            if (!stored.empty()) si.lnkPath = stored;

            g.shortcuts.push_back(std::move(si));
            WL(L"AddShortcutToGroup: saving after adding shortcut");
            ApplySortMode(g); // автосортировка, если включена для группы
            SaveGroups();
            return true;
        }
    }
    WL(L"AddShortcutToGroup: group not found");
    return false;
}

bool WidgetManager::RemoveShortcutFromGroup(const std::wstring& groupId, int index)
{
    for (auto& g : m_groups)
    {
        if (g.id == groupId && index >= 0 && index < (int)g.shortcuts.size())
        {
            const auto& si = g.shortcuts[index];

            WCHAR desktopPath[MAX_PATH] = {};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0, desktopPath)))
            {
                PCWSTR fname = PathFindFileNameW(si.lnkPath.c_str());
                std::wstring dst = std::wstring(desktopPath) + L"\\" + fname;
                MoveFileW(si.lnkPath.c_str(), dst.c_str());
            }

            g.shortcuts.erase(g.shortcuts.begin() + index);

            if (g.shortcuts.empty())
            {
                RemoveGroup(groupId);
            }
            else
            {
                SaveGroups();
            }

            for (auto* w : m_widgets)
            {
                if (w->GetGroupId() == groupId)
                {
                    if (g.shortcuts.empty())
                        break;
                    const auto& groups = GetGroups();
                    for (const auto& ug : groups)
                    {
                        if (ug.id == groupId)
                        {
                            w->Update(ug);
                            break;
                        }
                    }
                    break;
                }
            }
            // Ярлык вернулся на стол — вытолкнуть его из-под виджетов.
            PushIconsOutOfWidgets();
            return true;
        }
    }
    return false;
}

int WidgetManager::PruneDeadShortcuts()
{
    std::wstring groupsDir = GetGroupsDir();
    int removed = 0;

    for (auto it = m_groups.begin(); it != m_groups.end(); )
    {
        for (auto sit = it->shortcuts.begin(); sit != it->shortcuts.end(); )
        {
            if (IsShortcutAlive(*sit)) { ++sit; continue; }
            EraseStoredLnk(groupsDir, *sit);
            sit = it->shortcuts.erase(sit);
            removed++;
        }
        if (it->shortcuts.empty())
        {
            // Группа опустела — удаляем вместе с папкой хранилища.
            // Виджеты не трогаем: вызыватели (Initialize/RefreshWidgets)
            // пересоздают их целиком после чистки.
            RemoveDirectoryW((groupsDir + L"\\" + it->id).c_str());
            it = m_groups.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (removed > 0)
    {
        WCHAR buf[64];
        swprintf_s(buf, L"Pruned %d dead shortcuts", removed);
        WL(buf);
        SaveGroups();
        WCHAR msg[128];
        swprintf_s(msg, Lang::Get(Str::W_PrunedMany), removed);
        NotifyBalloon(Lang::Get(Str::W_PrunedTitle), msg);
    }
    return removed;
}

bool WidgetManager::PruneGroup(const std::wstring& groupId)
{
    if (!Settings::IsAutoPruneDead())
        return false;
    for (auto& g : m_groups)
    {
        if (g.id != groupId) continue;
        std::wstring groupsDir = GetGroupsDir();
        bool changed = false;
        for (auto sit = g.shortcuts.begin(); sit != g.shortcuts.end(); )
        {
            if (IsShortcutAlive(*sit)) { ++sit; continue; }
            EraseStoredLnk(groupsDir, *sit);
            sit = g.shortcuts.erase(sit);
            changed = true;
        }
        if (g.shortcuts.empty())
        {
            std::wstring goneName = g.name;
            // NOTE: сносит и виджет — вызывателю нельзя трогать this!
            RemoveGroup(groupId);
            WCHAR buf[512];
            swprintf_s(buf, Lang::Get(Str::W_PrunedMsg), goneName.c_str());
            NotifyBalloon(Lang::Get(Str::W_PrunedTitle), buf);
            return true;
        }
        if (changed)
        {
            SaveGroups();
            for (auto* w : m_widgets)
            {
                if (w->GetGroupId() == groupId)
                {
                    w->Update(g);
                    break;
                }
            }
            if (PopupWindow* p = FindPopupByGroupId(groupId))
                p->Update(g);
        }
        return false;
    }
    return false;
}

bool WidgetManager::RemoveDeadShortcut(const std::wstring& groupId, int index)
{
    for (auto& g : m_groups)
    {
        if (g.id != groupId) continue;
        if (index < 0 || index >= (int)g.shortcuts.size()) return false;
        EraseStoredLnk(GetGroupsDir(), g.shortcuts[index]);
        g.shortcuts.erase(g.shortcuts.begin() + index);
        if (g.shortcuts.empty())
        {
            RemoveGroup(groupId);
            return true;
        }
        SaveGroups();
        for (auto* w : m_widgets)
        {
            if (w->GetGroupId() == groupId)
            {
                w->Update(g);
                break;
            }
        }
        return true;
    }
    return false;
}

bool WidgetManager::RefreshGroupFromDisk(const std::wstring& groupId)
{
    for (auto& g : m_groups)
    {
        if (g.id != groupId) continue;
        bool changed = false;
        if (Settings::IsAutoPruneDead())
        {
            std::wstring groupsDir = GetGroupsDir();
            for (auto sit = g.shortcuts.begin(); sit != g.shortcuts.end(); )
            {
                if (IsShortcutAlive(*sit)) { ++sit; continue; }
                EraseStoredLnk(groupsDir, *sit);
                sit = g.shortcuts.erase(sit);
                changed = true;
            }
        }
        if (g.shortcuts.empty())
        {
            RemoveGroup(groupId);
            return true;
        }
        // Имена/цели могли поменяться снаружи (переименование в shell-меню).
        for (auto& s : g.shortcuts)
        {
            if (s.lnkPath.empty() ||
                GetFileAttributesW(s.lnkPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            ShortcutInfo fresh = ExtractLnkInfo(s.lnkPath);
            if (!fresh.name.empty() && fresh.name != s.name)
            {
                s.name = fresh.name;
                changed = true;
            }
            if (!fresh.targetPath.empty() && fresh.targetPath != s.targetPath)
            {
                s.targetPath = fresh.targetPath;
                changed = true;
            }
        }
        if (changed)
        {
            ApplySortMode(g);
            SaveGroups();
            for (auto* w : m_widgets)
            {
                if (w->GetGroupId() == groupId)
                {
                    w->Update(g);
                    break;
                }
            }
        }
        if (PopupWindow* p = FindPopupByGroupId(groupId))
            p->Update(g);
        return false;
    }
    return true;
}

bool WidgetManager::MoveShortcutInGroup(const std::wstring& groupId, int from, int to)
{
    for (auto& g : m_groups)
    {
        if (g.id != groupId) continue;
        int n = (int)g.shortcuts.size();
        if (from < 0 || from >= n) return false;
        if (to < 0) to = 0;
        if (to >= n) to = n - 1;
        if (from == to) return true;

        ShortcutInfo si = std::move(g.shortcuts[from]);
        g.shortcuts.erase(g.shortcuts.begin() + from);
        g.shortcuts.insert(g.shortcuts.begin() + to, std::move(si));
        // Ручной порядок отменяет автосортировку (как в проводнике)
        g.sortMode = (int)GroupSortMode::Off;
        SaveGroups();

        for (auto* w : m_widgets)
        {
            if (w->GetGroupId() == groupId)
            {
                w->Update(g);
                break;
            }
        }
        if (PopupWindow* p = FindPopupByGroupId(groupId))
            p->Update(g);
        return true;
    }
    return false;
}

void WidgetManager::ApplySortMode(GroupData& group)
{
    GroupSortMode mode = (GroupSortMode)group.sortMode;
    if (mode == GroupSortMode::Off) return;

    auto targetExt = [](const ShortcutInfo& s) -> std::wstring {
        PCWSTR src = s.targetPath.empty() ? s.lnkPath.c_str() : s.targetPath.c_str();
        PCWSTR ext = PathFindExtensionW(src);
        return ext ? ext : L"";
    };

    switch (mode)
    {
    case GroupSortMode::NameAsc:
        std::stable_sort(group.shortcuts.begin(), group.shortcuts.end(),
            [](const ShortcutInfo& a, const ShortcutInfo& b) {
                return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
            });
        break;
    case GroupSortMode::NameDesc:
        std::stable_sort(group.shortcuts.begin(), group.shortcuts.end(),
            [](const ShortcutInfo& a, const ShortcutInfo& b) {
                return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) > 0;
            });
        break;
    case GroupSortMode::Type:
        std::stable_sort(group.shortcuts.begin(), group.shortcuts.end(),
            [&targetExt](const ShortcutInfo& a, const ShortcutInfo& b) {
                std::wstring ea = targetExt(a);
                std::wstring eb = targetExt(b);
                int c = _wcsicmp(ea.c_str(), eb.c_str());
                if (c != 0) return c < 0;
                return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
            });
        break;
    case GroupSortMode::Recent:
        // Недавние сверху: свежий lastUsed первый, никогда не запускавшиеся —
        // в конец, среди равных — по имени. stable_sort держит ручной порядок.
        std::stable_sort(group.shortcuts.begin(), group.shortcuts.end(),
            [](const ShortcutInfo& a, const ShortcutInfo& b) {
                if (a.lastUsed != b.lastUsed) return a.lastUsed > b.lastUsed;
                return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
            });
        break;
    default:
        break;
    }
}

bool WidgetManager::ApplySort(const std::wstring& groupId, GroupSortMode mode)
{
    for (auto& g : m_groups)
    {
        if (g.id != groupId) continue;
        g.sortMode = (int)mode;
        ApplySortMode(g);
        SaveGroups();

        for (auto* w : m_widgets)
        {
            if (w->GetGroupId() == groupId)
            {
                w->Update(g);
                break;
            }
        }
        if (PopupWindow* p = FindPopupByGroupId(groupId))
            p->Update(g);
        return true;
    }
    return false;
}

void WidgetManager::RecordShortcutLaunch(const std::wstring& groupId, int index)
{
    for (auto& g : m_groups)
    {
        if (g.id != groupId) continue;
        if (index < 0 || index >= (int)g.shortcuts.size()) return;
        auto& s = g.shortcuts[index];
        s.uses++;
        s.lastUsed = (long long)std::time(nullptr);
        // При сортировке «недавние сверху» порядок пересчитывается сразу.
        if ((GroupSortMode)g.sortMode == GroupSortMode::Recent)
            ApplySortMode(g);
        SaveGroups();
        for (auto* w : m_widgets)
        {
            if (w->GetGroupId() == groupId)
            {
                w->Update(g);
                break;
            }
        }
        return;
    }
}

bool WidgetManager::SetGroupColor(const std::wstring& groupId, int glassColor)
{
    for (auto& g : m_groups)
    {
        if (g.id != groupId) continue;
        g.glassColor = glassColor;
        SaveGroups();

        for (auto* w : m_widgets)
        {
            if (w->GetGroupId() == groupId)
            {
                w->Update(g);
                break;
            }
        }
        if (PopupWindow* p = FindPopupByGroupId(groupId))
            p->Update(g);
        return true;
    }
    return false;
}

bool WidgetManager::SetHideName(const std::wstring& groupId, bool hide)
{
    for (auto& g : m_groups)
    {
        if (g.id != groupId) continue;
        g.hideName = hide;
        SaveGroups();

        for (auto* w : m_widgets)
        {
            if (w->GetGroupId() == groupId)
            {
                // Update перерисовывает и ресайзит окно под новую высоту
                w->Update(g);
                break;
            }
        }
        return true;
    }
    return false;
}

bool WidgetManager::SetOverflowBadge(const std::wstring& groupId, bool show)
{
    for (auto& g : m_groups)
    {
        if (g.id != groupId) continue;
        g.showOverflow = show;
        SaveGroups();

        for (auto* w : m_widgets)
        {
            if (w->GetGroupId() == groupId)
            {
                w->Update(g);
                break;
            }
        }
        return true;
    }
    return false;
}

bool WidgetManager::SetGridSize(const std::wstring& groupId, int grid)
{
    if (grid < 2) grid = 2;
    if (grid > 3) grid = 3;
    for (auto& g : m_groups)
    {
        if (g.id != groupId) continue;
        if (g.gridSize == grid) return true;
        g.gridSize = grid;
        SaveGroups();

        for (auto* w : m_widgets)
        {
            if (w->GetGroupId() == groupId)
            {
                w->Update(g);
                break;
            }
        }
        if (PopupWindow* p = FindPopupByGroupId(groupId))
            p->Update(g);
        return true;
    }
    return false;
}

bool WidgetManager::RemoveGroup(const std::wstring& groupId)
{
    auto it = std::find_if(m_groups.begin(), m_groups.end(),
        [&groupId](const GroupData& g) { return g.id == groupId; });
    if (it == m_groups.end()) return false;

    m_groups.erase(it);
    SaveGroups();
    PopupWindow::CloseAll();

    auto wit = std::find_if(m_widgets.begin(), m_widgets.end(),
        [&groupId](DesktopWidget* w) { return w->GetGroupId() == groupId; });
    if (wit != m_widgets.end())
    {
        (*wit)->Destroy();
        delete *wit;
        m_widgets.erase(wit);
    }
    // Убрать из выделения, чтобы не остался висячий id.
    m_selected.erase(groupId);

    return true;
}
const GroupData* WidgetManager::FindGroup(const std::wstring& groupId) const
{
    for (const auto& g : m_groups)
        if (g.id == groupId)
            return &g;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Мультивыделение групп (только runtime, в JSON не сохраняется)
// ---------------------------------------------------------------------------

DesktopWidget* WidgetManager::FindWidget(const std::wstring& groupId)
{
    for (auto* w : m_widgets)
        if (w && w->GetGroupId() == groupId)
            return w;
    return nullptr;
}

std::vector<DesktopWidget*> WidgetManager::SelectedWidgets()
{
    std::vector<DesktopWidget*> out;
    for (auto* w : m_widgets)
        if (w && m_selected.count(w->GetGroupId()))
            out.push_back(w);
    return out;
}

bool WidgetManager::IsSelected(const std::wstring& groupId) const
{
    return m_selected.count(groupId) != 0;
}

void WidgetManager::RefreshSelectionVisuals()
{
    for (auto* w : m_widgets)
        if (w) w->UpdateBitmap();
}

void WidgetManager::ToggleSelectGroup(const std::wstring& groupId)
{
    if (!FindWidget(groupId)) return;
    if (m_selected.count(groupId))
        m_selected.erase(groupId);
    else
        m_selected.insert(groupId);
    RefreshSelectionVisuals();
}

void WidgetManager::SelectSingleGroup(const std::wstring& groupId)
{
    if (m_selected.size() == 1 && m_selected.count(groupId))
        return; // уже выбрана одна она
    m_selected.clear();
    if (FindWidget(groupId))
        m_selected.insert(groupId);
    RefreshSelectionVisuals();
}

void WidgetManager::SetSelectedGroups(const std::vector<std::wstring>& ids)
{
    std::set<std::wstring> next;
    for (const auto& id : ids)
        if (FindWidget(id))
            next.insert(id);
    if (next == m_selected) return;
    m_selected = std::move(next);
    RefreshSelectionVisuals();
}

void WidgetManager::ClearSelection()
{
    if (m_selected.empty()) return;
    m_selected.clear();
    RefreshSelectionVisuals();
}

static std::wstring SanitizeGroupName(std::wstring name)
{
    static const wchar_t* bad = L"<>:\"/\\|?*";
    name.erase(std::remove_if(name.begin(), name.end(),
        [](wchar_t c) { return wcschr(bad, c) != nullptr; }), name.end());
    size_t a = name.find_first_not_of(L" .");
    size_t b = name.find_last_not_of(L" .");
    if (a == std::wstring::npos) return L"";
    return name.substr(a, b - a + 1);
}

static std::wstring UniqueDesktopPath(const std::wstring& desktopDir, const std::wstring& fileName)
{
    std::wstring dst = desktopDir + L"\\" + fileName;
    if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES)
        return dst;
    size_t dot = fileName.find_last_of(L'.');
    std::wstring stem = (dot == std::wstring::npos) ? fileName : fileName.substr(0, dot);
    std::wstring ext = (dot == std::wstring::npos) ? L"" : fileName.substr(dot);
    for (int n = 2; n < 1000; n++)
    {
        WCHAR buf[16];
        swprintf_s(buf, L" (%d)", n);
        dst = desktopDir + L"\\" + stem + buf + ext;
        if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES)
            return dst;
    }
    return dst;
}

bool WidgetManager::RenameGroup(const std::wstring& groupId, const std::wstring& newName)
{
    // Пустое имя разрешено (подпись просто не рисуется)
    std::wstring clean = SanitizeGroupName(newName);

    for (auto& g : m_groups)
    {
        if (g.id == groupId)
        {
            g.name = clean;
            SaveGroups();

            for (auto* w : m_widgets)
            {
                if (w->GetGroupId() == groupId)
                {
                    w->Update(g);
                    break;
                }
            }
            if (PopupWindow* p = FindPopupByGroupId(groupId))
            {
                if (p->IsVisible())
                {
                    for (const auto& ug : m_groups)
                    {
                        if (ug.id == groupId) { p->Update(ug); break; }
                    }
                }
            }
            return true;
        }
    }
    return false;
}

bool WidgetManager::UngroupGroup(const std::wstring& groupId)
{
    auto it = std::find_if(m_groups.begin(), m_groups.end(),
        [&groupId](const GroupData& g) { return g.id == groupId; });
    if (it == m_groups.end()) return false;

    WCHAR desktopPath[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0, desktopPath)))
    {
        for (const auto& s : it->shortcuts)
        {
            if (s.lnkPath.empty()) continue;
            PCWSTR fname = PathFindFileNameW(s.lnkPath.c_str());
            if (!fname || !*fname) continue;
            std::wstring dst = UniqueDesktopPath(desktopPath, fname);
            MoveFileW(s.lnkPath.c_str(), dst.c_str());
        }
    }

    // Remove possibly empty storage folder
    std::wstring groupDir = GetGroupsDir() + L"\\" + groupId;
    RemoveDirectoryW(groupDir.c_str());

    PopupWindow::CloseAll();
    bool ok = RemoveGroup(groupId);
    // Ярлыки вернулись на стол — вытолкнуть их из-под виджетов.
    PushIconsOutOfWidgets();
    return ok;
}

bool WidgetManager::DeleteGroupWithFiles(const std::wstring& groupId)
{
    auto it = std::find_if(m_groups.begin(), m_groups.end(),
        [&groupId](const GroupData& g) { return g.id == groupId; });
    if (it == m_groups.end()) return false;

    for (const auto& s : it->shortcuts)
    {
        if (!s.lnkPath.empty())
            DeletePathSilent(s.lnkPath);
    }

    std::wstring groupDir = GetGroupsDir() + L"\\" + groupId;
    DeletePathSilent(groupDir);

    PopupWindow::CloseAll();
    return RemoveGroup(groupId);
}

// Тихое рекурсивное удаление файла или папки (без корзины, без UI)
void WidgetManager::DeletePathSilent(const std::wstring& path)
{
    if (path.empty()) return;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::wstring from = path;
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&op);
}

std::wstring WidgetManager::FindGroupAtPoint(POINT pt)
{
    for (auto* w : m_widgets)
    {
        HWND hw = w->GetHwnd();
        if (!hw) continue;
        RECT rc;
        GetWindowRect(hw, &rc);
        if (PtInRect(&rc, pt))
            return w->GetGroupId();
    }
    return {};
}

void WidgetManager::SaveGroupPosition(const std::wstring& groupId, int x, int y)
{
    for (auto& g : m_groups)
    {
        if (g.id == groupId)
        {
            g.x = x;
            g.y = y;
            break;
        }
    }
    SaveGroups();
}

void WidgetManager::SavePopupSize(const std::wstring& groupId, int popupW, int popupListH)
{
    for (auto& g : m_groups)
    {
        if (g.id == groupId)
        {
            g.popupW = popupW;
            g.popupListH = popupListH;
            break;
        }
    }
    SaveGroups();
}

void WidgetManager::SnapAllWidgetsToGrid()
{
    for (auto* w : m_widgets)
    {
        if (w) w->SnapToGrid();
    }
    PushIconsOutOfWidgets();
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

void WidgetManager::CreateAllWidgets(HWND hDesktopParent)
{
    DestroyAllWidgets();
    m_hDesktopParent = hDesktopParent;
    SetDesktopParent(hDesktopParent);

    for (const auto& g : m_groups)
    {
        auto* widget = new (std::nothrow) DesktopWidget();
        if (!widget)
            continue;
        if (widget->Create(m_hDesktopParent, g, m_renderer, this))
        {
            int x = g.x;
            int y = g.y;
            if (x == -1 && y == -1)
            {
                // Координаты не заданы (а не «левый монитор»: там x < 0 валиден)
                x = 100 + (int)m_widgets.size() * 90;
                y = 100;
            }
            widget->ShowAt(x, y);
            // Виджеты — слой рабочего стола: под все окна приложений.
            // (Показ выше поднял их наверх стека; опускаем обратно.)
            SetWindowPos(widget->GetHwnd(), HWND_BOTTOM, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            m_widgets.push_back(widget);
        }
        else
        {
            // Create не удался — объект наш, удаляем (иначе утечка).
            delete widget;
        }
    }
    // Все виджеты встали — выталкиваем иконки из-под них вниз по сетке.
    PushIconsOutOfWidgets();
    ApplyWidgetsVisibility();
}

// Иконки рабочего стола из-под виджетов — вниз (no-op без конфликтов).
void WidgetManager::PushIconsOutOfWidgets()
{
    std::vector<RECT> wrs;
    for (auto* w : m_widgets)
    {
        if (!w || !w->GetHwnd()) continue;
        RECT r = {};
        if (GetWindowRect(w->GetHwnd(), &r) &&
            r.right > r.left && r.bottom > r.top)
            wrs.push_back(r);
    }
    PushDesktopIconsOutOfWidgets(wrs);
}

// Аварийный возврат иконок: упаковать видимым блоком + вытолкнуть
// из-под виджетов. Во временном процессе (флаг --rescue-icons) своих
// виджетов нет — собираем прямоугольники живых окон по классу.
void WidgetManager::RescueDesktopIcons()
{
    std::vector<RECT> wrs;
    for (auto* w : m_widgets)
    {
        if (!w || !w->GetHwnd()) continue;
        RECT r = {};
        if (GetWindowRect(w->GetHwnd(), &r) &&
            r.right > r.left && r.bottom > r.top)
            wrs.push_back(r);
    }
    if (wrs.empty())
        wrs = CollectAllWidgetRects();
    SpreadAndPushDesktopIcons(wrs);
}

void WidgetManager::DestroyAllWidgets()
{
    PopupWindow::CloseAll();
    m_selected.clear();
    for (auto* w : m_widgets)
    {
        w->Destroy();
        delete w;
    }
    m_widgets.clear();
}

void WidgetManager::RefreshWidgetIcons()
{
    for (auto* w : m_widgets)
    {
        if (!w) continue;
        const GroupData* g = FindGroup(w->GetGroupId());
        if (g) w->Update(*g);
    }
    for (const auto& g : m_groups)
    {
        if (PopupWindow* p = FindPopupByGroupId(g.id))
            p->Update(g);
    }
}

void WidgetManager::RefreshWidgets()
{
    WL(L"RefreshWidgets start");
    LoadGroups();
    if (Settings::IsAutoPruneDead())
        PruneDeadShortcuts();
    WCHAR buf[64];
    swprintf_s(buf, L"Loaded %d groups", (int)m_groups.size());
    WL(buf);
    CreateAllWidgets(m_hDesktopParent);
    swprintf_s(buf, L"Created %d widgets", (int)m_widgets.size());
    WL(buf);
}

// ---------------------------------------------------------------------------
// IPC command handling
// ---------------------------------------------------------------------------

void WidgetManager::OnGroupCommand(const std::wstring& cmdLine)
{
    WL(L"OnGroupCommand start");

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine.c_str(), &argc);
    if (!argv) { WL(L"CommandLineToArgvW failed"); return; }

    WL(L"argc parsed");

    HWND hwnd = GetForegroundWindow();
    std::wstring name;
    std::vector<std::wstring> paths;
    bool noMove = false;
    bool rescue = false;
    bool dumpWin = false;

    for (int i = 0; i < argc; i++)
    {
        if (_wcsicmp(argv[i], L"--hwnd") == 0 && i + 1 < argc)
            hwnd = (HWND)(ULONG_PTR)std::wcstoul(argv[++i], nullptr, 10);
        else if (_wcsicmp(argv[i], L"--name") == 0 && i + 1 < argc)
            name = argv[++i];
        else if (_wcsicmp(argv[i], L"--no-move") == 0)
            noMove = true;
        else if (_wcsicmp(argv[i], L"--rescue-icons") == 0)
            rescue = true;
        else if (_wcsicmp(argv[i], L"--dump-windows") == 0)
            dumpWin = true;
        else
        {
            // CommandLineToArgvW кавычки уже снял — ручной стрип портил пути.
            paths.push_back(argv[i]);
        }
    }
    LocalFree(argv);

    if (rescue)
    {
        WL(L"rescue icons command");
        RescueDesktopIcons();
        WL(L"rescue icons done");
        return;
    }
    if (dumpWin)
    {
        WL(L"dump windows command");
        DumpDesktopWindows();
        WL(L"dump windows done");
        return;
    }

    WL(L"parsed paths");

    if (paths.empty()) { WL(L"paths empty, returning"); return; }

    bool allSupported = true;
    for (auto& p : paths)
    {
        if (!IsGroupableShortcut(p)) { allSupported = false; break; }
    }
    if (!allSupported) { WL(L"not all groupable, returning"); return; }

    if (name.empty())
    {
        // Имя для отображения: как в проводнике по умолчанию — расширение
        // прячем у файлов, у папок оставляем имя целиком
        auto displayStem = [](const std::wstring& p) -> std::wstring {
            std::wstring n = p;
            size_t pos = n.find_last_of(L"\\/");
            if (pos != std::wstring::npos) n = n.substr(pos + 1);
            DWORD a = GetFileAttributesW(p.c_str());
            bool isDir = (a != INVALID_FILE_ATTRIBUTES) &&
                (a & FILE_ATTRIBUTE_DIRECTORY);
            if (!isDir)
            {
                PCWSTR ext = PathFindExtensionW(n.c_str());
                if (ext && ext != n.c_str()) n.resize(ext - n.c_str());
            }
            return n;
        };
        if (paths.size() >= 2)
        {
            name = displayStem(paths[0]) + L" & " + displayStem(paths[1]);
        }
        else
        {
            name = Lang::Get(Str::D_NewGroup);
        }
    }

    int spawnX = -1, spawnY = -1;
    POINT pt;
    if (GetCursorPos(&pt))
    {
        // Размер новой группы известен заранее (дефолтная сетка).
        GroupData probe;
        probe.gridSize = Settings::GetDefaultGrid();
        int spawnW = WidgetWidth(probe);
        int spawnH = WidgetHeight(probe) + WIDGET_LABEL_H; // с запасом под подпись
        spawnX = pt.x - spawnW / 2;
        spawnY = pt.y - spawnH / 2;

        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi))
        {
            int screenL = mi.rcWork.left;
            int screenT = mi.rcWork.top;
            int screenW = mi.rcWork.right - mi.rcWork.left;
            int screenH = mi.rcWork.bottom - mi.rcWork.top;
            if (spawnX < screenL) spawnX = screenL + 10;
            if (spawnY < screenT) spawnY = screenT + 10;
            if (spawnX + spawnW > screenL + screenW) spawnX = screenL + screenW - spawnW - 10;
            if (spawnY + spawnH > screenT + screenH) spawnY = screenT + screenH - spawnH - 10;
        }
    }

    WL(L"looking for existing group");
    for (auto& g : m_groups)
    {
        if (g.name == name)
        {
            WL(L"found existing group, adding shortcuts");
            for (const auto& lnk : paths)
            {
                ShortcutInfo si = ExtractLnkInfo(lnk);
                if (si.lnkPath.empty()) continue;
                if (!noMove)
                {
                    std::wstring stored = MoveLnkToStorage(lnk, g.id);
                    if (!stored.empty()) si.lnkPath = stored;
                }
                g.shortcuts.push_back(std::move(si));
            }
            ApplySortMode(g);
            SaveGroups();
            WL(L"existing group updated, refreshing");
            if (m_hDesktopParent && m_renderer)
                RefreshWidgets();
            return;
        }
    }

    WL(L"calling CreateGroup");
    CreateGroup(name, paths, spawnX, spawnY, noMove);
    WL(L"CreateGroup done");
}

std::wstring WidgetManager::ShowNameDialog(HWND parent)
{
    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"Static", Lang::Get(Str::D_NameTitle), WS_POPUPWINDOW | WS_CAPTION,
        0, 0, 380, 160, parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    CreateWindowW(L"STATIC", Lang::Get(Str::D_NamePrompt), WS_CHILD | WS_VISIBLE,
        20, 20, 340, 20, hDlg, 0, GetModuleHandleW(nullptr), nullptr);

    HWND hEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        20, 50, 340, 24, hDlg, (HMENU)1001, GetModuleHandleW(nullptr), nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR def[64];
    swprintf_s(def, Lang::Get(Str::D_DefaultGroup), st.wYear, st.wMonth, st.wDay);
    SetWindowTextW(hEdit, def);
    SendMessageW(hEdit, EM_SETSEL, 0, -1);

    CreateWindowW(L"BUTTON", Lang::Get(Str::D_Ok), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        190, 90, 80, 28, hDlg, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
    CreateWindowW(L"BUTTON", Lang::Get(Str::D_Cancel), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        280, 90, 80, 28, hDlg, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);

    RECT rc;
    GetWindowRect(hDlg, &rc);
    SetWindowPos(hDlg, nullptr,
        (GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(hDlg, SW_SHOW);

    bool confirmed = false;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        if (!IsWindow(hDlg)) break;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN)
        {
            confirmed = true;
            DestroyWindow(hDlg);
            break;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
        {
            DestroyWindow(hDlg);
            break;
        }
        if (IsDialogMessageW(hDlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    std::wstring result;
    if (confirmed && IsWindow(hEdit))
    {
        int len = GetWindowTextLengthW(hEdit) + 1;
        if (len > 1)
        {
            std::wstring t(len, L'\0');
            GetWindowTextW(hEdit, t.data(), len);
            t.resize(wcslen(t.c_str()));
            result = t;
        }
    }

    if (IsWindow(hDlg)) DestroyWindow(hDlg);

    static const wchar_t* bad = L"<>:\"/\\|?*";
    result.erase(std::remove_if(result.begin(), result.end(),
        [](wchar_t c) { return wcschr(bad, c) != nullptr; }), result.end());
    size_t a = result.find_first_not_of(L" .");
    size_t b = result.find_last_not_of(L" .");
    if (a == std::wstring::npos) return L"";
    return result.substr(a, b - a + 1);
}

struct RenameDlgState {
    HWND hEdit = nullptr;
    std::wstring result;
    bool confirmed = false;
    bool done = false;
};

static LRESULT CALLBACK RenameDlgWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    RenameDlgState* st = (RenameDlgState*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    switch (uMsg)
    {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK && st)
        {
            int len = GetWindowTextLengthW(st->hEdit) + 1;
            if (len > 1)
            {
                std::wstring t(len, L'\0');
                GetWindowTextW(st->hEdit, t.data(), len);
                t.resize(wcslen(t.c_str()));
                st->result = t;
            }
            st->confirmed = true;
            st->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL && st)
        {
            st->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (st) st->done = true;
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

bool WidgetManager::ShowRenameDialog(HWND parent, const std::wstring& currentName, std::wstring& outName)
{
    static bool clsRegistered = false;
    static const wchar_t* CLS = L"GroupRenameDlg";
    if (!clsRegistered)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = RenameDlgWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = CLS;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        clsRegistered = true;
    }

    RenameDlgState state;
    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        CLS, Lang::Get(Str::D_RenameTitle), WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
        0, 0, 380, 160, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hDlg) return false;
    SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)&state);

    CreateWindowW(L"STATIC", Lang::Get(Str::D_RenamePrompt), WS_CHILD | WS_VISIBLE,
        20, 20, 340, 20, hDlg, nullptr, GetModuleHandleW(nullptr), nullptr);

    state.hEdit = CreateWindowW(L"EDIT", currentName.c_str(),
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        20, 50, 340, 24, hDlg, (HMENU)1001, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEdit, EM_SETSEL, 0, -1);
    SetFocus(state.hEdit);

    CreateWindowW(L"BUTTON", Lang::Get(Str::D_Ok), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        190, 90, 80, 28, hDlg, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
    CreateWindowW(L"BUTTON", Lang::Get(Str::D_Cancel), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        280, 90, 80, 28, hDlg, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hDlg, HWND_TOPMOST, (sw - 380) / 2, (sh - 160) / 2, 0, 0,
        SWP_NOSIZE | SWP_SHOWWINDOW);

    MSG msg;
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0))
    {
        if (!IsWindow(hDlg)) break;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
        {
            state.done = true;
            DestroyWindow(hDlg);
            break;
        }
        if (IsDialogMessageW(hDlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(hDlg)) DestroyWindow(hDlg);

    if (!state.confirmed) return false;
    outName = SanitizeGroupName(state.result); // может быть пустым — разрешено
    return true;
}

bool WidgetManager::ShowFileRenameDialog(HWND parent, const std::wstring& currentFileName,
    std::wstring& outName)
{
    // Тот же каркас, что ShowRenameDialog, но с файловыми строками.
    // Пустое имя запрещено (файл без имени невозможен).
    static bool clsRegistered = false;
    static const wchar_t* CLS = L"GroupFileRenameDlg";
    if (!clsRegistered)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = RenameDlgWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = CLS;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        clsRegistered = true;
    }

    RenameDlgState state;
    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        CLS, Lang::Get(Str::D_FileRenameTitle), WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
        0, 0, 380, 160, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hDlg) return false;
    SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)&state);

    CreateWindowW(L"STATIC", Lang::Get(Str::D_FileRenamePrompt), WS_CHILD | WS_VISIBLE,
        20, 20, 340, 20, hDlg, nullptr, GetModuleHandleW(nullptr), nullptr);

    state.hEdit = CreateWindowW(L"EDIT", currentFileName.c_str(),
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        20, 50, 340, 24, hDlg, (HMENU)1001, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEdit, EM_SETSEL, 0, -1);
    SetFocus(state.hEdit);

    CreateWindowW(L"BUTTON", Lang::Get(Str::D_Ok), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        190, 90, 80, 28, hDlg, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
    CreateWindowW(L"BUTTON", Lang::Get(Str::D_Cancel), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        280, 90, 80, 28, hDlg, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hDlg, HWND_TOPMOST, (sw - 380) / 2, (sh - 160) / 2, 0, 0,
        SWP_NOSIZE | SWP_SHOWWINDOW);

    MSG msg;
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0))
    {
        if (!IsWindow(hDlg)) break;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
        {
            state.done = true;
            DestroyWindow(hDlg);
            break;
        }
        if (IsDialogMessageW(hDlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(hDlg)) DestroyWindow(hDlg);

    if (!state.confirmed) return false;
    std::wstring clean = SanitizeGroupName(state.result);
    if (clean.empty()) return false;
    outName = clean;
    return true;
}

void WidgetManager::OnSettingsToggle()
{
    // Без владельца: предсказуемый z-order и своя кнопка в таскбаре.
    // Повторный клик поднимает уже открытое окно (см. синглтон в Show).
    SettingsDialog::SetManager(this);
    SettingsDialog::Show(nullptr);
}

void WidgetManager::NotifyBalloon(const std::wstring& title, const std::wstring& text)
{
    if (!m_hNotifyWnd || !IsWindow(m_hNotifyWnd)) return;
    TrayBalloon* b = new TrayBalloon{ title, text };
    if (!PostMessageW(m_hNotifyWnd, WM_TRAY_BALLOON_MSG, 0, (LPARAM)b))
        delete b;
}

void WidgetManager::ShowSearch()
{
    if (!m_search)
        m_search = new SearchWindow();
    m_search->Show(this);
}

void WidgetManager::ToggleSearch()
{
    if (m_search && m_search->IsVisible())
        m_search->Hide();
    else
        ShowSearch();
}

void WidgetManager::ApplyWidgetsVisibility()
{
    for (auto* w : m_widgets)
    {
        if (!w || !w->GetHwnd()) continue;
        ShowWindow(w->GetHwnd(), m_widgetsHidden ? SW_HIDE : SW_SHOWNA);
    }
    if (m_widgetsHidden)
        PopupWindow::CloseAll();
}

void WidgetManager::ToggleWidgetsVisible()
{
    m_widgetsHidden = !m_widgetsHidden;
    ApplyWidgetsVisibility();
}
