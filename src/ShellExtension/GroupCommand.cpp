#include "GroupCommand.h"
#include "ClassFactory.h"
#include "Lang.h"
#include "ShellQuote.h"
#include <Shlwapi.h>
#include <strsafe.h>
#include <vector>

#pragma comment(lib, "Shlwapi.lib")

static HINSTANCE s_hInst = nullptr;

void SetModuleHandle(HINSTANCE hInst) { s_hInst = hInst; }
HINSTANCE GetModuleHandle_() { return s_hInst; }

// --- Construction ---
GroupCommand::GroupCommand() : m_cRef(1), m_pSelection(nullptr) { DllAddRef(); }
GroupCommand::~GroupCommand() { if (m_pSelection) m_pSelection->Release(); DllRelease(); }

// --- IUnknown ---
IFACEMETHODIMP GroupCommand::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    static const QITAB qit[] = {
        QITABENTMULTI(GroupCommand, IUnknown, IExplorerCommand),
        QITABENT(GroupCommand, IExplorerCommand),
        QITABENT(GroupCommand, IObjectWithSelection),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP_(ULONG) GroupCommand::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

IFACEMETHODIMP_(ULONG) GroupCommand::Release()
{
    ULONG r = InterlockedDecrement(&m_cRef);
    if (!r) delete this;
    return r;
}

// --- IExplorerCommand ---
IFACEMETHODIMP GroupCommand::GetTitle(IShellItemArray*, LPWSTR* ppszName)
{
    if (!ppszName) return E_POINTER;
    return SHStrDupW(Lang::Get(Str::SH_Title), ppszName);
}

static std::wstring GetModuleDir()
{
    // Динамический буфер: MAX_PATH обрезает длинные пути, а дальше
    // PathRemoveFileSpec режет уже обрезанный путь.
    std::vector<WCHAR> buf(1024);
    for (;;)
    {
        DWORD n = GetModuleFileNameW(s_hInst ? s_hInst : GetModuleHandleW(L"ShellExtension.dll"),
            buf.data(), (DWORD)buf.size());
        if (n == 0) return L"";
        if (n < buf.size() - 1)
        {
            std::wstring dir(buf.data(), n);
            size_t p = dir.find_last_of(L"\\/");
            if (p != std::wstring::npos) dir.resize(p);
            return dir;
        }
        if (buf.size() >= 32768) return L"";
        buf.resize(buf.size() * 2);
    }
}

static void GetGroupManagerIconPath(WCHAR* out, DWORD cch)
{
    out[0] = 0;
    std::wstring dir = GetModuleDir();
    if (dir.empty()) return;
    WCHAR exe[32768] = {};
    if (FAILED(StringCchPrintfW(exe, ARRAYSIZE(exe), L"%s\\GroupManager.exe,0", dir.c_str())))
        return;
    // Иконка из нашего exe — в командной панели Win11 выглядит нативно.
    // Если exe рядом нет (только DLL), откатываемся на системную.
    std::wstring exeOnly = dir + L"\\GroupManager.exe";
    if (GetFileAttributesW(exeOnly.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    StringCchCopyW(out, cch, exe);
}

IFACEMETHODIMP GroupCommand::GetIcon(IShellItemArray*, LPWSTR* ppszIcon)
{
    if (!ppszIcon) return E_POINTER;
    WCHAR icon[MAX_PATH * 2] = {};
    GetGroupManagerIconPath(icon, ARRAYSIZE(icon));
    if (!icon[0])
        StringCchCopyW(icon, ARRAYSIZE(icon), L"imageres.dll,-112");
    return SHStrDupW(icon, ppszIcon);
}

IFACEMETHODIMP GroupCommand::GetToolTip(IShellItemArray*, LPWSTR* ppszInfoTip)
{
    if (!ppszInfoTip) return E_POINTER;
    return SHStrDupW(Lang::Get(Str::SH_Tooltip), ppszInfoTip);
}

IFACEMETHODIMP GroupCommand::GetCanonicalName(GUID* pguid)
{
    if (!pguid) return E_POINTER;
    *pguid = GUID_NULL;
    return S_OK;
}

IFACEMETHODIMP GroupCommand::GetState(IShellItemArray* psiArray, BOOL fOkToBeSlow, EXPCMDSTATE* pCmdState)
{
    if (!pCmdState) return E_POINTER;
    *pCmdState = ECS_HIDDEN;

    if (!psiArray)
        return S_OK;

    DWORD count = 0;
    HRESULT hr = psiArray->GetCount(&count);
    if (FAILED(hr) || count < 2)
        return S_OK;

    // Медленную проверку (обращения к диску) — только когда разрешили.
    // Иначе GetState в UI-потоке тормозит каждое открытие меню.
    if (!fOkToBeSlow)
    {
        *pCmdState = ECS_ENABLED;
        return S_OK;
    }

    if (!AreAllGroupable(psiArray))
        return S_OK;

    *pCmdState = ECS_ENABLED;
    return S_OK;
}

IFACEMETHODIMP GroupCommand::Invoke(IShellItemArray* psiArray, IBindCtx*)
{
    if (!psiArray)
        return E_INVALIDARG;

    DWORD count = 0;
    HRESULT hr = psiArray->GetCount(&count);
    if (FAILED(hr) || count < 2)
        return E_INVALIDARG;

    auto paths = GetPaths(psiArray);
    if (paths.empty())
        return E_INVALIDARG;

    HWND hwnd = GetForegroundWindow();
    return LaunchGroupManager(paths, hwnd);
}

IFACEMETHODIMP GroupCommand::GetFlags(EXPCMDFLAGS* pFlags)
{
    if (!pFlags) return E_POINTER;
    // ECF_DEFAULT: верхняя командная панель нового меню Win11,
    // а не только "Показать дополнительные параметры".
    *pFlags = ECF_DEFAULT;
    return S_OK;
}

IFACEMETHODIMP GroupCommand::EnumSubCommands(IEnumExplorerCommand** ppEnum)
{
    if (ppEnum) *ppEnum = nullptr;
    return E_NOTIMPL;
}

// --- IObjectWithSelection ---
IFACEMETHODIMP GroupCommand::SetSelection(IShellItemArray* psiSelection)
{
    if (m_pSelection)
    {
        m_pSelection->Release();
        m_pSelection = nullptr;
    }
    if (psiSelection)
    {
        m_pSelection = psiSelection;
        m_pSelection->AddRef();
    }
    return S_OK;
}

IFACEMETHODIMP GroupCommand::GetSelection(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!m_pSelection) return S_FALSE;
    return m_pSelection->QueryInterface(riid, ppv);
}

// --- Helpers ---
static bool IsGroupablePath(PCWSTR path)
{
    // Группируем любые файлы и папки: достаточно существования пути
    // (виртуальные элементы без пути отсеяны раньше через SIGDN_FILESYSPATH)
    if (!path || !*path) return false;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

BOOL GroupCommand::AreAllGroupable(IShellItemArray* psiArray)
{
    if (!psiArray) return FALSE;

    DWORD count = 0;
    if (FAILED(psiArray->GetCount(&count))) return FALSE;

    for (DWORD i = 0; i < count; i++)
    {
        IShellItem* pItem = nullptr;
        if (FAILED(psiArray->GetItemAt(i, &pItem)))
            return FALSE;

        PWSTR pszPath = nullptr;
        HRESULT hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
        pItem->Release();

        if (FAILED(hr))
            return FALSE;

        BOOL ok = IsGroupablePath(pszPath);
        CoTaskMemFree(pszPath);

        if (!ok)
            return FALSE;
    }
    return TRUE;
}

std::vector<std::wstring> GroupCommand::GetPaths(IShellItemArray* psiArray)
{
    std::vector<std::wstring> v;
    if (!psiArray) return v;

    DWORD count = 0;
    if (FAILED(psiArray->GetCount(&count))) return v;

    for (DWORD i = 0; i < count; i++)
    {
        IShellItem* pItem = nullptr;
        if (SUCCEEDED(psiArray->GetItemAt(i, &pItem)))
        {
            PWSTR pszPath = nullptr;
            if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)))
            {
                v.push_back(pszPath);
                CoTaskMemFree(pszPath);
            }
            pItem->Release();
        }
    }
    return v;
}

HRESULT GroupCommand::LaunchGroupManager(const std::vector<std::wstring>& paths, HWND hwnd)
{
    std::wstring dir = GetModuleDir();
    if (dir.empty())
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);

    std::wstring exe = dir + L"\\GroupManager.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    std::wstring cmd;
    ArgvQuote(exe, cmd);
    cmd += L" --hwnd " + std::to_wstring(reinterpret_cast<ULONG_PTR>(hwnd));
    for (auto& p : paths)
    {
        cmd += L" ";
        ArgvQuote(p, cmd);
    }
    // CreateProcess ограничен 32767 символами командной строки.
    if (cmd.size() + 1 > 32767)
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return HRESULT_FROM_WIN32(GetLastError());

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return S_OK;
}
