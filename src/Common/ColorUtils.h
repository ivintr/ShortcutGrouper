#pragma once
#include <cmath>

struct ColorHsv { double h; double s; double v; }; // h: 0..360

ColorHsv RgbToHsv(int rgb);          // rgb: 0xRRGGBB
int HsvToRgb(double h, double s, double v); // -> 0xRRGGBB
int HsvToRgb(const ColorHsv& hsv);
