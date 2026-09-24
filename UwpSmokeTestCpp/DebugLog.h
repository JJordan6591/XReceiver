#pragma once

#include <cstdarg>
#include <cstdio>

namespace rx
{
    inline void Log([[maybe_unused]] _Printf_format_string_ wchar_t const* format, ...)
    {
#ifdef _DEBUG
        wchar_t buffer[512];
        va_list args;
        va_start(args, format);
        _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
        va_end(args);
        OutputDebugStringW(L"[rx] ");
        OutputDebugStringW(buffer);
        OutputDebugStringW(L"\n");
#endif
    }
}
