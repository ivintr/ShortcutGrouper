#pragma once
// Автообновление с GitHub Releases. Проверка — в фоне, не чаще раза в сутки.
// Найдена версия новее — баллун в трей; установка — по пункту меню трея:
// скачивает Setup.exe релиза во TEMP, запускает и завершает приложение.
#include <Windows.h>
#include <string>

namespace Updater {
    // IPC-окно для баллунов трея (то же, что у WidgetManager).
    void SetNotifyWindow(HWND hWnd);
    // Фоновая проверка. force=true — игнорировать суточный троттлинг
    // (пункт меню «Проверить обновления»).
    void CheckAsync(bool force = false);
    // Есть ли известная новая версия (после проверки).
    bool HasUpdate();
    // Строка новой версии (L"1.0.3.0") или пусто.
    std::wstring UpdateVersion();
    // Скачать Setup и запустить (в фоне), затем попросить выход через IDM_EXIT.
    // Возвращает false сразу, если нечего ставить или уже качаем.
    bool InstallUpdate(HWND hExitWnd);
    // Текущая версия приложения.
    const wchar_t* CurrentVersion();
}
