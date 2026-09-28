#pragma once
#include <Windows.h>
#include <ShObjIdl.h>
#include <shlwapi.h>
#include <string>
#include <vector>

DEFINE_GUID(CLSID_GroupCommand,
    0xb5e3c5a1, 0x7d4f, 0x4e8b,
    0x9a, 0x2c, 0x1f, 0x6d, 0x8e, 0x3b, 0x5a, 0x7c);

void SetModuleHandle(HINSTANCE hInst);
HINSTANCE GetModuleHandle_();

class GroupCommand : public IExplorerCommand,
                     public IObjectWithSelection
{
public:
    GroupCommand();

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    IFACEMETHODIMP GetTitle(IShellItemArray* psiItemArray, LPWSTR* ppszName) override;
    IFACEMETHODIMP GetIcon(IShellItemArray* psiItemArray, LPWSTR* ppszIcon) override;
    IFACEMETHODIMP GetToolTip(IShellItemArray* psiItemArray, LPWSTR* ppszInfoTip) override;
    IFACEMETHODIMP GetCanonicalName(GUID* pguidCommandName) override;
    IFACEMETHODIMP GetState(IShellItemArray* psiItemArray, BOOL fOkToBeSlow, EXPCMDSTATE* pCmdState) override;
    IFACEMETHODIMP Invoke(IShellItemArray* psiItemArray, IBindCtx* pbc) override;
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* pFlags) override;
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** ppEnum) override;

    IFACEMETHODIMP SetSelection(IShellItemArray* psiSelection) override;
    IFACEMETHODIMP GetSelection(REFIID riid, void** ppv) override;

private:
    ~GroupCommand();
    LONG m_cRef;
    IShellItemArray* m_pSelection;
    BOOL AreAllGroupable(IShellItemArray* psiArray);
    std::vector<std::wstring> GetPaths(IShellItemArray* psiArray);
    HRESULT LaunchGroupManager(const std::vector<std::wstring>& paths, HWND hwnd);
};
