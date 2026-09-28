#include "Logger.h"
#include "AppPaths.h"
#include <cstdio>
#include <cstdarg>
#include <string>

static void WriteLine(const wchar_t* line)
{
    std::wstring dir = GetAppDataDir();
    if (!dir.empty())
    {
        std::wstring path = dir + L"\\debug.log";
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8");
        if (f) { fwprintf(f, L"%s\n", line); fclose(f); }
    }
    OutputDebugStringW(line);
    OutputDebugStringW(L"\n");
}

void AppLog(const wchar_t* tag, const wchar_t* fmt, ...)
{
    WCHAR buf[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    if (tag && *tag)
    {
        WCHAR line[2100];
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"%s %s", tag, buf);
        WriteLine(line);
    }
    else
    {
        WriteLine(buf);
    }
}

void AppLogA(const char* tag, const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    char line[2100];
    if (tag && *tag)
        _snprintf_s(line, _countof(line), _TRUNCATE, "%s %s", tag, buf);
    else
    {
        strncpy_s(line, buf, _TRUNCATE);
    }
    // Консольная локаль для OutputDebugStringA не важна — файл пишем wide.
    WCHAR wline[2100];
    MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, _countof(wline));
    WriteLine(wline);
}
