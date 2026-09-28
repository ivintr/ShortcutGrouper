#pragma once
#include <Windows.h>
#include <vector>

// Real desktop icon grid: pitch and origin are read
// from the system SysListView32 (respects DPI and folder view).
struct DesktopGrid {
    bool valid = false;
    int stepX = 0;
    int stepY = 0;
    int originX = 0; // screen coords of a grid node
    int originY = 0;
};

// Polls the ListView (with a couple-seconds cache). Always safe to call:
// on failure returns valid=false.
DesktopGrid QueryDesktopGrid();
// То же, но шаг/фаза голосуются только по иконкам монитора, где точка pt.
// На мультимониторных сетапах с разным DPI глобальный шаг чужой
// и виджеты встают криво — эта версия берёт свой для каждого экрана.
DesktopGrid QueryDesktopGridForPoint(POINT pt);

// Рабочая область монитора у точки (общий хелпер виджета и попапа).
RECT MonitorWorkRect(POINT pt);

// Screen rects of real desktop icons (one rect per icon, sized as a grid
// cell: pos .. pos+step). With a couple-seconds cache. On failure returns
// an empty vector. Used to keep widgets from landing on files/icons.
// validOut (optional): false = опрос не удался (Explorer подвис), данные
// неполные — НЕ трактовать пустой список как «пустой стол».
std::vector<RECT> QueryDesktopIconRects(bool* validOut = nullptr);

// Сбросить кэш QueryDesktopIconRects (после программного сдвига иконок).
void InvalidateDesktopIconCache();

// Сдвигает иконки рабочего стола из-под заданных прямоугольников (обычно
// виджетов) вниз по сетке, как Fences. Возвращает число подвинутых.
// Без конфликтов — no-op. Кэш иконок после сдвига сбрасывается.
int PushDesktopIconsOutOfWidgets(const std::vector<RECT>& widgetRects);

// Аварийный возврат иконок: разложить оказавшиеся в одной точке (стопка)
// по свободным клеткам рядом с виджетами (виджеты точно на видимом месте),
// затем вытолкнуть из-под виджетов.
void SpreadAndPushDesktopIcons(const std::vector<RECT>& widgetRects);

// Диагностика в %APPDATA%\DesktopGroupManager\debug.log ("GRID ...").
void DebugLogGrid(const char* stage, int v1, int v2, int v3, int v4, int v5, int v6);

// Диагностика: все Progman/WorkerW/DefView/ListView с видимостью,
// размерами и числом иконок — понять, в какое окно мы тыкаем.
void DumpDesktopWindows();
