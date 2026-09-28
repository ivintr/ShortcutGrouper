#pragma once
#include <Windows.h>

class WidgetRenderer;

// Диалог выбора цвета стекла группы: HSV-кольцо + SV-квадрат.
// renderer нужен для матового стеклянного фона (как у контекстного меню).
// Возвращает true, если цвет подтверждён (outRgb = 0xRRGGBB).
bool ShowColorDialog(HWND parent, WidgetRenderer* renderer, int initialRgb, int& outRgb);
