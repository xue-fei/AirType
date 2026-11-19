#include "ModuleUtil.h"

#include <windows.h>

namespace airtype {

namespace {

std::wstring DirOf(const wchar_t* path) {
    if (path == nullptr) {
        return {};
    }
    const std::wstring full(path);
    const size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return full.substr(0, slash + 1);
}

}  // namespace

std::wstring SelfModuleDir() {
    // 用本函数自身的地址反查模块句柄：外壳是 in-proc 服务器，会被系统注入
    // 到任意宿主进程，宿主 exe 目录并不可靠，必须按 DLL 自己所在目录解析
    // airtype_ffi.dll 与配置。UNCHANGED_REFCOUNT 表示不增加引用计数。
    HMODULE handle = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&SelfModuleDir), &handle) ||
        handle == nullptr) {
        return {};
    }
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(handle, path, MAX_PATH) == 0) {
        return {};
    }
    return DirOf(path);
}

std::wstring HostExeDir() {
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) {
        return {};
    }
    return DirOf(exe);
}

}  // namespace airtype
