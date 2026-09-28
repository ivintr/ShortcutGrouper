#pragma once
// Квотирование аргументов командной строки: раньше ArgvQuote дублировался
// в нескольких местах, EscapePsSingleQuotes жил в PopupWindow.cpp.
#include <string>

// Квотирование по правилам CommandLineToArgvW: голые кавычки и хвостовые
// бэкслэши в пути рвали командную строку (обрыв/инъекция).
inline void ArgvQuote(const std::wstring& arg, std::wstring& out)
{
    out += L'"';
    size_t backslashes = 0;
    for (wchar_t c : arg)
    {
        if (c == L'\\') { backslashes++; continue; }
        if (c == L'"')
        {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
            backslashes = 0;
        }
        else
        {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out += c;
        }
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
}

// Экранирование для PowerShell-одинарных кавычек: ' -> ''.
// Без этого значение из реестра рвало команду (инъекция).
inline std::wstring EscapePsSingleQuotes(const std::wstring& s)
{
    std::wstring r;
    r.reserve(s.size());
    for (wchar_t c : s)
    {
        if (c == L'\'') r += L"''";
        else r += c;
    }
    return r;
}
