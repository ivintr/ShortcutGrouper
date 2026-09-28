#pragma once
#include <Windows.h>
#include <ShObjIdl.h>

DEFINE_GUID(CLSID_DesktopOpenWithFilter,
    0xD4E8C2A1, 0x3F5B, 0x4d9A,
    0xAE, 0x7C, 0x3B, 0x1D, 0xF2, 0x9E, 0x6C, 0x04);

class DesktopOpenWithFilter : public IShellExtInit,
                              public IContextMenu
{
public:
    DesktopOpenWithFilter();

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IShellExtInit
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE pidlFolder, IDataObject* pDataObj, HKEY hKeyProgID) override;

    // IContextMenu
    IFACEMETHODIMP QueryContextMenu(HMENU hMenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags) override;
    IFACEMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO pici) override;
    IFACEMETHODIMP GetCommandString(UINT_PTR idCmd, UINT uFlags, UINT* pwReserved, LPSTR pszName, UINT cchMax) override;

private:
    ~DesktopOpenWithFilter();
    LONG m_cRef;
    bool m_isDesktop;
};
