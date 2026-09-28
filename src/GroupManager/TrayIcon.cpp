#include "TrayIcon.h"
#include "ModernMenu.h"
#include "Renderer.h"
#include "Updater.h"
#include "Lang.h"
#include "../../resources/resource.h"
#include <shellapi.h>
#include <new>

#pragma comment(lib, "Shell32.lib")

void TrayIcon::Create(HINSTANCE hInst, HWND hOwner, UINT msgId)
{
    // Повторный Create без Remove — утечка m_nid и orphan-иконка.
    if (m_nid)
        Remove();
    m_msgId = msgId;
    auto* pnid = new (std::nothrow) NOTIFYICONDATAW;
    if (!pnid)
        return;
    m_nid = pnid;
    auto& nid = *pnid;

    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hOwner;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = msgId;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!nid.hIcon)
        nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION); // запасной вариант
    wcscpy_s(nid.szTip, L"Desktop Group Manager");

    if (!Shell_NotifyIconW(NIM_ADD, &nid))
        OutputDebugStringW(L"TrayIcon: NIM_ADD failed (Explorer may not be ready)\n");
}

void TrayIcon::ReAdd()
{
    // Пересоздание иконки после рестарта Explorer (иначе при автозапуске
    // или падении Explorer иконка пропадает навсегда).
    if (m_nid)
        Shell_NotifyIconW(NIM_ADD, (NOTIFYICONDATAW*)m_nid);
}

void TrayIcon::Remove()
{
    if (m_nid)
    {
        Shell_NotifyIconW(NIM_DELETE, (NOTIFYICONDATAW*)m_nid);
        delete (NOTIFYICONDATAW*)m_nid;
        m_nid = nullptr;
    }
}

// Подпись пункта обновлений: если версия известна — с номером.
static std::wstring UpdateMenuLabel()
{
    std::wstring s = Lang::Get(Str::T_Update);
    if (Updater::HasUpdate() && !Updater::UpdateVersion().empty())
        s += L" (" + Updater::UpdateVersion() + L")";
    return s;
}

void TrayIcon::ShowBalloon(const wchar_t* title, const wchar_t* text)
{
    if (!m_nid) return;
    auto& nid = *(NOTIFYICONDATAW*)m_nid;
    nid.uFlags |= NIF_INFO;
    wcsncpy_s(nid.szInfo, text ? text : L"", _TRUNCATE);
    wcsncpy_s(nid.szInfoTitle, title ? title : L"", _TRUNCATE);
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout = 10000;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    nid.uFlags &= ~NIF_INFO; // дальше — обычные операции без текста
    nid.szInfo[0] = 0;
    nid.szInfoTitle[0] = 0;
}

void TrayIcon::ShowMenu(HWND hWnd, bool widgetsHidden)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_SEARCH, Lang::Get(Str::T_Search));
    AppendMenuW(hMenu, MF_STRING, IDM_TOGGLE_VIS,
        Lang::Get(widgetsHidden ? Str::T_ShowWidgets : Str::T_HideWidgets));
    AppendMenuW(hMenu, MF_STRING, IDM_REFRESH, Lang::Get(Str::T_Refresh));
    AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS, Lang::Get(Str::T_Settings));
    AppendMenuW(hMenu, MF_STRING, IDM_UPDATE, UpdateMenuLabel().c_str());
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, Lang::Get(Str::T_Exit));

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
}

int TrayIcon::ShowModernMenu(HWND hWnd, WidgetRenderer* renderer, bool widgetsHidden)
{
    if (!renderer)
    {
        ShowMenu(hWnd, widgetsHidden);
        return 0;
    }

    // Контекстное меню трея в стиле Windows 11.
    ModernMenu menu(renderer);
    menu.items.push_back({ L"Desktop Group Manager", 0, false, false, false, true });
    menu.items.push_back({ L"", 0, false, false, true });
    menu.items.push_back({ Lang::Get(Str::T_Search), IDM_SEARCH });
    menu.items.push_back({ Lang::Get(widgetsHidden ? Str::T_ShowWidgets : Str::T_HideWidgets), IDM_TOGGLE_VIS });
    menu.items.push_back({ Lang::Get(Str::T_Refresh), IDM_REFRESH });
    menu.items.push_back({ Lang::Get(Str::T_Settings), IDM_SETTINGS });
    menu.items.push_back({ UpdateMenuLabel(), IDM_UPDATE });
    menu.items.push_back({ L"", 0, false, false, true });
    menu.items.push_back({ Lang::Get(Str::T_Exit), IDM_EXIT });

    POINT pt;
    GetCursorPos(&pt);
    int cmd = menu.Show(pt.x, pt.y);
    if (cmd != 0)
        PostMessageW(hWnd, WM_COMMAND, MAKEWPARAM((WORD)cmd, 0), 0);
    return cmd;
}
