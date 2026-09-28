#pragma once
// Единый файловый лог (%APPDATA%\DesktopGroupManager\debug.log) +
// OutputDebugString. Заменяет разрозненные WL/BL/LogGrid/MQLog/DebugLog/
// LogTiming — у каждого был свой велосипед с тем же файлом.
#include <Windows.h>

// tag без префиксов: итоговая строка "TAG formatted...".
void AppLog(const wchar_t* tag, const wchar_t* fmt, ...);
void AppLogA(const char* tag, const char* fmt, ...);
