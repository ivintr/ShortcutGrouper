#pragma once

#include <Windows.h>

// Simple logging helper. Writes messages to %TEMP%\DesktopGroupManager.log
// Thread-safe via critical section.

void InitializeLogger();
void UninitializeLogger();
void LogInfo(const wchar_t* fmt, ...);
void LogError(const wchar_t* fmt, ...);
