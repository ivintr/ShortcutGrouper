#include "DesktopGrid.h"
#include "Logger.h"
#include "WidgetTypes.h"
#include <Windows.h>
#include <commctrl.h>
#include <ShObjIdl.h>
#include <ShlObj.h>
#include <ExDisp.h>
#include <ServProv.h>
#include <Shlwapi.h>
#include <OleAcc.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>

// Статик-кэши ниже трогают из UI-потока, но без лока любой вызов с другого
// потока — гонка. Один мьютекс на все кэши файла.
static std::recursive_mutex s_gridCacheMutex;

#ifndef LVM_SETITEMPOSITION
#define LVM_SETITEMPOSITION (LVM_FIRST + 15)
#endif
#ifndef LVM_ARRANGE
#define LVM_ARRANGE (LVM_FIRST + 22)
#endif
#ifndef LVA_ALIGNLEFT
#define LVA_ALIGNLEFT 0x0001
#endif
#ifndef LVA_ALIGNTOP
#define LVA_ALIGNTOP 0x0002
#endif
#ifndef LVS_AUTOARRANGE
#define LVS_AUTOARRANGE 0x0100
#endif

#pragma comment(lib, "Comctl32.lib")

#ifndef LVM_GETITEMSPACING
#define LVM_GETITEMSPACING (LVM_FIRST + 51)
#endif

static void LogGrid(const char* stage, int v1, int v2, int v3, int v4, int v5, int v6)
{
    AppLogA("GRID", "%s: %d %d %d %d %d %d", stage, v1, v2, v3, v4, v5, v6);
}

void DebugLogGrid(const char* stage, int v1, int v2, int v3, int v4, int v5, int v6)
{
    LogGrid(stage, v1, v2, v3, v4, v5, v6);
}

static HWND FindDesktopListView()
{    HWND hScan = nullptr;
    while ((hScan = FindWindowExW(nullptr, hScan, L"WorkerW", nullptr)) != nullptr)
    {
        HWND hDef = FindWindowExW(hScan, nullptr, L"SHELLDLL_DefView", nullptr);
        if (hDef)
        {
            HWND hList = FindWindowExW(hDef, nullptr, L"SysListView32", nullptr);
            if (hList) return hList;
        }
    }
    HWND hProg = FindWindowW(L"Progman", nullptr);
    if (hProg)
    {
        HWND hDef = FindWindowExW(hProg, nullptr, L"SHELLDLL_DefView", nullptr);
        if (hDef)
        {
            HWND hList = FindWindowExW(hDef, nullptr, L"SysListView32", nullptr);
            if (hList) return hList;
        }
    }
    return nullptr;
}

// Send в чужой процесс (Explorer): обычный SendMessageW виснет навсегда,
// если Explorer подвис. Таймаут + ABORTIFHUNG, провал = 0.
// Таймаут 500мс: живой Explorer отвечает за единицы мс, а 2с на пункт
// при сотнях иконок подвешивали поток рамки надолго.
static const UINT kGridSendTimeoutMs = 500;
static LRESULT GridSend(HWND hList, UINT msg, WPARAM wp, LPARAM lp)
{
    DWORD_PTR res = 0;
    if (SendMessageTimeoutW(hList, msg, wp, lp,
            SMTO_NORMAL | SMTO_ABORTIFHUNG, kGridSendTimeoutMs, &res) == 0)
        return 0;
    return (LRESULT)(LONG_PTR)res;
}

// То же, но различает «ответ 0» и «таймаут/провал»: иначе подвисший Explorer
// даёт 0 иконок, и нажатие на файл считается кликом по пустому месту.
static bool GridSendEx(HWND hList, UINT msg, WPARAM wp, LPARAM lp, LRESULT* out)
{
    DWORD_PTR res = 0;
    if (SendMessageTimeoutW(hList, msg, wp, lp,
            SMTO_NORMAL | SMTO_ABORTIFHUNG, kGridSendTimeoutMs, &res) == 0)
        return false;
    if (out) *out = (LRESULT)(LONG_PTR)res;
    return true;
}

static int VoteDiff(const int* vals, int n)
{
    // Most frequent positive gap between sorted coords = grid pitch.
    // vals must be sorted ascending.
    struct D { int d, c; };
    D best[32] = {};
    int nb = 0;
    for (int i = 1; i < n; i++)
    {
        int d = vals[i] - vals[i - 1];
        if (d < 8) continue;
        bool found = false;
        for (int k = 0; k < nb; k++)
            if (best[k].d == d) { best[k].c++; found = true; break; }
        if (!found && nb < 32) { best[nb].d = d; best[nb].c = 1; nb++; }
    }
    if (nb == 0) return 0;
    int bi = 0;
    for (int k = 1; k < nb; k++)
        if (best[k].c > best[bi].c) bi = k;
    return best[bi].d;
}

static int CmpInt(const void* a, const void* b)
{
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

DesktopGrid QueryDesktopGrid()
{
    DesktopGrid g;

    static ULONGLONG lastQ = 0;
    static DesktopGrid cached;
    {
        std::lock_guard<std::recursive_mutex> lk(s_gridCacheMutex);
        ULONGLONG now = GetTickCount64();
        if (now - lastQ < 3000 && cached.valid)
            return cached;
        lastQ = now;
    }

    HWND hList = FindDesktopListView();
    if (!hList || !IsWindow(hList)) return g;

    LRESULT sp = GridSend(hList, LVM_GETITEMSPACING, 0, 0);
    int msgStepX = (int)(short)LOWORD(sp);
    int msgStepY = (int)(short)HIWORD(sp);

    DWORD pid = 0;
    GetWindowThreadProcessId(hList, &pid);
    if (!pid) return g;
    HANDLE hProc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!hProc) return g;

    LPVOID remote = VirtualAllocEx(hProc, nullptr, sizeof(POINT), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { CloseHandle(hProc); return g; }

    int count = (int)GridSend(hList, LVM_GETITEMCOUNT, 0, 0);
    int n = count < 32 ? count : 32;
    POINT pts[32];
    int np = 0;
    for (int i = 0; i < n; i++)
    {
        POINT zero = {};
        SIZE_T wr = 0;
        if (!WriteProcessMemory(hProc, remote, &zero, sizeof(zero), &wr) || wr != sizeof(zero))
            continue;
        if (!GridSend(hList, LVM_GETITEMPOSITION, (WPARAM)i, (LPARAM)remote))
            continue;
        POINT rp = {};
        SIZE_T rd = 0;
        if (!ReadProcessMemory(hProc, remote, &rp, sizeof(rp), &rd) || rd != sizeof(rp))
            continue;
        pts[np++] = rp;
    }
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);
    if (np <= 0) { LogGrid("noitems", count, 0, 0, 0, 0, 0); return g; }

    // Pitch: measured from real positions first, message second, metrics last.
    int xs[32], ys[32];
    for (int i = 0; i < np; i++) { xs[i] = pts[i].x; ys[i] = pts[i].y; }
    qsort(xs, np, sizeof(int), CmpInt);
    qsort(ys, np, sizeof(int), CmpInt);
    int stepX = VoteDiff(xs, np);
    int stepY = VoteDiff(ys, np);
    if (stepX < 16) stepX = (msgStepX >= 16) ? msgStepX : 0;
    if (stepY < 16) stepY = (msgStepY >= 16) ? msgStepY : 0;
    // Sanity: настоящий шаг иконок — десятки px. Выбросы вида 249
    // (разреженные выборки) откатываем на сообщение, затем в fallback.
    auto sanePitch = [](int v) { return v >= 40 && v <= 180; };
    if (!sanePitch(stepX)) stepX = sanePitch(msgStepX) ? msgStepX : 0;
    if (!sanePitch(stepY)) stepY = sanePitch(msgStepY) ? msgStepY : 0;
    if (stepX < 16 || stepY < 16) { LogGrid("nostep", stepX, stepY, msgStepX, msgStepY, np, count); return g; }

    // Anchor: most frequent remainder.
    struct Vote { int mx, my, cnt; };
    Vote votes[32] = {};
    int nvotes = 0;
    for (int i = 0; i < np; i++)
    {
        int mx = ((pts[i].x % stepX) + stepX) % stepX;
        int my = ((pts[i].y % stepY) + stepY) % stepY;
        bool found = false;
        for (int k = 0; k < nvotes; k++)
        {
            if (votes[k].mx == mx && votes[k].my == my)
            { votes[k].cnt++; found = true; break; }
            }
        if (!found && nvotes < 32)
        { votes[nvotes].mx = mx; votes[nvotes].my = my; votes[nvotes].cnt = 1; nvotes++; }
    }
    int best = 0;
    for (int k = 1; k < nvotes; k++)
        if (votes[k].cnt > votes[best].cnt) best = k;
    if (votes[best].cnt < 2 && np > 2) { LogGrid("novote1", votes[best].cnt, np, stepX, stepY, msgStepX, msgStepY); return g; }
    if (votes[best].cnt * 2 < np) { LogGrid("novote2", votes[best].cnt, np, stepX, stepY, msgStepX, msgStepY); return g; }

    RECT listRc = {};
    GetWindowRect(hList, &listRc);

    g.valid = true;
    g.stepX = stepX;
    g.stepY = stepY;
    g.originX = listRc.left + votes[best].mx;
    g.originY = listRc.top + votes[best].my;
    {
        std::lock_guard<std::recursive_mutex> lk(s_gridCacheMutex);
        cached = g;
    }
    LogGrid("ok", stepX, stepY, g.originX, g.originY, msgStepX, msgStepY);
    return g;
}

// Screen rects of desktop icons: one cell-sized rect per icon.
// Shares the same ListView IPC technique as QueryDesktopGrid, but reads
// ALL icons (not just the first 32) so widgets can avoid every file.
// Cached for a couple of seconds — icons rarely move, widgets ask often
// (every drop/startup), and each query is dozens of cross-process calls.
// Кэш иконок в file-scope, чтобы его можно было сбрасывать снаружи
// (InvalidateDesktopIconCache после программного сдвига иконок).
static ULONGLONG g_iconCacheT = 0;
static std::vector<RECT> g_iconCache;
static bool g_iconCacheValid = false;

void InvalidateDesktopIconCache()
{
    std::lock_guard<std::recursive_mutex> lk(s_gridCacheMutex);
    g_iconCacheValid = false;
}

std::vector<RECT> QueryDesktopIconRects(bool* validOut)
{
    // Один гард на всю функцию: покрывает чтение, failure-записи и store
    // (кэш статический, вызовы только читают/пишут его здесь и в Invalidate).
    std::lock_guard<std::recursive_mutex> lk(s_gridCacheMutex);
    auto setValid = [&](bool v) { if (validOut) *validOut = v; };
    ULONGLONG now = GetTickCount64();
    if (g_iconCacheValid && now - g_iconCacheT < 3000)
    {
        setValid(true);
        return g_iconCache;
    }
    g_iconCacheT = now;

    std::vector<RECT> out;

    HWND hList = FindDesktopListView();
    if (!hList || !IsWindow(hList))
    {
        // Не кэшируем провал: вдруг проводник ещё стартует.
        g_iconCacheT = now;
        g_iconCacheValid = false;
        setValid(false);
        return out;
    }

    LRESULT sp = GridSend(hList, LVM_GETITEMSPACING, 0, 0);
    int msgStepX = (int)(short)LOWORD(sp);
    int msgStepY = (int)(short)HIWORD(sp);

    DWORD pid = 0;
    GetWindowThreadProcessId(hList, &pid);
    if (!pid) { g_iconCacheT = now; g_iconCacheValid = false; setValid(false); return out; }
    HANDLE hProc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!hProc) { g_iconCacheT = now; g_iconCacheValid = false; setValid(false); return out; }

    LPVOID remote = VirtualAllocEx(hProc, nullptr, sizeof(POINT), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { CloseHandle(hProc); g_iconCacheT = now; g_iconCacheValid = false; setValid(false); return out; }

    // Провал опроса числа иконок — это НЕ «пустой стол»: помечаем данные
    // невалидными, иначе нажатие на файл при подвисшем Explorer считается
    // кликом по пустому месту и драг файлов рисует рамку.
    LRESULT countRes = 0;
    if (!GridSendEx(hList, LVM_GETITEMCOUNT, 0, 0, &countRes))
    {
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        g_iconCacheT = now;
        g_iconCacheValid = false;
        setValid(false);
        return out;
    }
    int count = (int)countRes;
    if (count < 0) count = 0;
    if (count > 1024) count = 1024; // паранойя: рабочего стола на тысячи иконок не бывает

    RECT listRc = {};
    GetWindowRect(hList, &listRc);

    // Шаг для размера ячейки: сначала пробуем честный грид
    // (он уже закэширован внутри), иначе сообщение, иначе метрики.
    int stepX = 0, stepY = 0;
    {
        DesktopGrid g = QueryDesktopGrid();
        if (g.valid) { stepX = g.stepX; stepY = g.stepY; }
    }
    auto sanePitch = [](int v) { return v >= 40 && v <= 180; };
    if (!sanePitch(stepX)) stepX = sanePitch(msgStepX) ? msgStepX : 0;
    if (!sanePitch(stepY)) stepY = sanePitch(msgStepY) ? msgStepY : 0;
    if (stepX < 16) stepX = GetSystemMetrics(SM_CXICONSPACING);
    if (stepY < 16) stepY = GetSystemMetrics(SM_CYICONSPACING);
    if (stepX < 48) stepX = 80;
    if (stepY < 48) stepY = 90;

    out.reserve(count > 0 ? count : 0);
    // Частичный провал опроса (Explorer подвис посреди перечисления) = данные
    // неполные: не кэшируем и помечаем невалидными, иначе пропущенная иконка
    // под курсором даст ложное «пустое место».
    bool itemsOk = true;
    for (int i = 0; i < count; i++)
    {
        POINT zero = {};
        SIZE_T wr = 0;
        if (!WriteProcessMemory(hProc, remote, &zero, sizeof(zero), &wr) || wr != sizeof(zero))
        { itemsOk = false; continue; }
        if (!GridSend(hList, LVM_GETITEMPOSITION, (WPARAM)i, (LPARAM)remote))
        { itemsOk = false; continue; }
        POINT rp = {};
        SIZE_T rd = 0;
        if (!ReadProcessMemory(hProc, remote, &rp, sizeof(rp), &rd) || rd != sizeof(rp))
        { itemsOk = false; continue; }
        int sx = listRc.left + rp.x;
        int sy = listRc.top + rp.y;
        RECT r = { sx, sy, sx + stepX, sy + stepY };
        out.push_back(r);
    }
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (!itemsOk)
    {
        // Провал не кэшируем: в кэше останется прошлый хороший слепок (если был).
        g_iconCacheT = now;
        setValid(false);
        return out;
    }
    g_iconCache = out;
    g_iconCacheValid = true;
    setValid(true);
    return out;
}

// ---------------------------------------------------------------------------
// Позиционирование иконок через shell COM (IFolderView), а не raw LVM.
// Raw LVM_SETITEMPOSITION проводник молча затирает из своего хранилища
// позиций (проверено: SET проходит, перечитывание показывает старое).
// COM-путь — тот же, которым двигает сам проводник: стор обновляется,
// позиция приживается. Все вызовы — best effort с verify.
// ---------------------------------------------------------------------------

struct ComIcon {
    PIDLIST_ABSOLUTE abs = nullptr; // владеем, CoTaskMemFree
    POINT client = {};              // как вернул GetItemPosition
};

struct ComDesktopView {
    IFolderView* view = nullptr;
    PIDLIST_ABSOLUTE deskAbs = nullptr;
    HWND hList = nullptr;
    RECT listRc = {};
    RECT clientRc = {};
};

static void CloseComDesktopView(ComDesktopView& d)
{
    if (d.view) { d.view->Release(); d.view = nullptr; }
    if (d.deskAbs) { CoTaskMemFree(d.deskAbs); d.deskAbs = nullptr; }
    d.hList = nullptr;
}

static void FreeComIcons(std::vector<ComIcon>& v)
{
    for (auto& e : v)
        if (e.abs) CoTaskMemFree(e.abs);
    v.clear();
}

// Полная цепочка до ЖИВОГО FolderView рабочего стола.
//
// Путь 1 (точный): от HWND нашего SHELLDLL_DefView через
// AccessibleObjectFromWindow(OBJID_NATIVEOM) — отдаёт automation-объект
// именно этого вида. Путь 2 (запасной): перебор окон shell с проверкой
// HWND вида (IShellView::GetWindow) против нашего DefView.
static bool ViewFromDispatch(IDispatch* pdisp, IFolderView** outView);
static bool FinishComDesktopView(ComDesktopView& d, int stage);
static bool OpenComDesktopView(ComDesktopView& d)
{
    CloseComDesktopView(d);
    d.hList = FindDesktopListView();
    if (!d.hList || !IsWindow(d.hList)) return false;
    GetWindowRect(d.hList, &d.listRc);
    GetClientRect(d.hList, &d.clientRc);
    HWND hDef = GetParent(d.hList);

    // Путь 1: напрямую от окон — сначала DefView, потом сам ListView.
    const DWORD kNativeOM = 0xFFFFFFF0; // OBJID_NATIVEOM
    HWND hTry[2] = { hDef, d.hList };
    for (int t = 0; t < 2; t++)
    {
        if (!hTry[t] || !IsWindow(hTry[t])) continue;
        IDispatch* pdisp = nullptr;
        HRESULT hrA = AccessibleObjectFromWindow(hTry[t], kNativeOM,
            IID_IDispatch, (void**)&pdisp);
        LogGrid("comom", t, (int)hrA, pdisp ? 1 : 0, 0, 0, 0);
        if (SUCCEEDED(hrA) && pdisp)
        {
            if (ViewFromDispatch(pdisp, &d.view) && d.view)
            {
                pdisp->Release();
                return FinishComDesktopView(d, 10 + t);
            }
            pdisp->Release();
        }
    }

    IShellWindows* psw = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr,
        CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&psw));
    if (FAILED(hr) || !psw) { LogGrid("comview", (int)hr, 0, 0, 0, 0, 0); return false; }

    long wcount = 0;
    if (FAILED(psw->get_Count(&wcount))) { psw->Release(); return false; }
    bool found = false;
    for (long i = 0; i < wcount && !found; i++)
    {
        VARIANT vIdx;
        VariantInit(&vIdx);
        vIdx.vt = VT_I4;
        vIdx.lVal = i;
        IDispatch* pdisp = nullptr;
        if (FAILED(psw->Item(vIdx, &pdisp)) || !pdisp) continue;
        // Путь 2: проверяем, что HWND этого вида — наш DefView.
        {
            IServiceProvider* psp = nullptr;
            if (SUCCEEDED(pdisp->QueryInterface(IID_IServiceProvider, (void**)&psp)) && psp)
            {
                IShellBrowser* psb = nullptr;
                if (SUCCEEDED(psp->QueryService(SID_STopLevelBrowser,
                        IID_IShellBrowser, (void**)&psb)) && psb)
                {
                    IShellView* psv = nullptr;
                    if (SUCCEEDED(psb->QueryActiveShellView(&psv)) && psv)
                    {
                        HWND hwndV = nullptr;
                        HRESULT hrW = psv->GetWindow(&hwndV); // IOleWindow
                        LogGrid("comcand", i, (int)(LONG_PTR)hwndV,
                            SUCCEEDED(hrW) ? 1 : 0,
                            hwndV == hDef ? 1 : 0, 0, 0);
                        if (SUCCEEDED(hrW) && hwndV && hwndV == hDef)
                        {
                            if (SUCCEEDED(psv->QueryInterface(IID_PPV_ARGS(&d.view))) && d.view)
                                found = true;
                        }
                        psv->Release();
                    }
                    psb->Release();
                }
                psp->Release();
            }
        }
        pdisp->Release();
        if (found) break;
    }
    psw->Release();
    if (!found || !d.view)
    {
        LogGrid("comview", 0, 7, wcount, 0, 0, 0);
        CloseComDesktopView(d);
        return false;
    }
    return FinishComDesktopView(d, 20);
}

static std::vector<ComIcon> EnumComIcons(ComDesktopView& d);
static void FreeComIcons(std::vector<ComIcon>& v);

// Общая часть цепочки: IDispatch вида -> IFolderView.
static bool ViewFromDispatch(IDispatch* pdisp, IFolderView** outView)
{
    if (!pdisp || !outView) return false;
    *outView = nullptr;
    IServiceProvider* psp = nullptr;
    HRESULT hr = pdisp->QueryInterface(IID_IServiceProvider, (void**)&psp);
    if (FAILED(hr) || !psp) return false;
    IShellBrowser* psb = nullptr;
    hr = psp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, (void**)&psb);
    psp->Release();
    if (FAILED(hr) || !psb) return false;
    IShellView* psv = nullptr;
    hr = psb->QueryActiveShellView(&psv);
    psb->Release();
    if (FAILED(hr) || !psv) return false;
    hr = psv->QueryInterface(IID_PPV_ARGS(outView));
    psv->Release();
    return SUCCEEDED(hr) && *outView;
}

static bool FinishComDesktopView(ComDesktopView& d, int stage)
{
    HRESULT hr = SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &d.deskAbs);
    if (FAILED(hr) || !d.deskAbs)
    {
        LogGrid("comview", (int)hr, 6, 0, 0, 0, 0);
        CloseComDesktopView(d);
        return false;
    }
    // Калибровка: первая позиция COM vs raw LVM должны совпасть.
    {
        std::vector<ComIcon> probe = EnumComIcons(d);
        if (!probe.empty())
            LogGrid("compos", probe[0].client.x, probe[0].client.y,
                (int)probe.size(), 0, 0, 0);
        FreeComIcons(probe);
    }
    LogGrid("comview", 1, stage, 0, 0, 0, 0);
    return true;
}

// Все иконки стола с позициями (абсолютные PIDL + клиентские координаты).
static std::vector<ComIcon> EnumComIcons(ComDesktopView& d)
{
    std::vector<ComIcon> out;
    if (!d.view || !d.deskAbs) return out;
    IEnumIDList* penum = nullptr;
    if (FAILED(d.view->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&penum))) || !penum)
        return out;
    PITEMID_CHILD child = nullptr;
    while (out.size() < 512 && penum->Next(1, &child, nullptr) == S_OK && child)
    {
        POINT pt = {};
        HRESULT hrPos = d.view->GetItemPosition(child, &pt);
        PIDLIST_ABSOLUTE abs = ILCombine(d.deskAbs, child);
        CoTaskMemFree(child);
        child = nullptr;
        if (!abs) continue;
        if (FAILED(hrPos)) { CoTaskMemFree(abs); continue; }
        out.push_back({ abs, pt });
    }
    if (child) CoTaskMemFree(child);
    penum->Release();
    return out;
}

static bool MoveComIcon(ComDesktopView& d, const ComIcon& ic, POINT to)
{
    if (!d.view || !ic.abs) return false;
    PCIDLIST_ABSOLUTE arr[1] = { ic.abs };
    HRESULT hr = d.view->SelectAndPositionItems(1, arr, &to, 0);
    LogGrid("commove", to.x, to.y, (int)hr, 0, 0, 0);
    return SUCCEEDED(hr);
}

// Сдвинуть + проверить перечитыванием (защита от рассинхрона и от
// переупаковки shell: при mismatch дальше не двигаем, чтобы не разносить).
static bool MoveComIconVerified(ComDesktopView& d, const ComIcon& ic, POINT to)
{
    if (!MoveComIcon(d, ic, to)) return false;
    Sleep(200);
    std::vector<ComIcon> cur = EnumComIcons(d);
    bool ok = false;
    for (const auto& e : cur)
    {
        if (ILIsEqual(ic.abs, e.abs))
        {
            ok = (abs(e.client.x - to.x) <= 2 && abs(e.client.y - to.y) <= 2);
            LogGrid("comverify", e.client.x, e.client.y, to.x, to.y, ok ? 1 : 0, 0);
            break;
        }
    }
    FreeComIcons(cur);
    return ok;
}

// Сдвиг иконок из-под виджетов вниз по сетке (подход Fences).
// Нужен, когда виджет уже стоит у края рабочей области и выше ехать
// некуда: нахлёст в десяток px сдвигом виджета не лечится в принципе
// (следующая ячейка — в 76..90px, отсюда были «большие дыры»).
// Двигаем только конфликтующие иконки, минимально — вниз в своей колонке.
int PushDesktopIconsOutOfWidgets(const std::vector<RECT>& widgetRects)
{
    if (widgetRects.empty()) return 0;

    HWND hList = FindDesktopListView();
    if (!hList || !IsWindow(hList)) return 0;

    // Диагностика режима проводника: auto-arrange решает, устоит ли сдвиг.
    {
        LONG_PTR style = GetWindowLongPtrW(hList, GWL_STYLE);
        int countProbe = (int)GridSend(hList, LVM_GETITEMCOUNT, 0, 0);
        LogGrid("style", (int)(style & 0xFFFFFFFF),
            (style & LVS_AUTOARRANGE) ? 1 : 0, countProbe, 0, 0, 0);
    }

    DesktopGrid g = QueryDesktopGrid();
    if (!g.valid) { LogGrid("push-nogrid", 0, 0, 0, 0, 0, 0); return 0; }

    RECT listRc = {};
    GetWindowRect(hList, &listRc);
    RECT clientRc = {};
    GetClientRect(hList, &clientRc);
    LogGrid("push-rc", listRc.left, listRc.top, listRc.right, listRc.bottom,
        clientRc.right, clientRc.bottom);

    // Заблокированные прямоугольники: виджеты + 2px зазора.
    std::vector<RECT> blocked = widgetRects;
    for (RECT& r : blocked) { r.left -= 2; r.top -= 2; r.right += 2; r.bottom += 2; }
    auto overlapsBlocked = [&](const RECT& r) {
        for (const RECT& b : blocked)
            if (r.left < b.right && r.right > b.left &&
                r.top < b.bottom && r.bottom > b.top)
                return true;
        return false;
    };

    ComDesktopView com;
    if (!OpenComDesktopView(com)) return 0;
    // Свежие rect окна из COM-открытия (то же окно ListView).
    listRc = com.listRc;
    clientRc = com.clientRc;

    std::vector<ComIcon> comIcons = EnumComIcons(com);
    int count = (int)comIcons.size();

    struct IconPos { size_t ref; RECT screen; };
    std::vector<IconPos> icons;
    icons.reserve(comIcons.size());
    for (size_t i = 0; i < comIcons.size(); i++)
    {
        RECT sr = { listRc.left + comIcons[i].client.x, listRc.top + comIcons[i].client.y,
                    listRc.left + comIcons[i].client.x + g.stepX,
                    listRc.top + comIcons[i].client.y + g.stepY };
        icons.push_back({ i, sr });
    }
    if (!icons.empty())
        LogGrid("compos", comIcons[0].client.x, comIcons[0].client.y,
            count, g.stepX, g.stepY, 0);

    struct Cell { int x, y; };
    auto cellOf = [&](int sx, int sy) -> Cell {
        int dx = sx - g.originX, dy = sy - g.originY;
        int qx = (dx >= 0) ? (dx + g.stepX / 2) / g.stepX : (dx - g.stepX / 2) / g.stepX;
        int qy = (dy >= 0) ? (dy + g.stepY / 2) / g.stepY : (dy - g.stepY / 2) / g.stepY;
        return { qx, qy };
    };
    int cliOx = g.originX - listRc.left; // начало сетки в клиентских координатах
    int cliOy = g.originY - listRc.top;
    auto cellRectClient = [&](Cell c) -> RECT {
        return { cliOx + c.x * g.stepX, cliOy + c.y * g.stepY,
                 cliOx + c.x * g.stepX + g.stepX, cliOy + c.y * g.stepY + g.stepY };
    };
    auto cellRectScreen = [&](Cell c) -> RECT {
        return { g.originX + c.x * g.stepX, g.originY + c.y * g.stepY,
                 g.originX + c.x * g.stepX + g.stepX, g.originY + c.y * g.stepY + g.stepY };
    };

    // Занятые ячейки: все иконки + покрытие виджетов.
    std::vector<Cell> occupied;
    for (const auto& ic : icons)
        occupied.push_back(cellOf(ic.screen.left, ic.screen.top));
    for (const RECT& b : widgetRects)
    {
        Cell c0 = cellOf(b.left, b.top);
        Cell c1 = cellOf(b.right - 1, b.bottom - 1);
        int x0 = c0.x < c1.x ? c0.x : c1.x, x1 = c0.x > c1.x ? c0.x : c1.x;
        int y0 = c0.y < c1.y ? c0.y : c1.y, y1 = c0.y > c1.y ? c0.y : c1.y;
        for (int cx = x0; cx <= x1; cx++)
            for (int cy = y0; cy <= y1; cy++)
                occupied.push_back({ cx, cy });
    }
    auto isOccupied = [&](Cell c) {
        for (const Cell& o : occupied)
            if (o.x == c.x && o.y == c.y) return true;
        return false;
    };
    auto fitsClient = [&](const RECT& cr) {
        return cr.left >= 0 && cr.top >= 0 &&
               cr.right <= clientRc.right && cr.bottom <= clientRc.bottom;
    };

    // Конфликтующие иконки — сверху вниз, слева направо (детерминированно).
    std::vector<size_t> conflict;
    for (size_t k = 0; k < icons.size(); k++)
        if (overlapsBlocked(icons[k].screen))
            conflict.push_back(k);
    std::sort(conflict.begin(), conflict.end(), [&](size_t a, size_t b) {
        if (icons[a].screen.top != icons[b].screen.top)
            return icons[a].screen.top < icons[b].screen.top;
        return icons[a].screen.left < icons[b].screen.left;
    });

    int moved = 0;
    for (size_t k : conflict)
    {
        LogGrid("confl", (int)k, icons[k].screen.left,
            icons[k].screen.top, 0, 0, 0);
        Cell cur = cellOf(icons[k].screen.left, icons[k].screen.top);
        // Свою ячейку освобождаем на время поиска (мы её покидаем).
        for (auto it = occupied.begin(); it != occupied.end(); ++it)
            if (it->x == cur.x && it->y == cur.y) { occupied.erase(it); break; }

        Cell best = cur;
        bool found = false;
        // Сначала строго вниз в своей колонке (до 12 рядов).
        for (int dy = 1; dy <= 12 && !found; dy++)
        {
            Cell c = { cur.x, cur.y + dy };
            RECT cr = cellRectClient(c);
            if (!fitsClient(cr))
            {
                if (cr.top >= clientRc.bottom) break; // ниже только хуже
                continue;
            }
            if (!isOccupied(c) && !overlapsBlocked(cellRectScreen(c)))
            { best = c; found = true; }
        }
        // Потом соседние колонки рядом (до ±3, вниз до 6).
        for (int ring = 1; ring <= 3 && !found; ring++)
            for (int dx = -ring; dx <= ring && !found; dx++)
            {
                if (dx != ring && dx != -ring) continue;
                for (int dy = 0; dy <= 6 && !found; dy++)
                {
                    Cell c = { cur.x + dx, cur.y + dy };
                    if (!fitsClient(cellRectClient(c))) continue;
                    if (!isOccupied(c) && !overlapsBlocked(cellRectScreen(c)))
                    { best = c; found = true; }
                }
            }
        if (!found) { occupied.push_back(cur); continue; } // некуда — оставляем

        RECT dst = cellRectClient(best);
        POINT np = { dst.left, dst.top };
        // Двигаем через shell (обновляет его хранилище позиций).
        // При mismatch дальше не идём — иначе разнесём стол.
        if (MoveComIconVerified(com, comIcons[icons[k].ref], np))
        {
            LogGrid("moved", (int)k,
                dst.left + listRc.left, dst.top + listRc.top,
                best.x, best.y, 0);
            occupied.push_back(best);
            moved++;
        }
        else
        {
            LogGrid("push-abort", (int)k, 0, 0, 0, 0, 0);
            occupied.push_back(cur);
            break;
        }
    }

    FreeComIcons(comIcons);
    CloseComDesktopView(com);
    if (moved > 0) InvalidateDesktopIconCache();
    LogGrid("push", moved, (int)conflict.size(), count, g.stepX, g.stepY, 0);
    return moved;
}

// Разложить иконки, оказавшиеся в одной точке (стопка — проводник
// схлопывает точечные сдвиги обратно, поэтому цели выбираем рядом
// с виджетами: виджеты точно на видимом месте, в отличие от произвольных
// колонок виртуального экрана с мёртвыми зонами).
// Затем перечитать позиции и залогировать итог (verify). Возвращает число
// переставленных.
int SpreadStackedDesktopIcons(const std::vector<RECT>& widgetRects)
{
    ComDesktopView com;
    if (!OpenComDesktopView(com)) return 0;
    RECT listRc = com.listRc;
    RECT clientRc = com.clientRc;
    DesktopGrid g = QueryDesktopGrid();
    if (!g.valid) { CloseComDesktopView(com); return 0; }

    std::vector<ComIcon> comIcons = EnumComIcons(com);
    // pos: ссылка на comIcons + клиентская точка (обновляем при перестановке).
    struct Pos { size_t ref; POINT pt; };
    std::vector<Pos> pos;
    for (size_t i = 0; i < comIcons.size(); i++)
        pos.push_back({ i, comIcons[i].client });

    struct Cell { int x, y; };
    auto cellOfScreen = [&](int sx, int sy) -> Cell {
        int dx = sx - g.originX, dy = sy - g.originY;
        int qx = (dx >= 0) ? (dx + g.stepX / 2) / g.stepX : (dx - g.stepX / 2) / g.stepX;
        int qy = (dy >= 0) ? (dy + g.stepY / 2) / g.stepY : (dy - g.stepY / 2) / g.stepY;
        return { qx, qy };
    };

    // Занятость по клиентским точкам (обновляем при каждой перестановке).
    std::vector<POINT> occ;
    for (const auto& e : pos) occ.push_back(e.pt);
    auto occEraseOne = [&](POINT p) {
        for (auto it = occ.begin(); it != occ.end(); ++it)
            if (it->x == p.x && it->y == p.y) { occ.erase(it); break; }
    };
    auto occBusy = [&](POINT p) {
        for (const POINT& o : occ)
            if (o.x == p.x && o.y == p.y) return true;
        return false;
    };

    // Заблокированное виджетами (пиксельно, +2px) — цели не должны заезжать.
    std::vector<RECT> blocked = widgetRects;
    for (RECT& r : blocked) { r.left -= 2; r.top -= 2; r.right += 2; r.bottom += 2; }
    auto hitsWidget = [&](int cliX, int cliY) {
        RECT cand = { listRc.left + cliX, listRc.top + cliY,
                      listRc.left + cliX + g.stepX, listRc.top + cliY + g.stepY };
        for (const RECT& b : blocked)
            if (cand.left < b.right && cand.right > b.left &&
                cand.top < b.bottom && cand.bottom > b.top)
                return true;
        return false;
    };

    // Кандидаты: колонки виджетов, ряды ниже их низа (+2..+9). Виджеты
    // на видимом месте по построению — рядом с ними тоже видно.
    std::vector<POINT> cand;
    for (const RECT& b : widgetRects)
    {
        Cell c0 = cellOfScreen(b.left, b.top);
        Cell c1 = cellOfScreen(b.right - 1, b.bottom - 1);
        int x0 = c0.x < c1.x ? c0.x : c1.x, x1 = c0.x > c1.x ? c0.x : c1.x;
        int yBot = c0.y > c1.y ? c0.y : c1.y;
        int cliOx = g.originX - listRc.left, cliOy = g.originY - listRc.top;
        for (int cx = x0; cx <= x1; cx++)
            for (int dy = 2; dy <= 9; dy++)
                cand.push_back({ cliOx + cx * g.stepX, cliOy + (yBot + dy) * g.stepY });
    }

    // 0 = кандидат не подходит, 1 = встали+проверено, 2 = сдвинули, но shell
    // вернул обратно (дальше двигать бессмысленно — globally abort).
    auto placeOne = [&](size_t k, POINT np) -> int {
        if (np.y < 0 || np.y + g.stepY > clientRc.bottom) return 0;
        if (np.x < 0 || np.x + g.stepX > clientRc.right) return 0;
        if (hitsWidget(np.x, np.y)) return 0;
        if (!MoveComIconVerified(com, comIcons[pos[k].ref], np)) return 2;
        LogGrid("spread", (int)k,
            np.x + listRc.left, np.y + listRc.top, 0, 0, 0);
        occEraseOne(pos[k].pt);
        pos[k].pt = np;
        occ.push_back(np);
        return 1;
    };

    int spread = 0;
    bool aborted = false;
    // Группы-стопки: ВСЕХ участников (включая первого) уводим к виджетам,
    // коли текущая точка уже доказала свою невидимость.
    std::vector<char> done(pos.size(), 0);
    for (size_t a = 0; a < pos.size() && !aborted; a++)
    {
        if (done[a]) continue;
        // Собрать группу одинаковых позиций.
        std::vector<size_t> group;
        group.push_back(a);
        for (size_t b = a + 1; b < pos.size(); b++)
        {
            if (done[b]) continue;
            if (pos[b].pt.x == pos[a].pt.x &&
                pos[b].pt.y == pos[a].pt.y)
                group.push_back(b);
        }
        if (group.size() < 2) continue;
        for (size_t k : group) done[k] = 1;
        for (size_t k : group)
        {
            bool placed = false;
            for (const POINT& np : cand)
            {
                if (aborted) break;
                if (occBusy(np)) continue;
                int r = placeOne(k, np);
                if (r == 1) { placed = true; spread++; break; }
                if (r == 2) { aborted = true; break; }
            }
            if (!placed && !aborted)
            {
                // Запасной вариант: вниз в своей колонке.
                for (int r = 1; r <= 14 && !placed && !aborted; r++)
                {
                    POINT np = { pos[k].pt.x, pos[k].pt.y + r * g.stepY };
                    if (occBusy(np)) continue;
                    int rc = placeOne(k, np);
                    if (rc == 1) { placed = true; spread++; }
                    else if (rc == 2) aborted = true;
                }
            }
            if (!placed)
            {
                // Не встали (некуда или shell вернул) — дальше не двигаем,
                // чтобы не разносить стол.
                LogGrid("spread-abort", (int)k, aborted ? 1 : 0, 0, 0, 0, 0);
                if (aborted) break;
            }
        }
    }

    FreeComIcons(comIcons);
    CloseComDesktopView(com);

    // Verify: перечитать и показать итог.
    InvalidateDesktopIconCache();
    Sleep(400);
    std::vector<RECT> after = QueryDesktopIconRects();
    int n = 0;
    for (const RECT& r : after)
    {
        if (n >= 64) break;
        LogGrid("verify", n, r.left, r.top, 0, 0, 0);
        n++;
    }
    LogGrid("spread-done", spread, n, 0, 0, 0, 0);
    return spread;
}

// Диагностика топологии окон рабочего стола: какое окно ListView мы
// используем, а где живут видимые иконки (WorkerW от 0x052C всё путают).
void DumpDesktopWindows()
{
    auto dumpOne = [](const char* tag, HWND h) {
        if (!h || !IsWindow(h)) return;
        RECT rc = {};
        GetWindowRect(h, &rc);
        HWND par = GetParent(h);
        LogGrid(tag, (int)(LONG_PTR)h, (int)(LONG_PTR)par,
            IsWindowVisible(h) ? 1 : 0,
            rc.left, rc.top, rc.right);
        HWND hDef = FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr);
        if (hDef && IsWindow(hDef))
        {
            HWND hList = FindWindowExW(hDef, nullptr, L"SysListView32", nullptr);
            int cnt = -1;
            if (hList && IsWindow(hList))
                cnt = (int)GridSend(hList, LVM_GETITEMCOUNT, 0, 0);
            RECT lr = {};
            if (hList) GetWindowRect(hList, &lr);
            LogGrid("winlist", (int)(LONG_PTR)h, (int)(LONG_PTR)hList, cnt,
                lr.left, lr.top, lr.right);
        }
    };
    HWND hProg = FindWindowW(L"Progman", nullptr);
    dumpOne("winprog", hProg);
    HWND hScan = nullptr;
    int n = 0;
    while ((hScan = FindWindowExW(nullptr, hScan, L"WorkerW", nullptr)) != nullptr && n < 16)
    {
        n++;
        dumpOne("winwork", hScan);
    }
    HWND hCur = FindDesktopListView();
    LogGrid("wincur", (int)(LONG_PTR)hCur, 0, 0, 0, 0, 0);
    // Живые позиции иконок в выбранном окне — сверка после рестарта Explorer.
    {
        std::vector<RECT> all = QueryDesktopIconRects();
        int dumped = 0;
        for (const RECT& r : all)
        {
            if (dumped >= 16) break;
            LogGrid("dumppos", dumped, r.left, r.top, 0, 0, 0);
            dumped++;
        }
        LogGrid("dumppos-done", dumped, 0, 0, 0, 0, 0);
    }
}

// Автоупорядочивание проводника упаковывает иконки от края ВИРТУАЛЬНОГО
// экрана (-800 у пользователя — мёртвая зона отключённого монитора),
// поэтому любой наш сдвиг возвращается в невидимую стопку. Лечится только
// выключением автоупорядочивания (флаг FWF_AUTOARRANGE=0x1 в Bags\1\Desktop,
// то же что галка «Упорядочить значки автоматически»). Спрашиваем согласие,
// т.к. меняем системную настройку. Возвращает true если можно двигать.
static bool EnsureShellAutoArrangeOff()
{
    HKEY hKey = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Desktop",
        0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey);
    if (rc != ERROR_SUCCESS || !hKey)
    {
        LogGrid("fflags", rc, -1, 0, 0, 0, 0);
        return true; // ключа нет — переупаковывать нечему, двигаем
    }
    DWORD flags = 0;
    DWORD cb = sizeof(flags);
    DWORD type = 0;
    rc = RegQueryValueExW(hKey, L"FFlags", nullptr, &type, (BYTE*)&flags, &cb);
    if (rc != ERROR_SUCCESS || type != REG_DWORD)
    {
        LogGrid("fflags", rc, -2, 0, 0, 0, 0);
        RegCloseKey(hKey);
        return true;
    }
    const DWORD kAutoArrange = 0x1;
    if (!(flags & kAutoArrange))
    {
        LogGrid("fflags", 0, (int)flags, 0, 0, 0, 0);
        RegCloseKey(hKey);
        return true; // уже выключено
    }
    LogGrid("fflags", 1, (int)flags, 0, 0, 0, 0);
    int answer = MessageBoxW(nullptr,
        L"Проводник включён в режим «Упорядочить значки автоматически» и\n"
        L"прячет иконки в невидимую зону экрана (край отключённого монитора).\n\n"
        L"Отключить автоупорядочивание, чтобы вернуть папки и Корзину\n"
        L"на видимое место? (Выравнивание по сетке останется включено.\n"
        L"Вернуть можно в любой момент: вид рабочего стола → галка обратно.)",
        L"DesktopGroupManager — возврат иконок",
        MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
    if (answer != IDYES)
    {
        LogGrid("fflags", 2, (int)flags, 0, 0, 0, 0);
        RegCloseKey(hKey);
        return false;
    }
    DWORD fresh = flags & ~kAutoArrange;
    rc = RegSetValueExW(hKey, L"FFlags", 0, REG_DWORD,
        (const BYTE*)&fresh, sizeof(fresh));
    RegCloseKey(hKey);
    LogGrid("fflags", 3, (int)fresh, rc, 0, 0, 0);
    if (rc != ERROR_SUCCESS) return false;
    // Пнуть shell перечитать настройки вида.
    PIDLIST_ABSOLUTE pidlDesk = nullptr;
    if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &pidlDesk)) && pidlDesk)
    {
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST, pidlDesk, nullptr);
        CoTaskMemFree(pidlDesk);
    }
    Sleep(800);
    return true;
}

// Аварийный возврат иконок: разложить стопку + вытолкнуть из-под виджетов.
void SpreadAndPushDesktopIcons(const std::vector<RECT>& widgetRects)
{
    if (!EnsureShellAutoArrangeOff())
    {
        LogGrid("rescue", -1, 0, 0, 0, 0, 0);
        return;
    }
    int spread = SpreadStackedDesktopIcons(widgetRects);
    int moved = PushDesktopIconsOutOfWidgets(widgetRects);
    LogGrid("rescue", spread, moved, 0, 0, 0, 0);
}

RECT MonitorWorkRect(POINT pt)
{
    RECT work = {};
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (hMon)
    {
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) work = mi.rcWork;
    }
    if (work.bottom <= work.top)
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

// ---------------------------------------------------------------------------
// Поклеточная сетка по монитору: шаг/фаза голосуются только по иконкам
// того экрана, где точка. На мультимониторных сетапах с разным DPI
// глобальное голосование даёт чужой шаг и виджеты встают криво.
// Сам QueryDesktopGrid не трогаем (проверен), логика ниже — рядом, отдельно.
// ---------------------------------------------------------------------------

// Чтение позиций иконок (клиентские координаты ListView).
static bool CollectIconPoints(std::vector<POINT>& out, RECT& listRc,
    int& msgStepX, int& msgStepY, int maxItems)
{
    out.clear();
    HWND hList = FindDesktopListView();
    if (!hList || !IsWindow(hList)) return false;
    GetWindowRect(hList, &listRc);
    LRESULT sp = GridSend(hList, LVM_GETITEMSPACING, 0, 0);
    msgStepX = (int)(short)LOWORD(sp);
    msgStepY = (int)(short)HIWORD(sp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hList, &pid);
    if (!pid) return false;
    HANDLE hProc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!hProc) return false;
    LPVOID remote = VirtualAllocEx(hProc, nullptr, sizeof(POINT), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { CloseHandle(hProc); return false; }
    int count = (int)GridSend(hList, LVM_GETITEMCOUNT, 0, 0);
    if (count < 0) count = 0;
    if (count > maxItems) count = maxItems;
    for (int i = 0; i < count; i++)
    {
        POINT zero = {};
        SIZE_T wr = 0;
        if (!WriteProcessMemory(hProc, remote, &zero, sizeof(zero), &wr) || wr != sizeof(zero))
            continue;
        if (!GridSend(hList, LVM_GETITEMPOSITION, (WPARAM)i, (LPARAM)remote))
            continue;
        POINT rp = {};
        SIZE_T rd = 0;
        if (!ReadProcessMemory(hProc, remote, &rp, sizeof(rp), &rd) || rd != sizeof(rp))
            continue;
        out.push_back(rp);
    }
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return !out.empty();
}

// Голосование шага/фазы по заданной выборке точек (та же математика,
// что в QueryDesktopGrid, но на векторах и без его кэша/логов).
static DesktopGrid GridFromPointSample(const std::vector<POINT>& pts,
    const RECT& listRc, int msgStepX, int msgStepY)
{
    DesktopGrid g;
    int np = (int)pts.size();
    if (np <= 0) return g;
    std::vector<int> xs(np), ys(np);
    for (int i = 0; i < np; i++) { xs[i] = pts[i].x; ys[i] = pts[i].y; }
    qsort(xs.data(), np, sizeof(int), CmpInt);
    qsort(ys.data(), np, sizeof(int), CmpInt);
    int stepX = VoteDiff(xs.data(), np);
    int stepY = VoteDiff(ys.data(), np);
    if (stepX < 16) stepX = (msgStepX >= 16) ? msgStepX : 0;
    if (stepY < 16) stepY = (msgStepY >= 16) ? msgStepY : 0;
    auto sanePitch = [](int v) { return v >= 40 && v <= 180; };
    if (!sanePitch(stepX)) stepX = sanePitch(msgStepX) ? msgStepX : 0;
    if (!sanePitch(stepY)) stepY = sanePitch(msgStepY) ? msgStepY : 0;
    if (stepX < 16 || stepY < 16) return g;

    struct Vote { int mx, my, cnt; };
    Vote votes[128] = {};
    int nvotes = 0;
    for (int i = 0; i < np; i++)
    {
        int mx = ((pts[i].x % stepX) + stepX) % stepX;
        int my = ((pts[i].y % stepY) + stepY) % stepY;
        bool found = false;
        for (int k = 0; k < nvotes; k++)
        {
            if (votes[k].mx == mx && votes[k].my == my)
            { votes[k].cnt++; found = true; break; }
        }
        if (!found && nvotes < 128)
        { votes[nvotes].mx = mx; votes[nvotes].my = my; votes[nvotes].cnt = 1; nvotes++; }
    }
    int best = 0;
    for (int k = 1; k < nvotes; k++)
        if (votes[k].cnt > votes[best].cnt) best = k;
    if (votes[best].cnt < 2 && np > 2) return g;
    if (votes[best].cnt * 2 < np) return g;

    g.valid = true;
    g.stepX = stepX;
    g.stepY = stepY;
    g.originX = listRc.left + votes[best].mx;
    g.originY = listRc.top + votes[best].my;
    return g;
}

DesktopGrid QueryDesktopGridForPoint(POINT pt)
{
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    RECT monRc = {};
    if (hMon)
    {
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) monRc = mi.rcMonitor;
    }

    static ULONGLONG lastT = 0;
    static RECT lastMon = {};
    static DesktopGrid lastG;
    {
        std::lock_guard<std::recursive_mutex> lk(s_gridCacheMutex);
        ULONGLONG now = GetTickCount64();
        if (lastG.valid && now - lastT < 3000 &&
            lastMon.left == monRc.left && lastMon.top == monRc.top &&
            lastMon.right == monRc.right && lastMon.bottom == monRc.bottom)
            return lastG;
        lastT = now;
    }

    DesktopGrid g;
    if (monRc.right > monRc.left)
    {
        std::vector<POINT> pts;
        RECT listRc = {};
        int msgStepX = 0, msgStepY = 0;
        if (CollectIconPoints(pts, listRc, msgStepX, msgStepY, 256))
        {
            std::vector<POINT> local;
            local.reserve(pts.size());
            for (const POINT& p : pts)
            {
                int sx = listRc.left + p.x;
                int sy = listRc.top + p.y;
                if (sx >= monRc.left - 40 && sx < monRc.right + 40 &&
                    sy >= monRc.top - 40 && sy < monRc.bottom + 40)
                    local.push_back(p);
            }
            if (local.size() >= 3)
            {
                g = GridFromPointSample(local, listRc, msgStepX, msgStepY);
                if (g.valid)
                    LogGrid("okm", g.stepX, g.stepY, g.originX, g.originY,
                        (int)local.size(), (int)pts.size());
            }
        }
    }
    if (!g.valid)
        g = QueryDesktopGrid(); // запасной: глобальный (у него свой кэш)
    {
        std::lock_guard<std::recursive_mutex> lk(s_gridCacheMutex);
        lastMon = monRc;
        lastG = g;
    }
    return g;
}
