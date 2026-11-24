#pragma once

// 外壳内部用的消息与载荷定义。
//
// Rust 核心的回调在 tokio worker（MTA）线程触发，TSF 接口要求 STA。
// 回调只负责把文本打包 PostMessage 到隐藏窗口，由窗口线程请求编辑会话。

#include <windows.h>

#include <string>

namespace airtype {

// 必须用 RegisterWindowMessageW 而不是 WM_APP + n：WM_APP..0xBFFF 名义上是
// 进程内私有范围，但隐藏消息窗口仍可能收到广播（PostMessageW(NULL, ...)），
// 其他进程若用同样的 WM_APP+n 编码自己的指针，本进程会把它当成 AirTypePayload
// 读走 → 跨进程类型混淆，轻则泄漏重则崩。注册消息保证全系统唯一，
// 同进程内多次调用拿到同一个 UINT。
struct AirTypeMessages {
    UINT text = 0;
    UINT partial = 0;
    // 卸载器向 HWND_BROADCAST 发送，让所有已加载本 DLL 的进程自毁悬浮图标。
    // TSF 注入到每个进程后，卸载只删注册表/文件，已加载的进程收不到 Deactivate，
    // 图标窗口会变成孤儿留在屏幕上；广播是唯一能跨进程通知到它们的手段。
    UINT shutdown = 0;
    // 后台轮询线程投递到隐藏窗口，让 STA 线程重新定位状态图标。
    // 用注册消息而非 WM_APP+n，理由同 text/partial（防跨进程指针混淆）。
    UINT refreshStatus = 0;

    static const AirTypeMessages& Get() {
        static AirTypeMessages instance = [] {
            AirTypeMessages m;
            m.text = RegisterWindowMessageW(L"AirTypeTsf.Text");
            m.partial = RegisterWindowMessageW(L"AirTypeTsf.Partial");
            m.shutdown = RegisterWindowMessageW(L"AirTypeTsf.Shutdown");
            m.refreshStatus = RegisterWindowMessageW(L"AirTypeTsf.RefreshStatus");
            return m;
        }();
        return instance;
    }
};

struct AirTypePayload {
    std::wstring text;
    bool final = true;
};

}  // namespace airtype
