#include "FfiBridge.h"

#include <string>
#include <windows.h>

#include "Log.h"
#include "ModuleUtil.h"

namespace airtype {

namespace {

std::wstring DefaultSearchPath() {
    // 外壳是 in-proc 服务器，会被注入到任意宿主进程（记事本、Word…），
    // 宿主 exe 目录下通常没有 airtype_ffi.dll，因此优先按本模块目录找；
    // 探针等独立 exe 把 ffi 放在自己旁边的用法继续兼容。
    const std::wstring dirs[] = {SelfModuleDir(), HostExeDir()};
    for (const std::wstring& dir : dirs) {
        if (dir.empty()) {
            continue;
        }
        const std::wstring candidate = dir + L"airtype_ffi.dll";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    // 两个目录都没有时，让 LoadLibrary 的错误信息指向最可能的位置。
    const std::wstring dir = SelfModuleDir();
    return dir.empty() ? std::wstring(L"airtype_ffi.dll") : dir + L"airtype_ffi.dll";
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
