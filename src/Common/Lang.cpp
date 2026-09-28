#include "Lang.h"

LONG Lang::s_init = 0;
bool Lang::s_russian = true;

LangId Lang::GetOverride()
{
    DWORD ov = 0;
    DWORD cb = sizeof(ov);
    DWORD type = 0;
    HKEY hk = nullptr;
    LangId r = LangId::Auto;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\DesktopGroupManager",
            0, KEY_READ, &hk) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(hk, L"Language", nullptr, &type,
                (BYTE*)&ov, &cb) == ERROR_SUCCESS && type == REG_DWORD && ov <= 2)
            r = (LangId)ov;
        RegCloseKey(hk);
    }
    return r;
}

void Lang::SetOverride(LangId lang)
{
    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\DesktopGroupManager",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) == ERROR_SUCCESS)
    {
        DWORD v = (DWORD)lang;
        RegSetValueExW(hk, L"Language", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(hk);
    }
}

void Lang::Initialize()
{
    if (InterlockedCompareExchange(&s_init, 1, 0) != 0)
        return;
    s_russian = true;
    LangId ov = GetOverride();
    if (ov == LangId::Russian) return;
    if (ov == LangId::English) { s_russian = false; return; }
    s_russian = (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN);
}

bool Lang::IsRussian()
{
    Initialize();
    return EffectiveRussian();
}

const wchar_t* Lang::Get(Str id)
{
    Initialize();
    return EffectiveRussian() ? Russian(id) : English(id);
}

bool Lang::EffectiveRussian()
{
    LangId ov = GetOverride();
    if (ov == LangId::Russian) return true;
    if (ov == LangId::English) return false;
    return s_russian;
}

const wchar_t* Lang::Russian(Str id)
{
    switch (id)
    {
    case Str::W_DefaultGroupName: return L"Группа";
    case Str::W_Open:             return L"Открыть";
    case Str::W_Rename:           return L"Переименовать…";
    case Str::W_HideName:         return L"Скрыть имя";
    case Str::W_Sorting:          return L"Сортировка";
    case Str::W_SortOff:          return L"Без сортировки";
    case Str::W_SortAsc:          return L"По имени (А–Я)";
    case Str::W_SortDesc:         return L"По имени (Я–А)";
    case Str::W_SortType:        return L"По типу";
    case Str::W_SortRecent:      return L"Недавние сверху";
    case Str::W_Color:            return L"Цвет";
    case Str::W_GridSize:         return L"Размер сетки";
    case Str::W_CustomColor:      return L"Свой цвет…";
    case Str::W_SelCount:         return L"Выбрано: %d";
    case Str::W_UngroupAll:       return L"Разгруппировать все";
    case Str::W_UngroupAllConfirm: return L"Разгруппировать выбранные группы (%d)?\nЯрлыки вернутся на рабочий стол.";
    case Str::W_DeleteAll:        return L"Удалить все…";
    case Str::W_DeleteAllConfirm: return L"Удалить выбранные группы (%d)?\nИх ярлыки будут удалены.";
    case Str::W_ClearSel:         return L"Снять выделение";
    case Str::W_Ungroup:          return L"Разгруппировать";
    case Str::W_DeleteGroup:      return L"Удалить группу…";
    case Str::W_UngroupConfirm:   return L"Разгруппировать «%s»?\nЯрлыки вернутся на рабочий стол.";
    case Str::W_UngroupCaption:   return L"Разгруппировать";
    case Str::W_DeleteConfirm:    return L"Удалить группу «%s»?\nЯрлыки группы будут удалены.";
    case Str::W_DeleteCaption:    return L"Удалить группу";
    case Str::W_DeadMsg:          return L"«%s» больше не запускается — похоже, приложение удалено.\nЯрлык убран из группы.";
    case Str::W_DeadCaption:      return L"Ярлык недоступен";
    case Str::W_LaunchFailMsg:    return L"Не удалось запустить «%s».\nКод ошибки: %d.";
    case Str::W_LaunchFailCaption: return L"Ошибка запуска";
    case Str::W_ShellVerbFail:    return L"Не удалось выполнить команду «%s».";
    case Str::U_AvailTitle:       return L"Доступно обновление";
    case Str::U_AvailMsg:         return L"Вышла версия %s. Откройте меню трея, чтобы скачать и установить.";
    case Str::U_UptodateTitle:    return L"Обновления";
    case Str::U_UptodateMsg:      return L"У вас последняя версия.";
    case Str::U_NetFailMsg:       return L"Не удалось проверить обновления (нет сети?).";
    case Str::U_DownFailMsg:      return L"Не удалось скачать или запустить обновление.";
    case Str::W_PrunedTitle:       return L"Группа убрана со стола";
    case Str::W_PrunedMsg:         return L"«%s»: все ярлыки битые, группа удалена.";
    case Str::W_PrunedMany:        return L"Убрано битых ярлыков: %d (пустые группы удалены).";
    case Str::W_RenameError:      return L"Не удалось переименовать файл.";
    case Str::W_ErrorCaption:     return L"Ошибка";
    case Str::W_CmdCut:     return L"Вырезать";
    case Str::W_CmdCopy:    return L"Копир.";
    case Str::W_CmdRename:  return L"Переимен.";
    case Str::W_CmdDelete:  return L"Удалить";
    case Str::W_File:       return L"Файл";
    case Str::W_OpenContaining: return L"Расположение файла";
    case Str::W_UninstallApp:     return L"Удалить приложение";
    case Str::W_UninstallConfirm: return L"Удалить приложение «%s» с компьютера?\nБудет запущена штатная программа удаления.\nПрограмма и её данные будут удалены безвозвратно.";
    case Str::W_UninstallCaption: return L"Удаление приложения";
    case Str::W_UninstallError:   return L"Не удалось запустить удаление.\n%s";
    case Str::W_UninstallApps:        return L"Удалить приложения (%d)…";
    case Str::W_UninstallAppsConfirm: return L"Удалить %d приложений?\nДля каждого запустится своя программа удаления.";
    case Str::C_Default:  return L"По умолчанию";
    case Str::C_Blue:     return L"Синий";
    case Str::C_Teal:     return L"Бирюзовый";
    case Str::C_Green:    return L"Зелёный";
    case Str::C_Yellow:   return L"Жёлтый";
    case Str::C_Orange:   return L"Оранжевый";
    case Str::C_Red:      return L"Красный";
    case Str::C_Pink:     return L"Розовый";
    case Str::C_Purple:   return L"Фиолетовый";
    case Str::C_Dark:     return L"Тёмный";
    case Str::T_Refresh:  return L"Обновить виджеты";
    case Str::T_Search:   return L"Поиск…";
    case Str::T_HideWidgets: return L"Скрыть виджеты";
    case Str::T_ShowWidgets: return L"Показать виджеты";    case Str::T_Settings: return L"Настройки…";
    case Str::T_Update:   return L"Проверить обновления";
    case Str::T_Exit:     return L"Выход";
    case Str::S_Title:    return L"Настройки";
    case Str::S_General:  return L"Общие";
    case Str::S_Groups:   return L"Группы";
    case Str::S_Autostart:     return L"Запускать вместе с Windows";
    case Str::S_AutostartDesc: return L"Приложение запускается автоматически при входе в систему";
    case Str::S_Snap:     return L"Привязка к сетке";
    case Str::S_SnapDesc: return L"Виджеты выравниваются по сетке значков рабочего стола";
    case Str::S_HotSearch:     return L"Горячая клавиша поиска";
    case Str::S_HotSearchDesc: return L"Открытие окна поиска с клавиатуры";
    case Str::S_HotToggle:     return L"Горячая клавиша виджетов";
    case Str::S_HotToggleDesc: return L"Показать или скрыть все виджеты";
    case Str::S_HotPress:      return L"Нажмите клавиши…";
    case Str::S_Overflow:      return L"Счётчик переполнения";
    case Str::S_OverflowDesc:  return L"Показывать «+N» в подписи переполненных групп";
    case Str::S_OnboardTitle:  return L"Группы на рабочем столе";
    case Str::S_OnboardText:   return L"Выделите два ярлыка и нажмите «Объединить в группу». Виджеты можно таскать, группы открываются кликом.";
    case Str::S_Prune:     return L"Удалять ярлыки удалённых приложений";
    case Str::S_PruneDesc: return L"Битые ярлыки, включая удалённые игры Steam, убираются из групп автоматически";
    case Str::S_DefGrid:     return L"Размер новых групп";
    case Str::S_DefGridDesc: return L"Сколько значков показывать в новых группах: 2×2 или 3×3";
    case Str::S_Language:     return L"Язык";
    case Str::S_LanguageDesc: return L"Язык интерфейса приложения";
    case Str::S_LangAuto:     return L"Авто";
    case Str::S_LangRussian:  return L"Русский";
    case Str::S_LangEnglish:  return L"English";
    case Str::S_Close:    return L"Закрыть";
    case Str::D_NameTitle:  return L"Объединить в группу";
    case Str::D_NamePrompt: return L"Введите имя группы:";
    case Str::D_Ok:         return L"ОК";
    case Str::D_Cancel:     return L"Отмена";
    case Str::D_DefaultGroup: return L"Группа %04d-%02d-%02d";
    case Str::D_RenameTitle:  return L"Переименовать группу";
    case Str::D_RenamePrompt: return L"Новое имя группы (пусто — без подписи):";
    case Str::D_FileRenameTitle:  return L"Переименовать";
    case Str::D_FileRenamePrompt: return L"Новое имя файла:";
    case Str::D_ColorTitle:  return L"Выбор цвета";
    case Str::D_NewGroup:   return L"Новая группа";
    case Str::Q_Hint:   return L"Введите название ярлыка…";
    case Str::Q_Empty:  return L"Ничего не найдено";
    case Str::SH_Title:   return L"Объединить в группу";
    case Str::SH_Tooltip: return L"Объединить выбранные элементы в группу на рабочем столе";
    }
    return L"";
}

const wchar_t* Lang::English(Str id)
{
    switch (id)
    {
    case Str::W_DefaultGroupName: return L"Group";
    case Str::W_Open:             return L"Open";
    case Str::W_Rename:           return L"Rename…";
    case Str::W_HideName:         return L"Hide name";
    case Str::W_Sorting:          return L"Sorting";
    case Str::W_SortOff:          return L"No sorting";
    case Str::W_SortAsc:          return L"By name (A–Z)";
    case Str::W_SortDesc:         return L"By name (Z–A)";
    case Str::W_SortType:         return L"By type";
    case Str::W_SortRecent:       return L"Recent first";
    case Str::W_Color:            return L"Color";
    case Str::W_GridSize:         return L"Grid size";
    case Str::W_CustomColor:      return L"Custom color…";
    case Str::W_SelCount:         return L"Selected: %d";
    case Str::W_UngroupAll:       return L"Ungroup all";
    case Str::W_UngroupAllConfirm: return L"Ungroup selected groups (%d)?\nShortcuts will return to the desktop.";
    case Str::W_DeleteAll:        return L"Delete all…";
    case Str::W_DeleteAllConfirm: return L"Delete selected groups (%d)?\nTheir shortcuts will be deleted.";
    case Str::W_ClearSel:         return L"Clear selection";
    case Str::W_Ungroup:          return L"Ungroup";
    case Str::W_DeleteGroup:      return L"Delete group…";
    case Str::W_UngroupConfirm:   return L"Ungroup “%s”?\nShortcuts will return to the desktop.";
    case Str::W_UngroupCaption:   return L"Ungroup";
    case Str::W_DeleteConfirm:    return L"Delete group “%s”?\nThe group’s shortcuts will be deleted.";
    case Str::W_DeleteCaption:    return L"Delete group";
    case Str::W_DeadMsg:          return L"“%s” no longer launches — the app seems uninstalled.\nThe shortcut was removed from the group.";
    case Str::W_DeadCaption:      return L"Shortcut unavailable";
    case Str::W_LaunchFailMsg:    return L"Failed to launch “%s”.\nError code: %d.";
    case Str::W_LaunchFailCaption: return L"Launch failed";
    case Str::W_ShellVerbFail:    return L"Failed to run the “%s” command.";
    case Str::U_AvailTitle:       return L"Update available";
    case Str::U_AvailMsg:         return L"Version %s is out. Open the tray menu to download and install it.";
    case Str::U_UptodateTitle:    return L"Updates";
    case Str::U_UptodateMsg:      return L"You have the latest version.";
    case Str::U_NetFailMsg:       return L"Could not check for updates (offline?).";
    case Str::U_DownFailMsg:      return L"Failed to download or launch the update.";
    case Str::W_PrunedTitle:       return L"Group removed from desktop";
    case Str::W_PrunedMsg:         return L"“%s”: all shortcuts are broken, the group was removed.";
    case Str::W_PrunedMany:        return L"Removed %d dead shortcuts (empty groups deleted).";
    case Str::W_RenameError:      return L"Failed to rename the file.";
    case Str::W_ErrorCaption:     return L"Error";
    case Str::W_CmdCut:     return L"Cut";
    case Str::W_CmdCopy:    return L"Copy";
    case Str::W_CmdRename:  return L"Rename";
    case Str::W_CmdDelete:  return L"Delete";
    case Str::W_File:       return L"File";
    case Str::W_OpenContaining: return L"Open file location";
    case Str::W_UninstallApp:     return L"Uninstall app";
    case Str::W_UninstallConfirm: return L"Uninstall “%s” from this PC?\nThe standard uninstaller will be launched.\nThe program and its data will be removed permanently.";
    case Str::W_UninstallCaption: return L"Uninstall app";
    case Str::W_UninstallError:   return L"Failed to start uninstall.\n%s";
    case Str::W_UninstallApps:        return L"Uninstall apps (%d)…";
    case Str::W_UninstallAppsConfirm: return L"Uninstall %d apps?\nEach will launch its own uninstaller.";
    case Str::C_Default:  return L"Default";
    case Str::C_Blue:     return L"Blue";
    case Str::C_Teal:     return L"Teal";
    case Str::C_Green:    return L"Green";
    case Str::C_Yellow:   return L"Yellow";
    case Str::C_Orange:   return L"Orange";
    case Str::C_Red:      return L"Red";
    case Str::C_Pink:     return L"Pink";
    case Str::C_Purple:   return L"Purple";
    case Str::C_Dark:     return L"Dark";
    case Str::T_Refresh:  return L"Refresh widgets";
    case Str::T_Search:   return L"Search…";
    case Str::T_HideWidgets: return L"Hide widgets";
    case Str::T_ShowWidgets: return L"Show widgets";
    case Str::T_Settings: return L"Settings…";
    case Str::T_Update:   return L"Check for updates";
    case Str::T_Exit:     return L"Exit";
    case Str::S_Title:    return L"Settings";
    case Str::S_General:  return L"General";
    case Str::S_Groups:   return L"Groups";
    case Str::S_Autostart:     return L"Start with Windows";
    case Str::S_AutostartDesc: return L"The app starts automatically when you sign in";
    case Str::S_Snap:     return L"Snap to grid";
    case Str::S_SnapDesc: return L"Widgets align to the desktop icon grid";
    case Str::S_HotSearch:     return L"Search hotkey";
    case Str::S_HotSearchDesc: return L"Open the search window with keyboard";
    case Str::S_HotToggle:     return L"Widgets hotkey";
    case Str::S_HotToggleDesc: return L"Show or hide all widgets";
    case Str::S_HotPress:      return L"Press keys…";
    case Str::S_Overflow:      return L"Overflow counter";
    case Str::S_OverflowDesc:  return L"Show “+N” in the caption of overflowing groups";
    case Str::S_OnboardTitle:  return L"Desktop groups";
    case Str::S_OnboardText:   return L"Select two shortcuts and click “Group”. Drag widgets, click a group to open.";
    case Str::S_Prune:     return L"Remove shortcuts of uninstalled apps";
    case Str::S_PruneDesc: return L"Broken shortcuts, including removed Steam games, are removed from groups automatically";
    case Str::S_DefGrid:     return L"New groups size";
    case Str::S_DefGridDesc: return L"How many icons to show in new groups: 2×2 or 3×3";
    case Str::S_Language:     return L"Language";
    case Str::S_LanguageDesc: return L"Application interface language";
    case Str::S_LangAuto:     return L"Auto";
    case Str::S_LangRussian:  return L"Русский";
    case Str::S_LangEnglish:  return L"English";
    case Str::S_Close:    return L"Close";
    case Str::D_NameTitle:  return L"Group Shortcuts";
    case Str::D_NamePrompt: return L"Enter group name:";
    case Str::D_Ok:         return L"OK";
    case Str::D_Cancel:     return L"Cancel";
    case Str::D_DefaultGroup: return L"Group %04d-%02d-%02d";
    case Str::D_RenameTitle:  return L"Rename group";
    case Str::D_RenamePrompt: return L"New group name (empty — no caption):";
    case Str::D_FileRenameTitle:  return L"Rename";
    case Str::D_FileRenamePrompt: return L"New file name:";
    case Str::D_ColorTitle:  return L"Pick a color";
    case Str::D_NewGroup:   return L"New Group";
    case Str::Q_Hint:   return L"Type shortcut name…";
    case Str::Q_Empty:  return L"No matches";
    case Str::SH_Title:   return L"Group Shortcuts";
    case Str::SH_Tooltip: return L"Group the selected items into a desktop group";
    }
    return L"";
}
