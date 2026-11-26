#pragma once

// airtype_ffi.dll 的 C++ 包装。
//
// 职责单一：加载 DLL、取导出函数、把 C ABI 包成成员函数。
// 不含任何 TSF 逻辑，也不做线程 marshal——回调线程模型由调用方处理。

#include <cstdint>
#include <string>
#include <windows.h>

#include "airtype_ffi.h"

namespace airtype {

// 导出函数的类型。不用头文件里的函数声明取地址（那是 dllimport 桩），
// 而是按 ABI 重新声明，再和 GetProcAddress 的返回值互转。
using FnVersion = const char* (*)();
using FnInit = int32_t (*)(const char*);
using FnSetText = int32_t (*)(airtype_text_cb, void*);
using FnSetPartial = int32_t (*)(airtype_partial_cb, void*);
using FnSetLevel = int32_t (*)(airtype_level_cb, void*);
using FnStart = int32_t (*)();
using FnStop = void (*)();
using FnLastError = const char* (*)();

class FfiBridge {
public:
    FfiBridge() = default;
    ~FfiBridge();

    FfiBridge(const FfiBridge&) = delete;
    FfiBridge& operator=(const FfiBridge&) = delete;

    /// 加载 DLL。path 为空时按默认名字 "airtype_ffi.dll" 搜索
    /// （先在 exe 同目录，再走系统 DLL 搜索路径）。
    bool Load(const std::wstring& path = {});

    bool Ready() const { return dll_ != nullptr; }

    /// 卸载 DLL 并清空所有函数指针。
    ///
    /// **调用前必须确保 airtype_stop() 已返回**：stop 现在会同步等 tokio worker
    /// 退出（shutdown_timeout），否则 FreeLibrary 之后还有线程在跑本模块代码，
    /// 会踩到已解除映射的代码页直接崩进程。
    void Unload();

    /// 加载时磁盘上的版本戳（{app}\lib\ffi-current.txt）。空 = 老布局，不参与热重载。
    const std::wstring& LoadedStamp() const { return loaded_stamp_; }

    /// 读磁盘**当前**的版本戳。与 LoadedStamp() 不同就说明装了新版本。
    static std::wstring DiskStamp();

    /// 最近一次失败的描述（来自 DLL 的 airtype_last_error 或本地图加载失败）。
    std::string LastError() const { return last_error_; }

    /// 读 DLL 内部的 airtype_last_error()（调用方需自行保证字符串有效期）。
    const char* ReadLastError() const {
        return last_error_proc_ ? last_error_proc_() : "";
    }

    const char* Version() const;
    int32_t Init(const std::string& config_path_utf8);
    int32_t SetTextCallback(airtype_text_cb cb, void* userdata);
    int32_t SetPartialCallback(airtype_partial_cb cb, void* userdata);
    /// 注册音频电平回调。旧版 ffi 没有这个导出时返回 AIRTYPE_ERR_STATE，
    /// 调用方忽略即可（音量可视化不可用，其余功能不受影响）。
    int32_t SetLevelCallback(airtype_level_cb cb, void* userdata);
    int32_t Start();
    void Stop();

private:
    void* Proc(const char* name) const;

    HMODULE dll_ = nullptr;
    std::string last_error_;
    std::wstring loaded_stamp_;

    FnVersion version_ = nullptr;
    FnInit init_ = nullptr;
    FnSetText set_text_ = nullptr;
    FnSetPartial set_partial_ = nullptr;
    FnSetLevel set_level_ = nullptr;
    FnStart start_ = nullptr;
    FnStop stop_ = nullptr;
    FnLastError last_error_proc_ = nullptr;
};

}  // namespace airtype
