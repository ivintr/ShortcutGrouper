// render_demo (dev tool): renders app UI with the real WidgetRenderer headless
// and assembles docs demo GIF (~30s): widget glow, popup slide/fade, row
// hover walk, popup scroll, overflow group, glass colors, hideName, 3x3 grid,
// selected state, command-bar menu, tray menu walk. Not part of the app.
#include <Windows.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <objbase.h>
#include <cstdio>
#include <string>
#include <vector>
#include "Renderer.h"
#include "WidgetTypes.h"
#include "WidgetTypes.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")

// Stub settings for the headless demo (defaults ON).
// Out-of-line definition forces symbol emission for Renderer.obj.
class Settings {
public:
    static bool IsShowOverflow();
};
bool Settings::IsShowOverflow() { return true; }

static int GetEncoderClsid(const wchar_t* mime, CLSID* out)
{
    UINT n = 0, sz = 0;
    if (Gdiplus::GetImageEncodersSize(&n, &sz) != Gdiplus::Ok || n == 0)
        return -1;
    std::vector<BYTE> buf(sz);
    if (Gdiplus::GetImageEncoders(n, sz, (Gdiplus::ImageCodecInfo*)buf.data()) != Gdiplus::Ok)
        return -1;
    auto* info = (Gdiplus::ImageCodecInfo*)buf.data();
    for (UINT i = 0; i < n; i++)
    {
        if (wcscmp(info[i].MimeType, mime) == 0)
        {
            *out = info[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

static bool BitmapSize(HBITMAP hbm, int& w, int& h)
{
    BITMAP bm = {};
    if (!hbm || GetObjectW(hbm, sizeof(bm), &bm) != sizeof(bm)) return false;
    if (!bm.bmBits || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
    w = bm.bmWidth;
    h = abs(bm.bmHeight);
    return true;
}

static void AlphaBlit(HDC dst, int dx, int dy, int w, int h,
    HBITMAP src, BYTE alpha)
{
    HDC mem = CreateCompatibleDC(dst);
    HGDIOBJ old = SelectObject(mem, src);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    GdiAlphaBlend(dst, dx, dy, w, h, mem, 0, 0, w, h, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
}

// NETSCAPE2.0 Application Extension (infinite loop). GDI+ не пишет его сам —
// без блока гифка играет один раз и встаёт. Вставляем после заголовка и
// глобальной палитры (если есть).
static bool AddGifLoop(const wchar_t* path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 13)
    {
        fclose(f);
        return false;
    }
    std::vector<BYTE> data((size_t)sz);
    size_t got = fread(data.data(), 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz || memcmp(data.data(), "GIF89a", 6) != 0)
        return false;
    size_t off = 13; // заголовок (6) + дескриптор экрана (7)
    // Байт 10 — packed-поле LSD: бит 7 = есть глобальная палитра,
    // биты 0-2 — её размер (3 * 2^(N+1) байт). Блок вставляем ПОСЛЕ неё.
    if (data[10] & 0x80)
        off += 3u * (2u << (data[10] & 0x07));
    if (off > data.size())
        return false;
    // Не дублируем.
    static const BYTE ext[] = {
        0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0',
        0x03, 0x01, 0x00, 0x00, 0x00
    };
    for (size_t i = 0; i + sizeof(ext) <= data.size(); i++)
    {
        if (memcmp(&data[i], ext, sizeof(ext)) == 0)
            return true;
    }
    data.insert(data.begin() + (ptrdiff_t)off, ext, ext + sizeof(ext));
    FILE* w = nullptr;
    if (_wfopen_s(&w, path, L"wb") != 0 || !w) return false;
    size_t wrote = fwrite(data.data(), 1, data.size(), w);
    fclose(w);
    return wrote == data.size();
}

// One animation keyframe: widget + optional overlay (popup or menu)
// + optional drag ghost + optional marquee lasso rect.
struct Key {
    HBITMAP widget = nullptr;
    int widgetDx = 0;
    int widgetDy = 0;
    HBITMAP overlay = nullptr; // popup or menu bitmap
    bool overlayIsMenu = false;
    int overlayDx = 0;
    BYTE overlayAlpha = 255;
    bool ghost = false;
    int ghostX = 0, ghostY = 0;
    bool marquee = false;      // rubber-band lasso around the widget
    int marqueePad = 0;        // extra padding beyond widget bounds
    int delayCs = 6;
};

// 48px file icon for the drag ghost. mspaint.exe нет на части систем
// (Store-заглушка) — пробуем несколько путей, в крайнем случае системная.
// Возвращает иконку и флаг владения (shared нельзя DestroyIcon).
static HICON GhostIcon(const wchar_t* path, bool& owned)
{
    owned = false;
    const wchar_t* cands[3] = {
        path,
        L"C:\\Windows\\System32\\notepad.exe",
        L"C:\\Windows\\explorer.exe",
    };
    for (auto c : cands)
    {
        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(c, 0, &sfi, sizeof(sfi),
                SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon)
        {
            owned = true;
            return sfi.hIcon;
        }
    }
    return LoadIconW(nullptr, IDI_APPLICATION); // shared
}

static GroupData MkGroup(const wchar_t* name, int grid,
    const std::vector<std::pair<const wchar_t*, const wchar_t*>>& apps,
    int glass = 0xFFFFFF, bool hideName = false)
{
    GroupData g;
    g.id = name;
    g.name = name;
    g.gridSize = grid;
    g.showOverflow = true;
    g.glassColor = glass;
    g.hideName = hideName;
    for (auto& a : apps)
    {
        ShortcutInfo si;
        si.name = a.first;
        si.lnkPath = a.second;
        si.targetPath = a.second;
        g.shortcuts.push_back(si);
    }
    return g;
}

static WidgetRenderer::MenuRenderItem MI(const wchar_t* text, bool check = false,
    bool dis = false, bool sub = false, wchar_t glyph = 0)
{
    WidgetRenderer::MenuRenderItem it;
    it.text = text ? text : L"";
    it.checked = check;
    it.disabled = dis;
    it.hasSubmenu = sub;
    it.glyph = glyph;
    return it;
}

static WidgetRenderer::MenuRenderItem MSep()
{
    WidgetRenderer::MenuRenderItem it;
    it.separator = true;
    return it;
}

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* outPath = L"docs/screenshots/demo.gif";
    if (argc > 1) outPath = argv[1];

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartupInput gsi;
    if (Gdiplus::GdiplusStartup(&gdiToken, &gsi, nullptr) != Gdiplus::Ok)
        return 2;

    WidgetRenderer r;
    if (!r.Initialize())
    {
        wprintf(L"renderer init failed\n");
        return 3;
    }
    r.SetDeferIcons(false);

    GroupData gDemo = MkGroup(L"Demo", 2, {
        { L"Notepad", L"C:\\Windows\\System32\\notepad.exe" },
        { L"Paint", L"C:\\Windows\\System32\\mspaint.exe" },
        { L"Console", L"C:\\Windows\\System32\\cmd.exe" },
        { L"Explorer", L"C:\\Windows\\explorer.exe" },
    });
    GroupData gDemo5 = MkGroup(L"Demo", 2, {
        { L"Notepad", L"C:\\Windows\\System32\\notepad.exe" },
        { L"Paint", L"C:\\Windows\\System32\\mspaint.exe" },
        { L"Console", L"C:\\Windows\\System32\\cmd.exe" },
        { L"Explorer", L"C:\\Windows\\explorer.exe" },
        { L"Registry", L"C:\\Windows\\System32\\regedit.exe" },
    });
    GroupData gTall = MkGroup(L"Tools", 2, {
        { L"Notepad", L"C:\\Windows\\System32\\notepad.exe" },
        { L"Paint", L"C:\\Windows\\System32\\mspaint.exe" },
        { L"Console", L"C:\\Windows\\System32\\cmd.exe" },
        { L"Explorer", L"C:\\Windows\\explorer.exe" },
        { L"Registry", L"C:\\Windows\\System32\\regedit.exe" },
        { L"TaskMgr", L"C:\\Windows\\System32\\taskmgr.exe" },
        { L"SysInfo", L"C:\\Windows\\System32\\msinfo32.exe" },
        { L"Charmap", L"C:\\Windows\\System32\\charmap.exe" },
        { L"Snip", L"C:\\Windows\\System32\\SnippingTool.exe" },
        { L"Console2", L"C:\\Windows\\System32\\mmc.exe" },
    });
    GroupData gBlue = MkGroup(L"Demo", 2, {
        { L"Notepad", L"C:\\Windows\\System32\\notepad.exe" },
        { L"Paint", L"C:\\Windows\\System32\\mspaint.exe" },
        { L"Console", L"C:\\Windows\\System32\\cmd.exe" },
        { L"Explorer", L"C:\\Windows\\explorer.exe" },
    }, 0x0078D4);
    GroupData gTeal = MkGroup(L"Demo", 2, {
        { L"Notepad", L"C:\\Windows\\System32\\notepad.exe" },
        { L"Paint", L"C:\\Windows\\System32\\mspaint.exe" },
        { L"Console", L"C:\\Windows\\System32\\cmd.exe" },
        { L"Explorer", L"C:\\Windows\\explorer.exe" },
    }, 0x038387);
    GroupData gPurple = MkGroup(L"Demo", 2, {
        { L"Notepad", L"C:\\Windows\\System32\\notepad.exe" },
        { L"Paint", L"C:\\Windows\\System32\\mspaint.exe" },
        { L"Console", L"C:\\Windows\\System32\\cmd.exe" },
        { L"Explorer", L"C:\\Windows\\explorer.exe" },
    }, 0x744DA9);
    GroupData gNoName = MkGroup(L"Demo", 2, {
        { L"Notepad", L"C:\\Windows\\System32\\notepad.exe" },
        { L"Paint", L"C:\\Windows\\System32\\mspaint.exe" },
        { L"Console", L"C:\\Windows\\System32\\cmd.exe" },
        { L"Explorer", L"C:\\Windows\\explorer.exe" },
    }, 0xFFFFFF, true);
    GroupData gGrid = MkGroup(L"Grid", 3, {
        { L"Notepad", L"C:\\Windows\\System32\\notepad.exe" },
        { L"Paint", L"C:\\Windows\\System32\\mspaint.exe" },
        { L"Console", L"C:\\Windows\\System32\\cmd.exe" },
        { L"Explorer", L"C:\\Windows\\explorer.exe" },
        { L"Registry", L"C:\\Windows\\System32\\regedit.exe" },
        { L"TaskMgr", L"C:\\Windows\\System32\\taskmgr.exe" },
        { L"SysInfo", L"C:\\Windows\\System32\\msinfo32.exe" },
        { L"Charmap", L"C:\\Windows\\System32\\charmap.exe" },
        { L"Snip", L"C:\\Windows\\System32\\SnippingTool.exe" },
    });

    WidgetRenderContext ctx;
    std::vector<HBITMAP> owned;
    auto keep = [&](RenderedBitmap b) -> HBITMAP {
        if (b.hBitmap) owned.push_back(b.hBitmap);
        return b.hBitmap;
    };
    HBITMAP wPlain = keep(r.RenderWidget(gDemo, ctx, 0, false));
    HBITMAP w5plain = keep(r.RenderWidget(gDemo5, ctx, 0, false));
    bool ghostOwned = false;
    HICON hGhost = GhostIcon(L"C:\\Windows\\System32\\mspaint.exe", ghostOwned);
    wprintf(L"hGhost=%p owned=%d\n", hGhost, (int)ghostOwned);
    HBITMAP wG1 = keep(r.RenderWidget(gDemo, ctx, 90, false));
    HBITMAP wG2 = keep(r.RenderWidget(gDemo, ctx, 175, false));
    HBITMAP wGlow = keep(r.RenderWidget(gDemo, ctx, 255, false));
    HBITMAP wSel = keep(r.RenderWidget(gDemo, ctx, 0, true));
    HBITMAP wBlue = keep(r.RenderWidget(gBlue, ctx, 0, false));
    HBITMAP wTeal = keep(r.RenderWidget(gTeal, ctx, 0, false));
    HBITMAP wPurple = keep(r.RenderWidget(gPurple, ctx, 0, false));
    HBITMAP wNoName = keep(r.RenderWidget(gNoName, ctx, 0, false));
    HBITMAP wGrid = keep(r.RenderWidget(gGrid, ctx, 0, false));
    HBITMAP wTall = keep(r.RenderWidget(gTall, ctx, 0, false));
    HBITMAP pb = keep(r.RenderPopup(gDemo, -1));
    std::vector<HBITMAP> pbHov;
    for (int i = 0; i < 4; i++)
        pbHov.push_back(keep(r.RenderPopup(gDemo, i)));
    // Tall popup with limited viewport for scroll demo.
    HBITMAP pbT0 = keep(r.RenderPopup(gTall, -1, 0, 0, 0, 220));
    HBITMAP pbT1 = keep(r.RenderPopup(gTall, -1, 0, 0, 90, 220));
    HBITMAP pbT2 = keep(r.RenderPopup(gTall, -1, 0, 0, 180, 220));
    HBITMAP pbT3 = keep(r.RenderPopup(gTall, 7, 0, 0, 260, 220));
    if (!wPlain || !pb)
    {
        wprintf(L"render failed\n");
        return 4;
    }

    // Widget context menu: checkmark, submenu chevrons, danger item.
    std::vector<WidgetRenderer::MenuRenderItem> ctx2;
    ctx2.push_back(MI(L"Open"));
    ctx2.push_back(MI(L"Rename"));
    ctx2.push_back(MI(L"Hide name", true));
    ctx2.push_back(MI(L"Sorting", false, false, true));
    ctx2.push_back(MI(L"Color", false, false, true));
    ctx2.push_back(MI(L"Grid size", false, false, true));
    ctx2.push_back(MSep());
    ctx2.push_back(MI(L"Ungroup"));
    ctx2.push_back(MI(L"Delete group"));
    HBITMAP mbCtx = keep(r.RenderMenu(ctx2, -1, -1, 0, 0, true));
    HBITMAP mbCtxH = keep(r.RenderMenu(ctx2, 4, -1, 0, 0, true));

    // Command-bar menu with glyphs.
    std::vector<WidgetRenderer::MenuRenderItem> cmds;
    cmds.push_back(MI(L"Cut", false, false, false, (wchar_t)0xE8C6));
    cmds.push_back(MI(L"Copy", false, false, false, (wchar_t)0xE8C8));
    cmds.push_back(MI(L"Delete", false, false, false, (wchar_t)0xE74D));
    std::vector<WidgetRenderer::MenuRenderItem> files;
    files.push_back(MI(L"a.lnk"));
    files.push_back(MI(L"b.lnk"));
    HBITMAP mbCmd = keep(r.RenderMenu(cmds, files, -1, -1, 0, -1, 0, 0, true));
    HBITMAP mbCmdH = keep(r.RenderMenu(cmds, files, 1, -1, 1, -1, 0, 0, true));

    // Tray-style menu.
    std::vector<WidgetRenderer::MenuRenderItem> items;
    {
        WidgetRenderer::MenuRenderItem t;
        t.text = L"Shortcut Grouper"; t.isTitle = true;
        items.push_back(t);
    }
    items.push_back(MSep());
    const wchar_t* names[] = {
        L"Search", L"Hide widgets", L"Refresh", L"Settings", L"Check for updates",
    };
    for (auto n : names) items.push_back(MI(n));
    items.push_back(MSep());
    items.push_back(MI(L"Exit"));
    HBITMAP mb = keep(r.RenderMenu(items, -1, -1, 0, 0, true));
    HBITMAP mbH2 = keep(r.RenderMenu(items, 2, -1, 0, 0, true));
    HBITMAP mbH3 = keep(r.RenderMenu(items, 3, -1, 0, 0, true));
    HBITMAP mbH4 = keep(r.RenderMenu(items, 4, -1, 0, 0, true));
    HBITMAP mbH5 = keep(r.RenderMenu(items, 5, -1, 0, 0, true));
    HBITMAP mbH6 = keep(r.RenderMenu(items, 6, -1, 0, 0, true));
    if (!mb)
    {
        wprintf(L"menu render failed\n");
        return 4;
    }

    int ww = 0, wh = 0, pw = 0, ph = 0, mw = 0, mh = 0;
    BitmapSize(wPlain, ww, wh);
    BitmapSize(pb, pw, ph);
    BitmapSize(mb, mw, mh);
    int tallest = ph;
    {
        int w = 0, h = 0;
        if (BitmapSize(pbT2, w, h) && h > tallest) tallest = h;
        if (BitmapSize(mbCmd, w, h) && h > tallest) tallest = h;
        if (BitmapSize(mbCtx, w, h) && h > tallest) tallest = h;
    }
    int sideW = pw > mw ? pw : mw;
    const int CW = 60 + ww + 16 + sideW + 60;
    int contentH = ph > mh ? ph : mh;
    if (tallest > contentH) contentH = tallest;
    if (wh > contentH) contentH = wh;
    const int CH = contentH + 80;
    const int wx = 50, wy = (CH - wh) / 2;
    const int px = wx + ww + 16, py = (CH - ph) / 2;
    wprintf(L"canvas %dx%d\n", CW, CH);

    std::vector<Key> keys;
    auto K = [&](HBITMAP w, HBITMAP o, bool isMenu, int dx, BYTE a, int d) {
        Key k;
        k.widget = w; k.overlay = o; k.overlayIsMenu = isMenu;
        k.overlayDx = dx; k.overlayAlpha = a; k.delayCs = d;
        keys.push_back(k);
    };
    auto W = [&](HBITMAP w, int d) { K(w, nullptr, false, 0, 0, d); };
    auto WMv = [&](HBITMAP w, int dx, int dy, int d) {
        Key k;
        k.widget = w; k.widgetDx = dx; k.widgetDy = dy; k.delayCs = d;
        keys.push_back(k);
    };
    auto G = [&](HBITMAP w, int gx, int gy, int d) {
        Key k;
        k.widget = w; k.ghost = true; k.ghostX = gx; k.ghostY = gy;
        k.delayCs = d;
        keys.push_back(k);
    };
    auto P = [&](HBITMAP w, HBITMAP p, int dx, BYTE a, int d) {
        K(w, p, false, dx, a, d);
    };
    auto M = [&](HBITMAP m, BYTE a, int d) {
        K(nullptr, m, true, 0, a, d);
    };
    auto MQ = [&](int pad, int d) {
        Key k;
        k.widget = wPlain; k.marquee = true; k.marqueePad = pad; k.delayCs = d;
        keys.push_back(k);
    };
    // 1. Idle + glow ramp.
    W(wPlain, 150);
    W(wG1, 5); W(wG2, 5); W(wGlow, 150);
    // 2. Popup slides + fades in (8 мелких шагов — крупные дают вспышки).
    P(wGlow, pb, -pw * 7 / 8, 60, 3);
    P(wGlow, pb, -pw * 6 / 8, 95, 3);
    P(wGlow, pb, -pw * 5 / 8, 130, 3);
    P(wGlow, pb, -pw * 4 / 8, 165, 3);
    P(wGlow, pb, -pw * 3 / 8, 195, 3);
    P(wGlow, pb, -pw * 2 / 8, 220, 3);
    P(wGlow, pb, -pw / 8, 240, 3);
    P(wPlain, pb, 0, 255, 400);
    // 3. Row hover walks the whole list.
    P(wPlain, pbHov[0], 0, 255, 50);
    P(wPlain, pbHov[1], 0, 255, 50);
    P(wPlain, pbHov[2], 0, 255, 50);
    P(wPlain, pbHov[3], 0, 255, 150);
    // 3b. Drag-and-drop файла на виджет: иконка летит, виджет вспыхивает,
    // ярлык добавляется (+1 к счётчику).
    {
        int gx0 = CW - 110, gy0 = wy - 60;
        int gx1 = wx + 14, gy1 = wy + 14;
        G(wPlain, gx0, gy0, 60);
        for (int s = 1; s <= 5; s++)
        {
            int gx = gx0 + (gx1 - gx0) * s / 5;
            int gy = gy0 + (gy1 - gy0) * s / 5;
            G(s == 5 ? wGlow : wPlain, gx, gy, 5);
        }
        W(w5plain, 200); // группа обновилась: 5 ярлыков, бейдж +1
    }
    // 3c. Перетаскивание самого виджета на новое место и обратно (8 шагов).
    WMv(w5plain, 8, 3, 4);
    WMv(w5plain, 15, 6, 4);
    WMv(w5plain, 23, 9, 4);
    WMv(w5plain, 30, 12, 4);
    WMv(w5plain, 38, 15, 4);
    WMv(w5plain, 45, 18, 4);
    WMv(w5plain, 53, 21, 4);
    WMv(w5plain, 60, 24, 30);
    WMv(w5plain, 45, 18, 4);
    WMv(w5plain, 30, 12, 4);
    WMv(w5plain, 15, 6, 4);
    WMv(w5plain, 0, 0, 30);
    // 4. Tall popup scrolls down and back.
    P(wTall, pbT0, 0, 255, 250);
    P(wTall, pbT1, 0, 255, 40);
    P(wTall, pbT2, 0, 255, 40);
    P(wTall, pbT3, 0, 255, 250);
    P(wTall, pbT1, 0, 255, 40);
    P(wTall, pbT0, 0, 255, 250);
    P(wTall, pbT1, 0, 255, 40);
    P(wTall, pbT2, 0, 255, 40);
    P(wTall, pbT3, 0, 255, 250);
    // 5. Overflow badge group + full popup.
    P(wTall, nullptr, 0, 0, 400);
    // (wTall already shows +6 badge.)
    // 6. Glass colors, hideName, 3x3 grid, selected.
    W(wBlue, 200); W(wTeal, 200); W(wPurple, 250);
    W(wNoName, 150);
    W(wGrid, 200);
    // 6b. Marquee lasso: рамка растёт вокруг виджета, виджет выбран.
    MQ(4, 5);
    MQ(12, 5);
    MQ(20, 5);
    MQ(28, 5);
    W(wSel, 200); // лассо выбрало виджет
    // 7. Popup fades out (5 ступеней), context menu fades in (5 ступеней).
    P(wPlain, pb, 0, 210, 3);
    P(wPlain, pb, 0, 160, 3);
    P(wPlain, pb, 0, 110, 3);
    P(wPlain, pb, 0, 60, 3);
    M(mbCtx, 60, 3);
    M(mbCtx, 110, 3);
    M(mbCtx, 160, 3);
    M(mbCtx, 210, 3);
    M(mbCtx, 255, 200);
    M(mbCtxH, 255, 250);
    M(mbCtx, 210, 3);
    M(mbCtx, 160, 3);
    M(mbCtx, 110, 3);
    M(mbCtx, 60, 3);
    // 8. Command-bar menu with glyphs.
    M(mbCmd, 60, 3);
    M(mbCmd, 110, 3);
    M(mbCmd, 160, 3);
    M(mbCmd, 210, 3);
    M(mbCmd, 255, 200);
    M(mbCmdH, 255, 250);
    M(mbCmd, 210, 3);
    M(mbCmd, 160, 3);
    M(mbCmd, 110, 3);
    M(mbCmd, 60, 3);
    // 9. Tray menu: in (5 ступеней), full hover walk, hold, out (5 ступеней).
    M(mb, 60, 3);
    M(mb, 110, 3);
    M(mb, 160, 3);
    M(mb, 210, 3);
    M(mb, 255, 200);
    M(mbH2, 255, 50);
    M(mbH3, 255, 50);
    M(mbH4, 255, 50);
    M(mbH5, 255, 50);
    M(mbH6, 255, 500);
    M(mb, 210, 3);
    M(mb, 160, 3);
    M(mb, 110, 3);
    M(mb, 60, 3);
    // 10. Back to idle.
    W(wPlain, 2000);

    CLSID gifClsid;
    if (GetEncoderClsid(L"image/gif", &gifClsid) < 0)
    {
        wprintf(L"no gif encoder\n");
        return 5;
    }

    Gdiplus::Bitmap* multi = nullptr;
    Gdiplus::Status st = Gdiplus::Ok;
    bool first = true;
    for (size_t fi = 0; fi < keys.size() && st == Gdiplus::Ok; fi++)
    {
        const Key& k = keys[fi];
        HDC hdcS = GetDC(nullptr);
        HDC hdcM = CreateCompatibleDC(hdcS);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = CW;
        bmi.bmiHeader.biHeight = -CH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP hFrame = CreateDIBSection(hdcM, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ hOld = SelectObject(hdcM, hFrame);
        HBRUSH bg = CreateSolidBrush(RGB(27, 27, 27));
        RECT rc = { 0, 0, CW, CH };
        FillRect(hdcM, &rc, bg);
        DeleteObject(bg);
        if (k.widget)
        {
            int w = 0, h = 0;
            BitmapSize(k.widget, w, h);
            AlphaBlit(hdcM, wx + k.widgetDx, (CH - h) / 2 + k.widgetDy,
                w, h, k.widget, 255);
        }
        if (k.overlay)
        {
            int w = 0, h = 0;
            BitmapSize(k.overlay, w, h);
            int ox = k.overlayIsMenu ? (wx + ww + 16) : (px + k.overlayDx);
            // Vertically center each overlay by its own height.
            int oy = (CH - h) / 2;
            AlphaBlit(hdcM, ox, oy, w, h, k.overlay, k.overlayAlpha);
        }
        if (k.ghost && hGhost)
            DrawIconEx(hdcM, k.ghostX, k.ghostY, hGhost, 48, 48, 0, nullptr, DI_NORMAL);
        if (k.marquee)
        {
            // Резиновое лассо как в приложении: полупрозрачная заливка +
            // яркая граница вокруг виджета (растёт с marqueePad).
            Gdiplus::Graphics gp(hdcM);
            int m = k.marqueePad;
            Gdiplus::SolidBrush fill(Gdiplus::Color(40, 51, 153, 255));
            Gdiplus::Pen pen(Gdiplus::Color(220, 51, 153, 255), 2.0f);
            gp.FillRectangle(&fill, wx - m, wy - m, ww + 2 * m, wh + 2 * m);
            gp.DrawRectangle(&pen, wx - m, wy - m, ww + 2 * m, wh + 2 * m);
        }
        SelectObject(hdcM, hOld);
        DeleteDC(hdcM);
        ReleaseDC(nullptr, hdcS);

        Gdiplus::Bitmap* frame = Gdiplus::Bitmap::FromHBITMAP(hFrame, nullptr);
        DeleteObject(hFrame);
        if (!frame || frame->GetLastStatus() != Gdiplus::Ok)
        {
            delete frame;
            wprintf(L"frame convert failed\n");
            st = Gdiplus::GenericError;
            break;
        }
        size_t propSize = sizeof(Gdiplus::PropertyItem) + sizeof(ULONG);
        Gdiplus::PropertyItem* delay = (Gdiplus::PropertyItem*)malloc(propSize);
        delay->id = 0x5100; // PropertyTagFrameDelay
        delay->length = sizeof(ULONG);
        delay->type = 4; // PropertyTagTypeLong
        delay->value = delay + 1;
        *(ULONG*)delay->value = (ULONG)k.delayCs;
        frame->SetPropertyItem(delay);
        free(delay);

        Gdiplus::EncoderParameters ep;
        ep.Count = 1;
        ep.Parameter[0].Guid = Gdiplus::EncoderSaveFlag;
        ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        ep.Parameter[0].NumberOfValues = 1;
        if (first)
        {
            ULONG v = Gdiplus::EncoderValueMultiFrame;
            ep.Parameter[0].Value = &v;
            st = frame->Save(outPath, &gifClsid, &ep);
            if (st == Gdiplus::Ok)
            {
                multi = frame;
                frame = nullptr;
                first = false;
            }
        }
        else
        {
            ULONG v = Gdiplus::EncoderValueFrameDimensionTime;
            ep.Parameter[0].Value = &v;
            st = multi->SaveAdd(frame, &ep);
        }
        delete frame;
        if (st != Gdiplus::Ok)
            wprintf(L"gif save failed at frame %zu: %d\n", fi, (int)st);
    }
    if (st == Gdiplus::Ok && multi)
    {
        Gdiplus::EncoderParameters ep;
        ep.Count = 1;
        ep.Parameter[0].Guid = Gdiplus::EncoderSaveFlag;
        ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        ep.Parameter[0].NumberOfValues = 1;
        ULONG v = Gdiplus::EncoderValueFlush;
        ep.Parameter[0].Value = &v;
        st = multi->SaveAdd(&ep);
        delete multi;
        multi = nullptr;
    }
    else
    {
        delete multi;
        multi = nullptr;
    }
    // Бесконечный цикл анимации (иначе играет один раз и встаёт).
    if (st == Gdiplus::Ok && !AddGifLoop(outPath))
    {
        wprintf(L"loop extension failed\n");
        st = Gdiplus::GenericError;
    }

    for (HBITMAP b : owned) DeleteObject(b);
    if (hGhost && ghostOwned) DestroyIcon(hGhost);
    r.Shutdown();
    Gdiplus::GdiplusShutdown(gdiToken);
    if (SUCCEEDED(hrCo) || hrCo == S_FALSE) CoUninitialize();
    if (st != Gdiplus::Ok)
    {
        wprintf(L"gif failed: %d\n", (int)st);
        return 7;
    }
    // Total duration estimate.
    int totalCs = 0;
    for (auto& k : keys) totalCs += k.delayCs;
    wprintf(L"done: %s (%zu frames, ~%d.%ds)\n", outPath, keys.size(),
        totalCs / 100, totalCs % 100);
    return 0;
}
