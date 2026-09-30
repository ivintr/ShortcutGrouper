// render_demo (dev tool): renders app UI with the real WidgetRenderer headless
// and assembles docs demo GIF: widget glow -> popup slide/fade -> row hover
// walk -> overflow group -> 3x3 grid -> selected state -> tray-style menu.
// Not part of the app.
#include <Windows.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <objbase.h>
#include <cstdio>
#include <string>
#include <vector>
#include "Renderer.h"
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

// One animation keyframe. Only one of popup/menu/bigShown is drawn;
// widget always drawn unless widgetHidden.
struct Key {
    HBITMAP widget = nullptr;
    HBITMAP popup = nullptr;
    int popupDx = 0;
    BYTE popupAlpha = 255;
    HBITMAP menu = nullptr;
    BYTE menuAlpha = 255;
    int delayCs = 5;
};

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

    auto mkGroup = [](const wchar_t* name, int grid,
                      const std::vector<std::pair<const wchar_t*, const wchar_t*>>& apps) {
        GroupData g;
        g.id = name;
        g.name = name;
        g.gridSize = grid;
        g.showOverflow = true;
        for (auto& a : apps)
        {
            ShortcutInfo si;
            si.name = a.first;
            si.lnkPath = a.second;
            si.targetPath = a.second;
            g.shortcuts.push_back(si);
        }
        return g;
    };
    GroupData gDemo = mkGroup(L"Demo", 2, {
        { L"Notepad", L"C:\\Windows\\System32\\notepad.exe" },
        { L"Paint", L"C:\\Windows\\System32\\mspaint.exe" },
        { L"Console", L"C:\\Windows\\System32\\cmd.exe" },
        { L"Explorer", L"C:\\Windows\\explorer.exe" },
    });
    GroupData gBig = mkGroup(L"Tools", 2, {
        { L"Notepad", L"C:\\Windows\\System32\\notepad.exe" },
        { L"Paint", L"C:\\Windows\\System32\\mspaint.exe" },
        { L"Console", L"C:\\Windows\\System32\\cmd.exe" },
        { L"Explorer", L"C:\\Windows\\explorer.exe" },
        { L"Registry", L"C:\\Windows\\System32\\regedit.exe" },
        { L"TaskMgr", L"C:\\Windows\\System32\\taskmgr.exe" },
        { L"SysInfo", L"C:\\Windows\\System32\\msinfo32.exe" },
    });
    GroupData gGrid = mkGroup(L"Grid", 3, {
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
    RenderedBitmap wPlain = r.RenderWidget(gDemo, ctx, 0, false);
    RenderedBitmap wG1 = r.RenderWidget(gDemo, ctx, 90, false);
    RenderedBitmap wG2 = r.RenderWidget(gDemo, ctx, 170, false);
    RenderedBitmap wGlow = r.RenderWidget(gDemo, ctx, 255, false);
    RenderedBitmap wSel = r.RenderWidget(gDemo, ctx, 0, true);
    RenderedBitmap wBig = r.RenderWidget(gBig, ctx, 0, false);
    RenderedBitmap wGrid = r.RenderWidget(gGrid, ctx, 0, false);
    RenderedBitmap pb = r.RenderPopup(gDemo, -1);
    std::vector<RenderedBitmap> pbHov;
    for (int i = 0; i < 4; i++)
        pbHov.push_back(r.RenderPopup(gDemo, i));
    RenderedBitmap pbBig = r.RenderPopup(gBig, 5);
    if (!wPlain.hBitmap || !pb.hBitmap)
    {
        wprintf(L"render failed\n");
        return 4;
    }

    // Tray-style menu, same items as the real tray menu.
    std::vector<WidgetRenderer::MenuRenderItem> items;
    {
        WidgetRenderer::MenuRenderItem t;
        t.text = L"Shortcut Grouper"; t.isTitle = true;
        items.push_back(t);
    }
    {
        WidgetRenderer::MenuRenderItem s;
        s.separator = true;
        items.push_back(s);
    }
    const wchar_t* names[] = {
        L"Search", L"Hide widgets", L"Refresh", L"Settings", L"Check for updates",
    };
    for (auto n : names)
    {
        WidgetRenderer::MenuRenderItem it;
        it.text = n;
        items.push_back(it);
    }
    {
        WidgetRenderer::MenuRenderItem s;
        s.separator = true;
        items.push_back(s);
    }
    {
        WidgetRenderer::MenuRenderItem it;
        it.text = L"Exit";
        items.push_back(it);
    }
    RenderedBitmap mb = r.RenderMenu(items, -1, -1, 0, 0, true);
    RenderedBitmap mbH3 = r.RenderMenu(items, 3, -1, 0, 0, true);
    RenderedBitmap mbH5 = r.RenderMenu(items, 5, -1, 0, 0, true);
    RenderedBitmap mbH6 = r.RenderMenu(items, 6, -1, 0, 0, true);
    if (!mb.hBitmap)
    {
        wprintf(L"menu render failed\n");
        return 4;
    }

    int ww = 0, wh = 0, pw = 0, ph = 0, mw = 0, mh = 0;
    int pbw = 0, pbh = 0;
    BitmapSize(wPlain.hBitmap, ww, wh);
    BitmapSize(pb.hBitmap, pw, ph);
    BitmapSize(mb.hBitmap, mw, mh);
    BitmapSize(pbBig.hBitmap, pbw, pbh);
    const int sideW = pw > mw ? pw : mw;
    const int CW = 60 + ww + 16 + sideW + 60;
    int contentH = ph > mh ? ph : mh;
    if (pbh > contentH) contentH = pbh;
    if (wh > contentH) contentH = wh;
    const int CH = contentH + 80;
    const int wx = 50, wy = (CH - wh) / 2;
    const int px = wx + ww + 16, py = (CH - ph) / 2;
    const int pbx = wx + ww + 16, pby = (CH - pbh) / 2;
    SIZE msz = r.MeasureMenu(items);
    const int mx = wx + ww + 16, my = (CH - msz.cy) / 2;
    wprintf(L"widget %dx%d popup %dx%d bigpopup %dx%d menu %dx%d canvas %dx%d\n",
        ww, wh, pw, ph, pbw, pbh, mw, mh, CW, CH);

    std::vector<Key> keys;
    auto K = [&](HBITMAP w, HBITMAP p, int dx, BYTE pa,
                 HBITMAP m, BYTE ma, int d) {
        Key k;
        k.widget = w; k.popup = p; k.popupDx = dx; k.popupAlpha = pa;
        k.menu = m; k.menuAlpha = ma; k.delayCs = d;
        keys.push_back(k);
    };
    HBITMAP W0 = wPlain.hBitmap, WG1 = wG1.hBitmap, WG2 = wG2.hBitmap,
            WG = wGlow.hBitmap, WS = wSel.hBitmap,
            WB = wBig.hBitmap, W3 = wGrid.hBitmap;
    HBITMAP P = pb.hBitmap, PB = pbBig.hBitmap;
    HBITMAP M = mb.hBitmap;
    // 1. Idle, glow ramp (smooth hover-in).
    K(W0, nullptr, 0, 0, nullptr, 0, 55);
    K(WG1, nullptr, 0, 0, nullptr, 0, 4);
    K(WG2, nullptr, 0, 0, nullptr, 0, 4);
    K(WG, nullptr, 0, 0, nullptr, 0, 35);
    // 2. Popup slides + fades in (4 smooth steps).
    K(WG, P, -pw + 30, 80, nullptr, 0, 4);
    K(WG, P, -pw * 2 / 3, 140, nullptr, 0, 4);
    K(WG, P, -pw / 3, 200, nullptr, 0, 4);
    K(W0, P, 0, 255, nullptr, 0, 65);
    // 3. Row hover walks the whole list.
    K(W0, pbHov[0].hBitmap, 0, 255, nullptr, 0, 22);
    K(W0, pbHov[1].hBitmap, 0, 255, nullptr, 0, 22);
    K(W0, pbHov[2].hBitmap, 0, 255, nullptr, 0, 22);
    K(W0, pbHov[3].hBitmap, 0, 255, nullptr, 0, 45);
    // 4. Overflow group (+3 badge) and its full popup.
    K(WB, nullptr, 0, 0, nullptr, 0, 55);
    K(WB, PB, 0, 255, nullptr, 0, 75);
    // 5. 3x3 grid widget, then selected state.
    K(W3, nullptr, 0, 0, nullptr, 0, 60);
    K(WS, nullptr, 0, 0, nullptr, 0, 55);
    // 6. Popup fades out, menu fades in.
    K(W0, P, 0, 110, nullptr, 0, 4);
    K(nullptr, nullptr, 0, 0, M, 120, 4);
    K(nullptr, nullptr, 0, 0, M, 255, 20);
    // 7. Menu hover walks, hold, fade out.
    K(nullptr, nullptr, 0, 0, mbH3.hBitmap, 255, 25);
    K(nullptr, nullptr, 0, 0, mbH5.hBitmap, 255, 25);
    K(nullptr, nullptr, 0, 0, mbH6.hBitmap, 255, 50);
    K(nullptr, nullptr, 0, 0, M, 110, 4);
    K(W0, nullptr, 0, 0, nullptr, 0, 55);

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
            AlphaBlit(hdcM, wx, (CH - h) / 2, w, h, k.widget, 255);
        }
        if (k.popup)
        {
            int w = 0, h = 0;
            BitmapSize(k.popup, w, h);
            int isBig = (k.popup == PB);
            AlphaBlit(hdcM, (isBig ? pbx : px) + k.popupDx,
                (isBig ? pby : py), w, h, k.popup, k.popupAlpha);
        }
        if (k.menu)
        {
            int w = 0, h = 0;
            BitmapSize(k.menu, w, h);
            AlphaBlit(hdcM, mx, my, w, h, k.menu, k.menuAlpha);
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
    }
    else
    {
        delete multi;
    }

    DeleteObject(wPlain.hBitmap);
    DeleteObject(wG1.hBitmap);
    DeleteObject(wG2.hBitmap);
    DeleteObject(wGlow.hBitmap);
    DeleteObject(wSel.hBitmap);
    DeleteObject(wBig.hBitmap);
    DeleteObject(wGrid.hBitmap);
    DeleteObject(pb.hBitmap);
    for (auto& h : pbHov) DeleteObject(h.hBitmap);
    DeleteObject(pbBig.hBitmap);
    DeleteObject(mb.hBitmap);
    DeleteObject(mbH3.hBitmap);
    DeleteObject(mbH5.hBitmap);
    DeleteObject(mbH6.hBitmap);
    r.Shutdown();
    Gdiplus::GdiplusShutdown(gdiToken);
    if (SUCCEEDED(hrCo) || hrCo == S_FALSE) CoUninitialize();
    if (st != Gdiplus::Ok)
    {
        wprintf(L"gif failed: %d\n", (int)st);
        return 7;
    }
    wprintf(L"done: %s (%zu frames)\n", outPath, keys.size());
    return 0;
}
