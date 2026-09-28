#pragma once
#include <Windows.h>
#include <shlwapi.h>
#include <new>

extern LONG g_cLock;

// Счётчик живых COM-объектов DLL: DllCanUnloadNow смотрит на него,
// иначе выгрузка возможна под активными объектами (краш Explorer).
inline void DllAddRef() { InterlockedIncrement(&g_cLock); }
inline void DllRelease() { InterlockedDecrement(&g_cLock); }

template<class T>
class ClassFactory : public IClassFactory
{
public:
    ClassFactory() : _ref(1) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        static const QITAB qit[] = {
            QITABENT(ClassFactory, IClassFactory),
            {0},
        };
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&_ref);
        if (!r) delete this;
        return r;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* pUnk, REFIID riid, void** ppv) override
    {
        if (pUnk) return CLASS_E_NOAGGREGATION;
        // nothrow: исключение в Shell = краш Explorer, мертвая проверка
        // `if (!pObj)` при бросающем new не спасает.
        T* pObj = new (std::nothrow) T();
        if (!pObj) return E_OUTOFMEMORY;
        HRESULT hr = pObj->QueryInterface(riid, ppv);
        pObj->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL fLock) override
    {
        if (fLock) InterlockedIncrement(&g_cLock);
        else InterlockedDecrement(&g_cLock);
        return S_OK;
    }

private:
    LONG _ref;
};
