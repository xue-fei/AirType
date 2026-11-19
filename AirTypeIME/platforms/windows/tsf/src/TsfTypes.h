#pragma once

// 外壳内部用的消息与载荷定义。
//
// Rust 核心的回调在 tokio worker（MTA）线程触发，TSF 接口要求 STA。
// 回调只负责把文本打包 PostMessage 到隐藏窗口，由窗口线程请求编辑会话。

#include <windows.h>

#include <string>

namespace airtype {

inline constexpr UINT WM_AIRTYPE_TEXT = WM_APP + 1;      // 最终文本
inline constexpr UINT WM_AIRTYPE_PARTIAL = WM_APP + 2;   // 中间结果

struct AirTypePayload {
    std::wstring text;
    bool final = true;
};

}  // namespace airtype
