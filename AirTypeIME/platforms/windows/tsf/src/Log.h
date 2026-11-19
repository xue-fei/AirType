#pragma once

// 极简日志：同时写 OutputDebugStringA 和 stderr。
// TSF DLL 被注入到别的进程后没有控制台，调试器附加时 OutputDebugString 才有用；
// 独立 exe 阶段 stderr 足够。

#include <cstdarg>
#include <cstdio>
#include <windows.h>

namespace airtype {

inline void log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    char line[1100];
    snprintf(line, sizeof(line), "[airtype] %s\n", buf);

    OutputDebugStringA(line);
    fputs(line, stderr);
    fflush(stderr);
}

inline void log_hr(const char* what, HRESULT hr) {
    log("%s -> 0x%08X", what, static_cast<unsigned>(hr));
}

}  // namespace airtype
