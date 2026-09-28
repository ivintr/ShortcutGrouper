#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <set>
#include "WidgetTypes.h"
#include "Renderer.h"
#include "DesktopWidget.h"
#include "PopupWindow.h"

class SearchWindow;

class WidgetManager {
public:    bool Initialize(WidgetRenderer* renderer);
    void Shutdown();

    void LoadGroups();
    void SaveGroups();

    bool CreateGroup(const std::wstring& name, const std::vector<std::wstring>& lnkPaths,
        int spawnX = -1, int spawnY = -1, bool skipMove = false);
    bool AddShortcutToGroup(const std::wstring& groupId, const std::wstring& lnkPath);
    bool RemoveShortcutFromGroup(const std::wstring& groupId, int index);
    bool MoveShortcutInGroup(const std::wstring& groupId, int from, int to);
    bool ApplySort(const std::wstring& groupId, GroupSortMode mode);
    // Учёт запуска ярлыка (счётчик + время для «Недавние сверху»).
    void RecordShortcutLaunch(const std::wstring& groupId, int index);
    bool SetGroupColor(const std::wstring& groupId, int glassColor);
    bool SetHideName(const std::wstring& groupId, bool hide);
    bool SetOverflowBadge(const std::wstring& groupId, bool show);
    bool SetGridSize(const std::wstring& groupId, int grid); // 2 или 3
    bool RemoveGroup(const std::wstring& groupId);
    bool RenameGroup(const std::wstring& groupId, const std::wstring& newName);
    bool UngroupGroup(const std::wstring& groupId);
    bool DeleteGroupWithFiles(const std::wstring& groupId);
    // Жив ли ярлык: файл хранилища на месте и цель доступна.
    // Цели на недоступных носителях (вынутая флешка, отвал сети)
    // и URL/протоколы считаются живыми — их не трогаем.
    static bool IsShortcutAlive(const ShortcutInfo& si);
    // steam://rungameid/<appid> -> appId (для удаления игр Steam).
    static bool ParseSteamAppId(const std::wstring& target, DWORD& appId);
    // Вычистить битые ярлыки во всех группах (файлы-сироты в хранилище
    // стираются, опустевшие группы удаляются). Возвращает число убранных.
    int PruneDeadShortcuts();
    // То же для одной группы. true = группа опустела и удалена
    // (виджет снесён — вызывателю нельзя трогать this!).
    bool PruneGroup(const std::wstring& groupId);
    // Убрать один битый ярлык без возврата на рабочий стол.
    bool RemoveDeadShortcut(const std::wstring& groupId, int index);
    // Перечитать группу с диска после shell-меню (удаление/переименование
    // файла снаружи): битые убираются, имена и цели обновляются.
    // true = группа исчезла целиком (виджет и попапы снесены).
    bool RefreshGroupFromDisk(const std::wstring& groupId);
    const GroupData* FindGroup(const std::wstring& groupId) const;
    std::wstring FindGroupAtPoint(POINT pt);
    void SaveGroupPosition(const std::wstring& groupId, int x, int y);
    void SavePopupSize(const std::wstring& groupId, int popupW, int popupListH);
    void SnapAllWidgetsToGrid();

    void CreateAllWidgets(HWND hDesktopParent);
    void DestroyAllWidgets();
    void RefreshWidgets();
    // Перерисовать виджеты и попапы (после отложенной загрузки иконок).
    void RefreshWidgetIcons();

    void OnGroupCommand(const std::wstring& cmdLine);
    void OnSettingsToggle();
    void ShowSearch();
    void ToggleSearch();
    // Окно для уведомлений трея (IPC-окно main). Не владеем.
    void SetNotifyWindow(HWND hWnd) { m_hNotifyWnd = hWnd; }
    // Баллун трея из любого потока логики (удаляется получателем).
    void NotifyBalloon(const std::wstring& title, const std::wstring& text);
    // Показать/скрыть все виджеты (режим «чистый стол»). Позиции хранятся.
    void ToggleWidgetsVisible();
    bool IsWidgetsHidden() const { return m_widgetsHidden; }
    std::wstring ShowNameDialog(HWND parent);
    // Возвращает true, если подтверждено (имя может быть пустым).
    bool ShowRenameDialog(HWND parent, const std::wstring& currentName, std::wstring& outName);
    // Переименование файла (для пункта shell-меню): пустое имя запрещено.
    bool ShowFileRenameDialog(HWND parent, const std::wstring& currentFileName,
        std::wstring& outName);

    const std::vector<GroupData>& GetGroups() const { return m_groups; }
    WidgetRenderer* GetRenderer() const { return m_renderer; }
    // Аварийный возврат иконок (выполнять в живом процессе, не из SendMessage).
    void RescueDesktopIcons();

    // --- Мультивыделение групп (Ctrl+клик, перетаскивание пачкой) ---
    // Только runtime-состояние, в JSON не сохраняется.
    void ToggleSelectGroup(const std::wstring& groupId);
    void SelectSingleGroup(const std::wstring& groupId);
    void SetSelectedGroups(const std::vector<std::wstring>& ids);
    void ClearSelection();
    bool IsSelected(const std::wstring& groupId) const;
    size_t SelectedCount() const { return m_selected.size(); }
    std::vector<DesktopWidget*> SelectedWidgets();
    DesktopWidget* FindWidget(const std::wstring& groupId);

private:
    std::wstring GetGroupsDir();
    std::wstring GetJsonPath();
    // Сдвинуть иконки рабочего стола из-под виджетов (no-op без конфликтов).
    void PushIconsOutOfWidgets();
    void ApplyWidgetsVisibility();
    void RefreshSelectionVisuals();
    std::wstring MoveLnkToStorage(const std::wstring& lnkPath, const std::wstring& groupId);
    static void ApplySortMode(GroupData& group);
    static void DeletePathSilent(const std::wstring& path);

    WidgetRenderer* m_renderer = nullptr;
    std::vector<GroupData> m_groups;
    std::vector<DesktopWidget*> m_widgets;
    std::set<std::wstring> m_selected; // id выделенных групп
    SearchWindow* m_search = nullptr;  // окно глобального поиска
    bool m_widgetsHidden = false;      // виджеты скрыты (хоткей/трей)
    HWND m_hNotifyWnd = nullptr;       // IPC-окно для баллунов трея
    HWND m_hDesktopParent = nullptr;

    // Стереть файл ярлыка, но только внутри нашего хранилища
    // (в режиме --no-move lnkPath указывает наружу — его не трогаем).
    static void EraseStoredLnk(const std::wstring& groupsDir, const ShortcutInfo& si);
};

PopupWindow* FindPopupByGroupId(const std::wstring& id);
void RegisterPopupWindow(PopupWindow* popup);
void UnregisterPopupWindow(PopupWindow* popup);

void SetDesktopParent(HWND h);
void SetGlobalRenderer(WidgetRenderer* r);
void SetGlobalManager(WidgetManager* m);

// Прямоугольники всех наших виджетов (для выталкивания иконок).
std::vector<RECT> CollectAllWidgetRects();
