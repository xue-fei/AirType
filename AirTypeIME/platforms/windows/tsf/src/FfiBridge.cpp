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

// {app}\lib\ 目录（含尾部分隔符）。空 = 解析失败。
std::wstring SharedLibDir() {
    const std::wstring full = SharedLibPath();
    if (full.empty()) {
        return {};
    }
    const size_t pos = full.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : full.substr(0, pos + 1);
}

// 热重载用的版本戳文件：{app}\lib\ffi-current.txt，一行构建戳。
std::wstring StampFilePath() { return SharedLibDir() + L"ffi-current.txt"; }

// 读版本戳（滤掉空白与控制字符）。读不到返回空 —— 空表示"固定名模式"，
// 即老安装布局，此时不参与热重载比对。
std::wstring ReadStampFile() {
    const std::wstring path = StampFilePath();
    if (path.empty() || path == L"ffi-current.txt") {
        return {};
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }
    char buf[128]{};
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok || got == 0) {
        return {};
    }
    std::wstring out;
    for (DWORD i = 0; i < got; ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c <= 0x20 || c > 0x7E) {
            continue;  // 空白、换行、非 ASCII 一律丢弃
        }
        out.push_back(static_cast<wchar_t>(c));
    }
    return out;
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

    // 0. {app}\lib\airtype_ffi_<戳>.dll —— 热重载布局。戳由安装器写进
    //    ffi-current.txt，文件名带戳才能做到"新文件与旧文件并存、进程内仍持有
    //    旧映像"，从而在切换输入法时安全换用新版本。
    const std::wstring stamp = ReadStampFile();
    const std::wstring libdir = SharedLibDir();
    if (!stamp.empty() && !libdir.empty()) {
        const std::wstring versioned = libdir + L"airtype_ffi_" + stamp + L".dll";
        if (GetFileAttributesW(versioned.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return versioned;
        }
    }

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
    // 析构不 FreeLibrary：此时进程正在退出（或服务对象被销毁），
    // 系统回收更安全。需要换版本时走显式的 Unload()。
}

std::wstring FfiBridge::DiskStamp() { return ReadStampFile(); }

void FfiBridge::Unload() {
    if (dll_ == nullptr) {
        return;
    }
    // 再停一次：airtype_stop 幂等，多停一次只为确保没有残留会话。
    if (stop_ != nullptr) {
        stop_();
    }
    FreeLibrary(dll_);
    dll_ = nullptr;
    version_ = nullptr;
    init_ = nullptr;
    set_text_ = nullptr;
    set_partial_ = nullptr;
    set_level_ = nullptr;
    start_ = nullptr;
    stop_ = nullptr;
    last_error_proc_ = nullptr;
    loaded_stamp_.clear();
}

void* FfiBridge::Proc(const char* name) const {
    if (dll_ == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(GetProcAddress(dll_, name));
}

bool FfiBridge::Load(const std::wstring& path) {
    const std::wstring target = path.empty() ? DefaultSearchPath() : path;
    // 只在走默认搜索时记录戳：显式指定路径的场景（探针）不参与热重载。
    loaded_stamp_ = path.empty() ? ReadStampFile() : std::wstring();
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
