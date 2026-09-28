#include "Updater.h"
#include "Version.h"
#include "WidgetTypes.h"
#include "TrayIcon.h"
#include "Lang.h"
#include "Reg.h"
#include "Logger.h"
#include <wininet.h>
#include <urlmon.h>
#include <shellapi.h>
#include <thread>
#include <mutex>
#include <vector>
#include <ctime>
#include <new>

#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "Urlmon.lib")

namespace {

const wchar_t* REG_APP = L"Software\\DesktopGroupManager";
const wchar_t* kApiUrl = L"https://api.github.com/repos/ivintr/ShortcutGrouper/releases/latest";
const long kCheckPeriodSec = 24 * 3600;

HWND g_hNotify = nullptr;
std::mutex g_mu;
bool g_hasUpdate = false;
bool g_downloading = false;
std::wstring g_updateVersion;
std::wstring g_updateUrl;

void Balloon(const wchar_t* title, const wchar_t* text)
{
    // g_hNotify пишется один раз из UI-потока до старта фоновых потоков.
    HWND h = g_hNotify;
    if (!h || !title || !text) return;
    TrayBalloon* b = new (std::nothrow) TrayBalloon{ title, text };
    if (b)
    {
        // Не влезло в очередь — чистим сами, иначе утечка.
        if (!PostMessageW(h, WM_TRAY_BALLOON_MSG, 0, (LPARAM)b))
            delete b;
    }
}

bool ParseVersion(const std::wstring& s, int out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    size_t i = 0;
    if (i < s.size() && (s[i] == L'v' || s[i] == L'V')) i++;
    for (int k = 0; k < 4; k++)
    {
        if (i >= s.size() || s[i] < L'0' || s[i] > L'9')
            return k > 0; // "1" или "1.0" — тоже версия
        int v = 0;
        while (i < s.size() && s[i] >= L'0' && s[i] <= L'9')
        {
            v = v * 10 + (s[i] - L'0');
            if (v > 1000000) return false;
            i++;
        }
        out[k] = v;
        if (i < s.size() && s[i] == L'.') i++;
        else break;
    }
    return true;
}

bool IsNewer(const std::wstring& candidate)
{
    int cur[4] = {}, nxt[4] = {};
    if (!ParseVersion(SHORTCUT_GROUPER_VERSION_W, cur)) return false;
    if (!ParseVersion(candidate, nxt)) return false;
    for (int k = 0; k < 4; k++)
    {
        if (nxt[k] != cur[k]) return nxt[k] > cur[k];
    }
    return false;
}

// Минимальный JSON-парсинг ответа releases/latest: tag_name и первый
// browser_download_url с "Setup" и ".exe". Полноценный парсер ради двух
// полей не тянем.
bool ParseRelease(const std::string& json, std::wstring& tag, std::wstring& url)
{
    auto narrow = [](const std::string& s) {
        std::wstring r;
        r.reserve(s.size());
        for (unsigned char c : s) r += (wchar_t)c;
        return r;
    };
    size_t p = json.find("\"tag_name\"");
    if (p == std::string::npos) return false;
    p = json.find('"', p + 10);
    if (p == std::string::npos) return false;
    size_t q = json.find('"', p + 1);
    if (q == std::string::npos) return false;
    tag = narrow(json.substr(p + 1, q - p - 1));

    size_t u = 0;
    while ((u = json.find("\"browser_download_url\"", u)) != std::string::npos)
    {
        size_t a = json.find('"', u + 22);
        if (a == std::string::npos) break;
        size_t b = json.find('"', a + 1);
        if (b == std::string::npos) break;
        std::string link = json.substr(a + 1, b - a - 1);
        u = b + 1;
        if (link.find("Setup") != std::string::npos &&
            link.size() >= 4 &&
            link.compare(link.size() - 4, 4, ".exe") == 0)
        {
            url = narrow(link);
            return true;
        }
    }
    return false;
}

std::string FetchUrl(const wchar_t* url)
{
    std::string out;
    HINTERNET hNet = InternetOpenW(L"ShortcutGrouper", INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr, nullptr, 0);
    if (!hNet) return out;
    DWORD to = 15000;
    InternetSetOptionW(hNet, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionW(hNet, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    HINTERNET hUrl = InternetOpenUrlW(hNet, url, nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (hUrl)
    {
        char buf[4096];
        DWORD got = 0;
        while (out.size() < 256 * 1024 &&
            InternetReadFile(hUrl, buf, sizeof(buf), &got) && got > 0)
        {
            out.append(buf, got);
        }
        InternetCloseHandle(hUrl);
    }
    InternetCloseHandle(hNet);
    return out;
}

__int64 NowUnix()
{
    return (__int64)_time64(nullptr);
}

void CheckThread(bool force)
{
    HKEY hKey = nullptr;
    if (!force)
    {
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_APP, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            DWORD last = Reg::GetDword(hKey, L"LastUpdateCheck", 0);
            RegCloseKey(hKey);
            hKey = nullptr;
            if ((__int64)last > 0 && NowUnix() - (__int64)last < kCheckPeriodSec)
                return; // проверялись недавно
        }
    }
    std::string json = FetchUrl(kApiUrl);
    if (!json.empty())
    {
        // Троттлим и неуспех: не долбим API при каждой загрузке.
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_APP, 0, KEY_READ | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        {
            if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_APP, 0, nullptr, 0,
                    KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
                hKey = nullptr;
        }
        if (hKey)
        {
            Reg::SetDword(hKey, L"LastUpdateCheck", (DWORD)NowUnix());
            RegCloseKey(hKey);
        }

        std::wstring tag, url;
        if (ParseRelease(json, tag, url) && IsNewer(tag))
        {
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_hasUpdate = true;
                g_updateVersion = tag;
                if (!g_updateVersion.empty() &&
                    (g_updateVersion[0] == L'v' || g_updateVersion[0] == L'V'))
                    g_updateVersion.erase(0, 1);
                g_updateUrl = url;
            }
            WCHAR msg[256];
            swprintf_s(msg, Lang::Get(Str::U_AvailMsg), tag.c_str());
            Balloon(Lang::Get(Str::U_AvailTitle), msg);
            AppLog(L"UPD", L"update available: %s", tag.c_str());
        }
        else if (force)
        {
            Balloon(Lang::Get(Str::U_UptodateTitle), Lang::Get(Str::U_UptodateMsg));
        }
    }
    else if (force)
    {
        Balloon(Lang::Get(Str::W_ErrorCaption), Lang::Get(Str::U_NetFailMsg));
    }
}

void DownloadThread(HWND hExitWnd, std::wstring url, std::wstring ver)
{
    WCHAR tmp[MAX_PATH] = {};
    DWORD tn = GetTempPathW(MAX_PATH, tmp);
    if (tn == 0 || tn >= MAX_PATH)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_downloading = false;
        Balloon(Lang::Get(Str::W_ErrorCaption), Lang::Get(Str::U_DownFailMsg));
        return;
    }
    std::wstring dst = std::wstring(tmp) + L"ShortcutGrouper-Setup-" + ver + L".exe";
    HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), dst.c_str(), 0, nullptr);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_downloading = false;
    }
    if (SUCCEEDED(hr))
    {
        AppLog(L"UPD", L"downloaded %s", dst.c_str());
        HINSTANCE rc = ShellExecuteW(nullptr, L"open", dst.c_str(), nullptr, nullptr, SW_SHOW);
        if ((INT_PTR)rc <= 32)
        {
            AppLog(L"UPD", L"installer launch failed: %d", (int)(INT_PTR)rc);
            Balloon(Lang::Get(Str::W_ErrorCaption), Lang::Get(Str::U_DownFailMsg));
            return;
        }
        // Установщик сам всё обновит; наш процесс ему мешает (файлы заняты).
        PostMessageW(hExitWnd, WM_COMMAND, MAKEWPARAM(IDM_EXIT, 0), 0);
    }
    else
    {
        AppLog(L"UPD", L"download failed: 0x%08X", hr);
        Balloon(Lang::Get(Str::W_ErrorCaption), Lang::Get(Str::U_DownFailMsg));
    }
}

} // namespace

void Updater::SetNotifyWindow(HWND hWnd)
{
    g_hNotify = hWnd;
}

void Updater::CheckAsync(bool force)
{
    try
    {
        std::thread(CheckThread, force).detach();
    }
    catch (...)
    {
        // Нет потока — нет проверки, приложение работает дальше.
    }
}

bool Updater::HasUpdate()
{
    std::lock_guard<std::mutex> lk(g_mu);
    return g_hasUpdate;
}

std::wstring Updater::UpdateVersion()
{
    std::lock_guard<std::mutex> lk(g_mu);
    return g_updateVersion;
}

bool Updater::InstallUpdate(HWND hExitWnd)
{
    std::wstring url, ver;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_hasUpdate || g_downloading || g_updateUrl.empty())
            return false;
        g_downloading = true;
        url = g_updateUrl;
        ver = g_updateVersion;
    }
    try
    {
        std::thread(DownloadThread, hExitWnd, url, ver).detach();
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_downloading = false;
        return false;
    }
    return true;
}

const wchar_t* Updater::CurrentVersion()
{
    return SHORTCUT_GROUPER_VERSION_W;
}
