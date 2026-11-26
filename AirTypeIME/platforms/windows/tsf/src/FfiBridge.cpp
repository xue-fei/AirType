#include "FfiBridge.h"

#include <string>
#include <windows.h>

#include "Log.h"
#include "ModuleUtil.h"

namespace airtype {

namespace {

// {app}\lib\airtype_ffi.dll —— 所有版本目录共用的那一份核心。
//
// 为什么这么放：ffi 有 20MB，塞进 versions\<ver>\ 会让每个历史版本目录都占
// 20MB；而 TSF 外壳被宿主进程常驻锁定、旧版本删不掉，于是残留目录越堆越大
// （实测 3 个版本 = 60MB）。把 ffi 挪到共享 lib 后，版本目录只剩 80KB 的
// tsf + 90KB 探针，残留几乎不可见。
//
// 外壳在 {app}\versions\<版本>\，所以是「上两级再进 lib」。
std::wstring SharedLibPath() {
    const std::wstring dir = SelfModuleDir();
    if (dir.empty()) {
        return {};
    }
    const std::wstring raw = dir + L"..\\..\\lib\\airtype_ffi.dll";
    wchar_t full[MAX_PATH]{};
    const DWORD n = GetFullPathNameW(raw.c_str(), MAX_PATH, full, nullptr);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    return full;
}

std::wstring DefaultSearchPath() {
    // 查找顺序（前一个不存在才看后一个）：
    //   1. {app}\lib\              共享核心（新布局，优先——避免旧布局残留的同目录
    //                              ffi 抢先被加载，导致"装了新版还在跑旧核心"）
    //   2. 本模块目录              旧布局 / 探针独立运行时 ffi 就在旁边
    //   3. 宿主 exe 目录           最后兜底
    //
    // 外壳是 in-proc 服务器，会被注入到任意宿主进程（记事本、Word…），
    // 宿主 exe 目录下通常没有 airtype_ffi.dll，所以它排最后。
    const std::wstring shared = SharedLibPath();
    const std::wstring local = SelfModuleDir();
    const std::wstring candidates[] = {
        shared,
        local.empty() ? std::wstring() : local + L"airtype_ffi.dll",
    };
    for (const std::wstring& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    const std::wstring host = HostExeDir();
    if (!host.empty()) {
        const std::wstring candidate = host + L"airtype_ffi.dll";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    // 全都找不到时，让 LoadLibrary 的错误信息指向最可能的位置。
    return shared.empty() ? std::wstring(L"airtype_ffi.dll") : shared;
}

}  // namespace

FfiBridge::~FfiBridge() {
    // 不 FreeLibrary：DLL 里的 tokio runtime 退出时可能还在用模块。
    // 与外壳同生命周期，进程退出时由系统回收。
}

void* FfiBridge::Proc(const char* name) const {
    if (dll_ == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(GetProcAddress(dll_, name));
}

bool FfiBridge::Load(const std::wstring& path) {
    const std::wstring target = path.empty() ? DefaultSearchPath() : path;
    dll_ = LoadLibraryW(target.c_str());
    if (dll_ == nullptr) {
        last_error_ = "LoadLibraryW 失败: " + std::to_string(GetLastError());
        return false;
    }

    version_ = reinterpret_cast<FnVersion>(Proc("airtype_version"));
    init_ = reinterpret_cast<FnInit>(Proc("airtype_init"));
    set_text_ = reinterpret_cast<FnSetText>(Proc("airtype_set_text_callback"));
    set_partial_ = reinterpret_cast<FnSetPartial>(Proc("airtype_set_partial_callback"));
    // 可选导出：旧版 ffi 没有它，缺失时只为空指针，不影响其它功能。
    set_level_ = reinterpret_cast<FnSetLevel>(Proc("airtype_set_level_callback"));
    start_ = reinterpret_cast<FnStart>(Proc("airtype_start"));
    stop_ = reinterpret_cast<FnStop>(Proc("airtype_stop"));
    last_error_proc_ = reinterpret_cast<FnLastError>(Proc("airtype_last_error"));

    if (version_ == nullptr || init_ == nullptr || set_text_ == nullptr ||
        set_partial_ == nullptr || start_ == nullptr || stop_ == nullptr) {
        last_error_ = "导出表不完整（airtype_ffi.dll 版本过旧？）";
        return false;
    }
    last_error_.clear();
    return true;
}

const char* FfiBridge::Version() const {
    return version_ ? version_() : "";
}

int32_t FfiBridge::Init(const std::string& config_path_utf8) {
    if (!init_) {
        return AIRTYPE_ERR_INTERNAL;
    }
    return init_(config_path_utf8.empty() ? nullptr : config_path_utf8.c_str());
}

int32_t FfiBridge::SetTextCallback(airtype_text_cb cb, void* userdata) {
    if (!set_text_) {
        return AIRTYPE_ERR_INTERNAL;
    }
    return set_text_(cb, userdata);
}

int32_t FfiBridge::SetPartialCallback(airtype_partial_cb cb, void* userdata) {
    if (!set_partial_) {
        return AIRTYPE_ERR_INTERNAL;
    }
    return set_partial_(cb, userdata);
}

int32_t FfiBridge::SetLevelCallback(airtype_level_cb cb, void* userdata) {
    if (!set_level_) {
        return AIRTYPE_ERR_STATE;  // 旧版 ffi 没有该导出
    }
    return set_level_(cb, userdata);
}

int32_t FfiBridge::Start() {
    if (!start_) {
        return AIRTYPE_ERR_INTERNAL;
    }
    return start_();
}

void FfiBridge::Stop() {
    if (stop_) {
        stop_();
    }
}

}  // namespace airtype
