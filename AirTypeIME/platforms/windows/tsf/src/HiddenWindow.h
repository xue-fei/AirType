#pragma once

// 隐藏消息窗口：把 MTA 线程（Rust tokio worker）的回调 marshal 到 STA 线程。
//
// 回调侧只 PostMessage，不持锁、不调用 TSF；窗口过程在创建它的 STA 线程上执行，
// 那里才能安全地请求 ITfEditSession。

#include <atomic>
#include <functional>
#include <windows.h>

#include "TsfTypes.h"

namespace airtype {

class HiddenWindow {
public:
    using Handler = std::function<void(AirTypePayload*)>;
    using CommandHandler = std::function<void()>;
    /// 音量电平（毫电平 0~1000）在 STA 线程上的回调。
    using LevelHandler = std::function<void(int)>;

    HiddenWindow() = default;
    ~HiddenWindow();

    HiddenWindow(const HiddenWindow&) = delete;
    HiddenWindow& operator=(const HiddenWindow&) = delete;

    /// 创建隐藏窗口。必须在目标进程的 UI（STA）线程上调用。
    /// refresh：后台轮询线程投递 RefreshStatus 时在 STA 线程回调，用于重新定位图标。
    /// level：Rust 音频线程投递 Level 消息时在 STA 线程回调，参数为毫电平 0~1000。
    bool Create(const wchar_t* title, Handler handler, CommandHandler refresh = {},
                LevelHandler level = {});
    void Destroy();

    HWND Handle() const { return hwnd_.load(std::memory_order_relaxed); }
    bool Valid() const { return hwnd_.load(std::memory_order_relaxed) != nullptr; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static HiddenWindow* From(HWND hwnd);

    // 原子：PostText 在 MTA（tokio worker）线程读，Destroy 在 STA 线程写。
    std::atomic<HWND> hwnd_{nullptr};
    Handler handler_;
    CommandHandler refresh_;
    LevelHandler level_;
    wchar_t class_name_[64]{};
};

}  // namespace airtype
