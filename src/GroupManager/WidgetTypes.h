#pragma once

#include <Windows.h>
#include <Shlwapi.h>
#include <Shlobj.h>
#include <objbase.h>
#include <string>
#include <vector>
#include "AppPaths.h"

struct ShortcutInfo {
    std::wstring name;
    std::wstring lnkPath;
    std::wstring targetPath;
    int uses = 0;              // сколько раз запускали (для «Недавние сверху»)
    long long lastUsed = 0;    // время последнего запуска (time_t)
};

// Режим сортировки ярлыков внутри группы (хранится в JSON)
enum class GroupSortMode {
    Off = 0,     // ручной порядок (drag-and-drop)
    NameAsc = 1, // по имени А-Я (натуральный порядок: 2 < 10)
    NameDesc = 2,// по имени Я-А
    Type = 3,    // по типу цели, затем по имени
    Recent = 4   // недавние сверху (по lastUsed, затем по имени)
};

struct GroupData {
    std::wstring id;
    std::wstring name;
    int x = 0;
    int y = 0;
    // Запомненный размер раскрытого попапа: 0/-1 = авто
    int popupW = 0;
    int popupListH = -1;
    int sortMode = (int)GroupSortMode::Off;
    int glassColor = 0xFFFFFF; // тинт стекла 0xRRGGBB, по умолчанию белый
    bool hideName = false; // не показывать подпись на виджете
    bool showOverflow = true; // счётчик «+N» (истина + глобальная настройка)
    int gridSize = 2; // сетка мини-иконок виджета: 2 (2x2) или 3 (3x3)
    std::vector<ShortcutInfo> shortcuts;
};

constexpr int WIDGET_CELL     = 76;
// Подпись — полосой ВНУТРИ стеклянного бокса снизу (как плитки):
// виджет целиком влезает в ячейку сетки, снизу всегда воздух.
constexpr int WIDGET_LABEL_H  = 18;
constexpr int WIDGET_W        = WIDGET_CELL;
constexpr int WIDGET_H        = WIDGET_CELL; // == высоте: подпись внутри
constexpr int WIDGET_RADIUS   = 14;
constexpr int ICON_SIZE       = 22;
constexpr int ICON_PAD        = 18;

constexpr int POPUP_W         = 280;
constexpr int POPUP_MIN_W     = 200;
constexpr int POPUP_MAX_W     = 520;
constexpr int POPUP_BORDER_SZ = 6;
constexpr int POPUP_CORNER_SZ = 16;

// Шаг сетки для режима выравнивания (как иконки рабочего стола)
constexpr int SNAP_GRID_X     = WIDGET_W + 12;
constexpr int SNAP_GRID_Y     = WIDGET_H + 12;
constexpr int POPUP_ITEM_H    = 44;
constexpr int POPUP_PAD       = 8;
constexpr int POPUP_RADIUS    = 12;
constexpr int POPUP_HEADER_H  = 36;

constexpr int IPC_CLASS_MSG   = WM_USER + 1;
constexpr int WM_DND_DROP     = WM_USER + 50;

// Высота виджета: без подписи — компактный квадрат.
// Размер ячейки растёт с сеткой: иконки 22px + зазоры 4px + поля 14px.
// 2x2 -> 76 (как раньше), 3x3 -> 102.
inline int WidgetGrid(const GroupData& group)
{
    int g = group.gridSize;
    if (g < 2) g = 2;
    if (g > 3) g = 3;
    return g;
}

inline int WidgetCell(const GroupData& group)
{
    int grid = WidgetGrid(group);
    return grid * ICON_SIZE + (grid - 1) * 4 + 28;
}

inline int WidgetWidth(const GroupData& group)
{
    return WidgetCell(group);
}

inline int WidgetHeight(const GroupData& group)
{
    (void)group;
    // Высота всегда равна боксу: подпись внутри, hideName прячет только текст
    // (геометрия стабильна — сетка не прыгает от переименований).
    return WidgetCell(group);
}

// В группу можно объединять любые файлы и папки (.lnk, .url,
// обычные файлы, каталоги): проверяется только существование пути.
inline bool IsGroupableShortcut(const std::wstring& path)
{
    if (path.empty()) return false;
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Имя IPC-окна синглтона (раньше дублировалось тремя строками в коде).
constexpr wchar_t kGroupManagerIpcClass[] = L"GroupManagerIPC";

// Данные balloon-уведомления трея (владеет получатель WM_TRAY_BALLOON).
struct TrayBalloon {
    std::wstring title;
    std::wstring text;
};

// main.cpp слушает его же под своим именем (значение общее).
constexpr UINT WM_TRAY_BALLOON_MSG = WM_USER + 206;

inline std::wstring GenerateGroupId()
{    GUID guid;
    // Без проверки здесь оказывался мусорный ID (коллизии групп).
    if (FAILED(CoCreateGuid(&guid)))
        return L"";
    WCHAR buf[40];
    if (swprintf_s(buf, L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]) < 0)
        return L"";
    return buf;
}
