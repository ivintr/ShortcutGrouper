#include "ColorUtils.h"
#include <algorithm>
#include <windows.h>

ColorHsv RgbToHsv(int rgb)
{
    double r = ((rgb >> 16) & 255) / 255.0;
    double g = ((rgb >> 8) & 255) / 255.0;
    double b = (rgb & 255) / 255.0;
    double mx = max(r, max(g, b));
    double mn = min(r, min(g, b));
    double d = mx - mn;
    ColorHsv out = { 0, mx == 0 ? 0 : d / mx, mx };
    if (d != 0)
    {
        if (mx == r) out.h = 60 * fmod((g - b) / d, 6);
        else if (mx == g) out.h = 60 * ((b - r) / d + 2);
        else out.h = 60 * ((r - g) / d + 4);
        if (out.h < 0) out.h += 360;
    }
    return out;
}

int HsvToRgb(double h, double s, double v)
{
    h = fmod(h, 360.0);
    if (h < 0) h += 360;
    if (s < 0) s = 0; if (s > 1) s = 1;
    if (v < 0) v = 0; if (v > 1) v = 1;
    double c = v * s;
    double x = c * (1 - fabs(fmod(h / 60.0, 2) - 1));
    double m = v - c;
    double r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else { r = c; b = x; }
    int R = (int)((r + m) * 255 + 0.5);
    int G = (int)((g + m) * 255 + 0.5);
    int B = (int)((b + m) * 255 + 0.5);
    if (R < 0) R = 0; if (R > 255) R = 255;
    if (G < 0) G = 0; if (G > 255) G = 255;
    if (B < 0) B = 0; if (B > 255) B = 255;
    return (R << 16) | (G << 8) | B;
}

int HsvToRgb(const ColorHsv& hsv)
{
    return HsvToRgb(hsv.h, hsv.s, hsv.v);
}
