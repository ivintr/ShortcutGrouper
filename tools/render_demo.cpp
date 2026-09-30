// render_demo (dev tool): renders app UI with the real WidgetRenderer headless
// and assembles docs demo GIF: widget -> popup slide+fade -> row hover ->
// tray-style menu. Not part of the app.
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

// One animation keyframe.
struct Key {
    int widgetGlow = 0;    // -1 = hidden
    HBITMAP popup = nullptr;
    int popupDx = 0;       // slide offset (negative = tucked behind widget)
    BYTE popupAlpha = 255;
    HBITMAP menu = nullptr;
    BYTE menuAlpha = 255;
    int delayCs = 8;
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

    // Demo group: real system binaries => real icons, no test files needed.
    GroupData g;
    g.id = L"demo";
    g.name = L"Demo";
    g.gridSize = 2;
    g.showOverflow = true;
    const wchar_t* apps[][2] = {
        { L"Notepad", L"C:\\Windows\\System32\\notepad.exe" },
        { L"Paint", L"C:\\Windows\\System32\\mspaint.exe" },
        { L"Console", L"C:\\Windows\\System32\\cmd.exe" },
        { L"Explorer", L"C:\\Windows\\explorer.exe" },
    };
    for (auto& a : apps)
    {
        ShortcutInfo si;
        si.name = a[0];
        si.lnkPath = a[1];
        si.targetPath = a[1];
        g.shortcuts.push_back(si);
    }

    WidgetRenderContext ctx;
    RenderedBitmap wPlain = r.RenderWidget(g, ctx, 0, false);
    RenderedBitmap wGlow = r.RenderWidget(g, ctx, 220, false);
    RenderedBitmap pb = r.RenderPopup(g, -1);
    RenderedBitmap pbH2 = r.RenderPopup(g, 2);
    RenderedBitmap pbH4 = r.RenderPopup(g, 4);
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
    RenderedBitmap mbHov = r.RenderMenu(items, 5, -1, 0, 0, true);
    if (!mb.hBitmap)
    {
        wprintf(L"menu render failed\n");
        return 4;
    }

    int ww = 0, wh = 0, pw = 0, ph = 0, mw = 0, mh = 0, dummy = 0;
    BitmapSize(wPlain.hBitmap, ww, wh);
    BitmapSize(pb.hBitmap, pw, ph);
    BitmapSize(mb.hBitmap, mw, mh);

    const int CW = 60 + ww + 16 + (pw > mw ? pw : mw) + 60;
    int contentH = ph > mh ? ph : mh;
    if (wh > contentH) contentH = wh;
    const int CH = contentH + 80;
    const int wx = 50, wy = (CH - wh) / 2;
    const int px = wx + ww + 16, py = (CH - ph) / 2;
    SIZE msz = r.MeasureMenu(items);
    const int mx = wx + ww + 16, my = (CH - msz.cy) / 2;
    wprintf(L"widget %dx%d popup %dx%d menu %dx%d canvas %dx%d\n",
        ww, wh, pw, ph, mw, mh, CW, CH);

    std::vector<Key> keys;
    auto K = [&](int glow, HBITMAP p, int dx, BYTE pa,
                 HBITMAP m, BYTE ma, int d) {
        Key k;
        k.widgetGlow = glow; k.popup = p; k.popupDx = dx; k.popupAlpha = pa;
        k.menu = m; k.menuAlpha = ma; k.delayCs = d;
        keys.push_back(k);
    };
    // 1. Widget idle, then hover glow.
    K(0, nullptr, 0, 0, nullptr, 0, 60);
    K(220, nullptr, 0, 0, nullptr, 0, 45);
    // 2. Popup slides in + fades in.
    K(220, pb.hBitmap, -pw + 20, 90, nullptr, 0, 5);
    K(220, pb.hBitmap, -pw / 2, 160, nullptr, 0, 5);
    K(220, pb.hBitmap, -20, 220, nullptr, 0, 5);
    K(0, pb.hBitmap, 0, 255, nullptr, 0, 70);
    // 3. Row hover walks down.
    K(0, pbH2.hBitmap, 0, 255, nullptr, 0, 30);
    K(0, pbH4.hBitmap, 0, 255, nullptr, 0, 55);
    // 4. Popup fades, menu fades in.
    K(0, pb.hBitmap, 0, 110, nullptr, 0, 5);
    K(-1, nullptr, 0, 0, mb.hBitmap, 120, 5);
    K(-1, nullptr, 0, 0, mb.hBitmap, 255, 25);
    K(-1, nullptr, 0, 0, mbHov.hBitmap, 255, 70);
    // 5. Back to idle widget.
    K(0, nullptr, 0, 0, nullptr, 0, 50);

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
        if (k.widgetGlow >= 0)
            AlphaBlit(hdcM, wx, wy, ww, wh,
                k.widgetGlow > 0 ? wGlow.hBitmap : wPlain.hBitmap, 255);
        if (k.popup)
        {
            int w = 0, h = 0;
            BitmapSize(k.popup, w, h);
            AlphaBlit(hdcM, px + k.popupDx, py, w, h, k.popup, k.popupAlpha);
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
    DeleteObject(wGlow.hBitmap);
    DeleteObject(pb.hBitmap);
    DeleteObject(pbH2.hBitmap);
    DeleteObject(pbH4.hBitmap);
    DeleteObject(mb.hBitmap);
    DeleteObject(mbHov.hBitmap);
    r.Shutdown();
    Gdiplus::GdiplusShutdown(gdiToken);
    if (SUCCEEDED(hrCo) || hrCo == S_FALSE) CoUninitialize();
    if (st != Gdiplus::Ok)
    {
        wprintf(L"gif failed: %d\n", (int)st);
        return 7;
    }
    wprintf(L"done: %s\n", outPath);
    return 0;
}
