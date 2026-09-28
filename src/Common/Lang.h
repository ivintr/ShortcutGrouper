#pragma once
// Локализация: LangId + объявления. Реализация — в Lang.cpp
// (чтобы не тянуть registry-IO в каждый TU через header-only).
#include <Windows.h>

enum class LangId : DWORD { Auto = 0, Russian = 1, English = 2 };

enum class Str
{
    // Меню виджета
    W_DefaultGroupName,
    W_Open,
    W_Rename,
    W_HideName,
    W_Sorting,
    W_SortOff,
    W_SortAsc,
    W_SortDesc,
    W_SortType,
    W_SortRecent,
    W_Color,
    W_GridSize,
    W_CustomColor,
    W_SelCount,       // %d — число выбранных групп
    W_UngroupAll,
    W_UngroupAllConfirm, // %d — число групп
    W_DeleteAll,
    W_DeleteAllConfirm,  // %d — число групп
    W_ClearSel,
    W_Ungroup,
    W_DeleteGroup,
    W_UngroupConfirm, // %s — имя группы
    W_UngroupCaption,
    W_DeleteConfirm,  // %s — имя группы
    W_DeleteCaption,
    W_DeadMsg,        // %s — имя ярлыка
    W_DeadCaption,
    W_LaunchFailMsg,  // %s — путь, %d — код ShellExecute
    W_LaunchFailCaption,
    W_ShellVerbFail,  // %s — имя команды (cut/copy/delete)
    // Автообновление
    U_AvailTitle,
    U_AvailMsg,       // %s — новая версия
    U_UptodateTitle,
    U_UptodateMsg,
    U_NetFailMsg,
    U_DownFailMsg,
    W_PrunedTitle,
    W_PrunedMsg,        // %s — имя группы
    W_PrunedMany,       // %d — число ярлыков
    W_RenameError,
    W_ErrorCaption,
    W_CmdCut,
    W_CmdCopy,
    W_CmdRename,
    W_CmdDelete,
    W_File,
    W_OpenContaining,
    W_UninstallApp,
    W_UninstallConfirm, // %s — имя приложения
    W_UninstallCaption,
    W_UninstallError,
    W_UninstallApps,        // %d — число приложений
    W_UninstallAppsConfirm, // %d — число приложений
    // Цвета стекла
    C_Default,
    C_Blue,
    C_Teal,
    C_Green,
    C_Yellow,
    C_Orange,
    C_Red,
    C_Pink,
    C_Purple,
    C_Dark,
    // Трей
    T_Refresh,
    T_Search,
    T_HideWidgets,
    T_ShowWidgets,
    T_Settings,
    T_Update,         // проверить обновления / скачать и установить
    T_Exit,
    // Настройки
    S_Title,
    S_General,
    S_Groups,
    S_Autostart,
    S_AutostartDesc,
    S_Snap,
    S_SnapDesc,
    S_HotSearch,
    S_HotSearchDesc,
    S_HotToggle,
    S_HotToggleDesc,
    S_HotPress,
    S_Overflow,
    S_OverflowDesc,
    S_OnboardTitle,
    S_OnboardText,
    S_Prune,
    S_PruneDesc,
    S_DefGrid,
    S_DefGridDesc,
    S_Language,
    S_LanguageDesc,
    S_LangAuto,
    S_LangRussian,
    S_LangEnglish,
    S_Close,
    // Диалоги менеджера
    D_NameTitle,
    D_NamePrompt,
    D_Ok,
    D_Cancel,
    D_DefaultGroup, // %04d-%02d-%02d — дата
    D_RenameTitle,
    D_RenamePrompt,
    D_FileRenameTitle,
    D_FileRenamePrompt,
    D_ColorTitle,
    D_NewGroup,
    // Поиск по ярлыкам
    Q_Hint,   // плейсхолдер строки поиска
    Q_Empty,  // ничего не найдено
    // Shell-расширение
    SH_Title,
    SH_Tooltip,
};

class Lang
{
public:
    static LangId GetOverride();
    static void SetOverride(LangId lang);
    static void Initialize();
    static bool IsRussian();
    static const wchar_t* Get(Str id);

private:
    static LONG s_init;
    static bool s_russian;
    static bool EffectiveRussian();
    static const wchar_t* Russian(Str id);
    static const wchar_t* English(Str id);
};
