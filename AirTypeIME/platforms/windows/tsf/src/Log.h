#pragma once

// 极简日志：同时写 OutputDebugStringA 和 stderr。
// TSF DLL 被注入到别的进程后没有控制台，调试器附加时 OutputDebugString 才有用；
// 独立 exe 阶段 stderr 足够。

#include <cstdarg>
#include <cstdio>
#include <windows.h>

namespace airtype {

// 文件日志：注入到微信/浏览器等宿主后没有控制台、OutputDebugString 也难以
// 抓取，排查「图标定位」这类问题时必须能落盘。
// 非追踪构建下不写文件；追踪构建（AIRTYPE_TRACE_STATUS）下无条件追加写到
// 固定路径，省掉给宿主进程设环境变量的麻烦。
// 每次调用 fopen/fclose 开销不小，故只在高频路径之外使用；定位链路每 40ms
// 最多写一行，可接受。
#ifdef AIRTYPE_TRACE_STATUS
// 追踪构建：无条件写到固定文件，省掉给宿主进程设环境变量的麻烦。
// 所有加载本 DLL 的进程共用这一个文件（各自追加，行内不带进程名但带 pid）。
constexpr const char* kTraceLogFile = "C:\\Temp\\airtype_trace.log";

inline void log_to_file(const char* text) {
    FILE* f = fopen(kTraceLogFile, "a");
    if (f == nullptr) {
        return;
    }
    fprintf(f, "[pid %lu] ", static_cast<unsigned long>(GetCurrentProcessId()));
    fputs(text, f);
    fputc('\n', f);
    fclose(f);
}
#else
inline void log_to_file(const char*) {}
#endif

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

    // 追踪构建下同时落盘，便于从宿主进程回收证据。
    log_to_file(line);
}

inline void log_hr(const char* what, HRESULT hr) {
    log("%s -> 0x%08X", what, static_cast<unsigned>(hr));
}

}  // namespace airtype
