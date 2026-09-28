#define INITGUID
#include <Windows.h>
#include <ShObjIdl.h>
#include <shlwapi.h>
#include <string>
#include "ClassFactory.h"
#include "GroupCommand.h"
#include "DesktopOpenWithFilter.h"
#include "Lang.h"
#include <new>

LONG g_cLock = 0;

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hMod);
        SetModuleHandle(hMod);
        Lang::Initialize();
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// DllGetClassObject
// ---------------------------------------------------------------------------
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (IsEqualCLSID(rclsid, CLSID_GroupCommand))
    {
        auto* pFactory = new (std::nothrow) ClassFactory<GroupCommand>();
        if (!pFactory) return E_OUTOFMEMORY;
        HRESULT hr = pFactory->QueryInterface(riid, ppv);
        pFactory->Release();
        return hr;
    }

    if (IsEqualCLSID(rclsid, CLSID_DesktopOpenWithFilter))
    {
        auto* pFactory = new (std::nothrow) ClassFactory<DesktopOpenWithFilter>();
        if (!pFactory) return E_OUTOFMEMORY;
        HRESULT hr = pFactory->QueryInterface(riid, ppv);
        pFactory->Release();
        return hr;
    }

    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow()
{
    return (g_cLock == 0) ? S_OK : S_FALSE;
}
