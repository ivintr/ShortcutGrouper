#include "Logger.h"
#include "AppPaths.h"
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

// Ротация: лог растёт бесконечно (клики, GRID). Держим файл в пределах
// ~4 МБ: при превышении оставляем последний ~1 МБ (по границе строк).
// Проверка дешёвая (размер файла), делается на каждую запись.
static const unsigned long long kLogMaxBytes = 4ULL * 1024 * 1024;
static const unsigned long long kLogKeepBytes = 1ULL * 1024 * 1024;

static void RotateLogIfNeeded(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return; // файла ещё нет — нечего резать
    ULARGE_INTEGER size;
    size.LowPart = fad.nFileSizeLow;
    size.HighPart = fad.nFileSizeHigh;
    if (size.QuadPart < kLogMaxBytes)
        return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return;
    if (_fseeki64(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return;
    }
    __int64 total = _ftelli64(f);
    __int64 start = total > (__int64)kLogKeepBytes ? total - (__int64)kLogKeepBytes : 0;
    if (_fseeki64(f, start, SEEK_SET) != 0)
    {
        fclose(f);
        return;
    }
    std::vector<char> tail;
    tail.resize((size_t)(total - start));
    size_t got = tail.empty() ? 0 : fread(tail.data(), 1, tail.size(), f);
    fclose(f);
    if (got == 0)
        return;

    // Начинаем с целой строки. Файл — UTF-8 (режим ccs=UTF-8):
    // перевод строки — один байт 0x0A. Многобайтовый символ резать нельзя,
    // поэтому отступаем до границы UTF-8 последовательности.
    size_t begin = 0;
    for (size_t i = 0; i < got; i++)
    {
        if (tail[i] == '\n')
        {
            begin = i + 1;
            break;
        }
    }
    while (begin < got && (tail[begin] & 0xC0) == 0x80)
        begin++; // середина UTF-8 символа — пропускаем его целиком
    FILE* w = nullptr;
    if (_wfopen_s(&w, path.c_str(), L"wb") != 0 || !w)
        return;
    // BOM UTF-8, как пишет ccs=UTF-8 режим CRT.
    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    fwrite(bom, 1, 3, w);
    if (begin < got)
        fwrite(tail.data() + begin, 1, got - begin, w);
    fclose(w);
}

static void WriteLine(const wchar_t* line)
{
    std::wstring dir = GetAppDataDir();
    if (!dir.empty())
    {
        std::wstring path = dir + L"\\debug.log";
        RotateLogIfNeeded(path);
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
