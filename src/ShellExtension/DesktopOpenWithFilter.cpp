#include "DesktopOpenWithFilter.h"
#include "ClassFactory.h"
#include <Shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include "LogHelper.h"

#pragma comment(lib, "Shlwapi.lib")

DesktopOpenWithFilter::DesktopOpenWithFilter() : m_cRef(1), m_isDesktop(false) { DllAddRef(); }
DesktopOpenWithFilter::~DesktopOpenWithFilter() { DllRelease(); }

// --- IUnknown ---
IFACEMETHODIMP DesktopOpenWithFilter::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    static const QITAB qit[] = {
        QITABENT(DesktopOpenWithFilter, IShellExtInit),
        QITABENT(DesktopOpenWithFilter, IContextMenu),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP_(ULONG) DesktopOpenWithFilter::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

IFACEMETHODIMP_(ULONG) DesktopOpenWithFilter::Release()
{
    ULONG r = InterlockedDecrement(&m_cRef);
    if (!r) delete this;
    return r;
}

// --- IShellExtInit ---
IFACEMETHODIMP DesktopOpenWithFilter::Initialize(
    PCIDLIST_ABSOLUTE pidlFolder, IDataObject*, HKEY)
{
    m_isDesktop = false;

    if (!pidlFolder)
    {
        // pidlFolder is NULL when invoked from the Desktop background itself
        m_isDesktop = true;
        LogInfo(L"DOF: Initialize - pidlFolder NULL => desktop");
        return S_OK;
    }

    // Get Desktop PIDL
    PIDLIST_ABSOLUTE pidlDesktop = nullptr;
    if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &pidlDesktop)))
    {
        m_isDesktop = ILIsEqual(pidlFolder, pidlDesktop);
        CoTaskMemFree(pidlDesktop);
    }

    LogInfo(L"DOF: Initialize - isDesktop=%d", m_isDesktop);
    return S_OK;
}

// --- IContextMenu ---
IFACEMETHODIMP DesktopOpenWithFilter::QueryContextMenu(
    HMENU hMenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags)
{
    if (!m_isDesktop)
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);

    LogInfo(L"DOF: QueryContextMenu - scanning menu on Desktop, idCmdFirst=%u, count=%d",
        idCmdFirst, GetMenuItemCount(hMenu));

    int count = GetMenuItemCount(hMenu);
    if (count < 0)
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);

    int removed = 0;

    for (int i = count - 1; i >= 0; i--)
    {
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_STRING | MIIM_ID | MIIM_SUBMENU;

        wchar_t text[256] = {};
        mii.cch = 255;
        mii.dwTypeData = text;

        if (!GetMenuItemInfoW(hMenu, i, TRUE, &mii))
            continue;

        wchar_t lowerText[256] = {};
        wcscpy_s(lowerText, text);
        _wcslwr_s(lowerText, 256);

        // Match "open with" / "открыть с помощью" / "openas" patterns.
        // Скобки обязательны: && приоритетнее ||, без них "показать" без
        // "открыть" тоже давало match и сносило чужие пункты меню.
        bool match = wcsstr(lowerText, L"open with") ||
                     wcsstr(lowerText, L"открыть с помощью") ||
                     wcsstr(lowerText, L"openas") ||
                     (wcsstr(lowerText, L"показать") && wcsstr(lowerText, L"открыть"));

        if (match && mii.hSubMenu == nullptr)
        {
            LogInfo(L"DOF: Removing menu item [%s] at index %d (idCmd=%u)", text, i, mii.wID);
            DeleteMenu(hMenu, i, MF_BYPOSITION);
            removed++;
        }
    }

    LogInfo(L"DOF: Removed %d items", removed);

    // We don't add any new items — return 0 items added
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
}

IFACEMETHODIMP DesktopOpenWithFilter::InvokeCommand(LPCMINVOKECOMMANDINFO)
{
    return E_NOTIMPL;
}

IFACEMETHODIMP DesktopOpenWithFilter::GetCommandString(
    UINT_PTR, UINT, UINT*, LPSTR, UINT)
{
    return E_NOTIMPL;
}
