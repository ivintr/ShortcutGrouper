// render_demo (TEMP dev tool): renders widget + popup frames with the real
// WidgetRenderer headless and assembles docs demo GIF. Not part of the app.
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

struct Frame {
    int x, y;          // paste offset on canvas
    HBITMAP bmp = nullptr;
    int w = 0, h = 0;
    int delayCs = 0;   // frame delay, 1/100 s
};

static bool BitmapSize(HBITMAP hbm, int& w, int& h, void*& bits, int& stride)
{
    BITMAP bm = {};
    if (!hbm || GetObjectW(hbm, sizeof(bm), &bm) != sizeof(bm)) return false;
    if (!bm.bmBits || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
    w = bm.bmWidth;
    h = abs(bm.bmHeight);
    bits = bm.bmBits;
    stride = bm.bmWidthBytes;
    return true;
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
    RenderedBitmap wb = r.RenderWidget(g, ctx, 0, false);
    RenderedBitmap pb = r.RenderPopup(g, -1);
    RenderedBitmap pbHov = r.RenderPopup(g, 1);
    if (!wb.hBitmap || !pb.hBitmap)
    {
        wprintf(L"render failed\n");
        return 4;
    }
    int ww = 0, wh = 0, pw = 0, ph = 0, hw = 0, hh = 0, s = 0;
    void *wbBits = nullptr, *pbBits = nullptr, *hbBits = nullptr;
    BitmapSize(wb.hBitmap, ww, wh, wbBits, s);
    BitmapSize(pb.hBitmap, pw, ph, pbBits, s);
    BitmapSize(pbHov.hBitmap, hw, hh, hbBits, s);
    wprintf(L"widget %dx%d popup %dx%d\n", ww, wh, pw, ph);

    // Canvas: dark backdrop, widget left, popup slides in from behind it.
    const int CW = ww + pw + 120;
    const int CH = (ph > wh ? ph : wh) + 80;
    const int wx = 50, wy = (CH - wh) / 2;
    const int pxFull = wx + ww + 16, py = (CH - ph) / 2;

    struct Key { HBITMAP bmp; int dx; int delay; };
    std::vector<Key> keys = {
        { wb.hBitmap, 0, 70 },          // widget alone
        { wb.hBitmap, 0, 25 },
        { pb.hBitmap, -pw / 2, 10 },    // popup slides in (2 steps)
        { pb.hBitmap, 0, 12 },
        { pb.hBitmap, 0, 110 },         // hold
        { pbHov.hBitmap, 0, 90 },       // hover highlight
    };

    CLSID gifClsid;
    if (GetEncoderClsid(L"image/gif", &gifClsid) < 0)
    {
        wprintf(L"no gif encoder\n");
        return 5;
    }

    bool first = true;
    Gdiplus::Bitmap* multi = nullptr;
    Gdiplus::Status st = Gdiplus::Ok;
    for (size_t fi = 0; fi < keys.size() && st == Gdiplus::Ok; fi++)
    {
        // Compose frame with GDI.
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
        // Dark backdrop (#1b1b1b).
        HBRUSH bg = CreateSolidBrush(RGB(27, 27, 27));
        RECT rc = { 0, 0, CW, CH };
        FillRect(hdcM, &rc, bg);
        DeleteObject(bg);
        // Widget.
        {
            HDC hdcW = CreateCompatibleDC(hdcM);
            HGDIOBJ oW = SelectObject(hdcW, wb.hBitmap);
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            GdiAlphaBlend(hdcM, wx, wy, ww, wh, hdcW, 0, 0, ww, wh, bf);
            SelectObject(hdcW, oW);
            DeleteDC(hdcW);
        }
        // Popup (possibly slid).
        {
            HBITMAP src = (keys[fi].bmp == pbHov.hBitmap) ? pbHov.hBitmap : pb.hBitmap;
            int px = pxFull + keys[fi].dx;
            HDC hdcP = CreateCompatibleDC(hdcM);
            HGDIOBJ oP = SelectObject(hdcP, src);
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            GdiAlphaBlend(hdcM, px, py, pw, ph, hdcP, 0, 0, pw, ph, bf);
            SelectObject(hdcP, oP);
            DeleteDC(hdcP);
        }
        SelectObject(hdcM, hOld);
        DeleteDC(hdcM);
        ReleaseDC(nullptr, hdcS);

        // HBITMAP -> GDI+ Bitmap + per-frame delay.
        Gdiplus::Bitmap* frame = Gdiplus::Bitmap::FromHBITMAP(hFrame, nullptr);
        DeleteObject(hFrame);
        if (!frame || frame->GetLastStatus() != Gdiplus::Ok)
        {
            delete frame;
            wprintf(L"frame convert failed\n");
            return 6;
        }
        size_t propSize = sizeof(Gdiplus::PropertyItem) + sizeof(ULONG);
        Gdiplus::PropertyItem* delay = (Gdiplus::PropertyItem*)malloc(propSize);
        delay->id = 0x5100; // PropertyTagFrameDelay
        delay->length = sizeof(ULONG);
        delay->type = 4; // PropertyTagTypeLong
        delay->value = delay + 1;
        *(ULONG*)delay->value = (ULONG)keys[fi].delay;
        frame->SetPropertyItem(delay);
        free(delay);

        Gdiplus::Status st2;
        if (first)
        {
            Gdiplus::EncoderParameters ep;
            ep.Count = 1;
            ep.Parameter[0].Guid = Gdiplus::EncoderSaveFlag;
            ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
            ep.Parameter[0].NumberOfValues = 1;
            ULONG v = Gdiplus::EncoderValueMultiFrame;
            ep.Parameter[0].Value = &v;
            st2 = frame->Save(outPath, &gifClsid, &ep);
            if (st2 == Gdiplus::Ok)
            {
                multi = frame;
                frame = nullptr; // живёт до конца (на нём SaveAdd/Flush)
                first = false;
            }
        }
        else
        {
            Gdiplus::EncoderParameters ep;
            ep.Count = 1;
            ep.Parameter[0].Guid = Gdiplus::EncoderSaveFlag;
            ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
            ep.Parameter[0].NumberOfValues = 1;
            ULONG v = Gdiplus::EncoderValueFrameDimensionTime;
            ep.Parameter[0].Value = &v;
            st2 = multi->SaveAdd(frame, &ep);
        }
        delete frame;
        if (st2 != Gdiplus::Ok)
        {
            wprintf(L"gif save failed at frame %zu: %d\n", fi, (int)st2);
            st = st2;
        }
    }
    // Flush multi-frame.
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
    if (st != Gdiplus::Ok)
    {
        wprintf(L"gif flush failed: %d\n", (int)st);
        DeleteObject(wb.hBitmap);
        DeleteObject(pb.hBitmap);
        DeleteObject(pbHov.hBitmap);
        r.Shutdown();
        Gdiplus::GdiplusShutdown(gdiToken);
        return 7;
    }

    DeleteObject(wb.hBitmap);
    DeleteObject(pb.hBitmap);
    DeleteObject(pbHov.hBitmap);
    r.Shutdown();
    Gdiplus::GdiplusShutdown(gdiToken);
    if (SUCCEEDED(hrCo) || hrCo == S_FALSE) CoUninitialize();
    wprintf(L"done: %s\n", outPath);
    return 0;
}
