#include "LogHelper.h"
#include <cstdio>
#include <cstdarg>
#include <strsafe.h>

static INIT_ONCE s_initOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION s_cs;
static BOOL s_csReady = FALSE;
static WCHAR s_logPath[32768] = {};

static BOOL CALLBACK InitLoggerOnce(PINIT_ONCE, PVOID, PVOID*)
{
    WCHAR tmp[32768] = {};
    DWORD n = GetTempPathW(ARRAYSIZE(tmp), tmp);
    if (n == 0 || n >= ARRAYSIZE(tmp))
        return FALSE;
    if (FAILED(StringCchCatW(tmp, ARRAYSIZE(tmp), L"DesktopGroupManager.log")))
        return FALSE;
    InitializeCriticalSection(&s_cs);
    wcscpy_s(s_logPath, tmp);
    s_csReady = TRUE;
    return TRUE;
}

void InitializeLogger()
{
    PVOID ctx = nullptr;
    if (!InitOnceExecuteOnce(&s_initOnce, InitLoggerOnce, nullptr, &ctx) || !s_csReady)
        return;

    LogInfo(L"--- Log session started ---");
}

void UninitializeLogger()
{
    if (!s_csReady)
        return;

    LogInfo(L"--- Log session ended ---");
    // Leave перед Delete: Enter+Delete без Leave — дедлок/UB.
    // (Лока берётся только в Log*, здесь она свободна.)
    DeleteCriticalSection(&s_cs);
    s_csReady = FALSE;
}

static void LogWrite(const wchar_t* level, const wchar_t* fmt, va_list args)
{
    if (!s_csReady || !s_logPath[0])
        return;

    EnterCriticalSection(&s_cs);

    FILE* f = nullptr;
    if (_wfopen_s(&f, s_logPath, L"a, ccs=UTF-8") == 0 && f)
    {
        SYSTEMTIME st = {};
        GetLocalTime(&st);

        // Только wide-вывод: fprintf мешал ориентацию потока (ccs=UTF-8).
        fwprintf(f, L"[%04d-%02d-%02d %02d:%02d:%02d] [%s] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, level);

        vfwprintf(f, fmt, args);

        fwprintf(f, L"\n");
        fclose(f);
    }

    LeaveCriticalSection(&s_cs);
}

void LogInfo(const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogWrite(L"INFO", fmt, args);
    va_end(args);
}

void LogError(const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogWrite(L"ERROR", fmt, args);
    va_end(args);
}
