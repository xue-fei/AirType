# Windows TSF 外壳接入说明

> 第一版**不实现**这个外壳。这里只固化契约与步骤，等端到端原型跑通后再开。
> 原型用 `inject.mode = "clipboard_paste"`（剪贴板 + Ctrl+V）验证链路，正式输入法应改用本文件描述的 TSF 编辑会话。

## 为什么需要它

`airtype-daemon` 是一个独立进程，能收音频、能识别，但它**不是输入法**：操作系统不会让它参与其他应用的输入上下文。要成为输入法，必须提供一个注册到 TSF 的 in-proc COM DLL，由系统按需注入到目标进程。

## 职责边界

| 层 | 负责 | 不负责 |
|---|---|---|
| `airtype-core` + `airtype-daemon` | WebSocket 收流、PCM 重组、ASR、文本后处理 | 按键拦截、编辑会话、DLL 注册 |
| TSF 外壳（C++ / COM） | 注册为文本服务、管理 `ITfEditSession`、把回调文本写入输入框 | 任何识别逻辑 |

## C ABI 契约

头文件：[`include/airtype_ffi.h`](include/airtype_ffi.h)
产物：`cargo build -p airtype-ffi --release` → `target/release/airtype_ffi.dll`

```c
#include "airtype_ffi.h"

static void CALLTYPE on_text(const char* utf8, void* userdata) {
    // 注意：回调在 Rust 侧的 tokio worker 线程触发，不在 UI 线程。
    // 必须 marshal 回编辑会话线程，再调用 ITfInsertAtSelection::InsertTextAtSelection。
    // 不要在回调里做耗时操作或阻塞。
}

// DLL_PROCESS_ATTACH 或 ITfTextInputProcessor::Activate 中：
airtype_init("C:/path/to/config/airtype.toml");
airtype_set_text_callback(on_text, nullptr);
airtype_start();

// 反激活 / DLL_PROCESS_DETACH：
airtype_stop();
```

`airtype_start()` 会**强制把内置注入器设为 `none`**，最终文本只通过 `on_text` 交给外壳，
不会出现「剪贴板粘贴 + TSF 写入」双重输入。

错误码：`AIRTYPE_OK(0)` / `ERR_NULL(-1)` / `ERR_CONFIG(-2)` / `ERR_STATE(-3)` /
`ERR_UTF8(-4)` / `ERR_INTERNAL(-5)`。失败时用 `airtype_last_error()` 取 UTF-8 描述。

## 实现步骤

1. **建 COM DLL 工程**：CMake + MSVC，启用 C++17，链接 `ole32.lib` / `uuid.lib` / `oleaut32.lib`。
2. **实现 TSF 最小接口**：
   - `ITfTextInputProcessorEx::Activate` / `Deactivate`
   - `ITfThreadMgrEventSink`（可选，用于跟踪焦点）
   - `ITfKeyEventSink`（可选，用于把 PTT 快捷键映射到开始/结束）
   - `ITfEditSession::DoEditSession` → `ITfInsertAtSelection::InsertTextAtSelection` 写 UTF-16 文本
3. **注册文本服务**：`ITfInputProcessorProfiles::Register` + `ITfCategoryMgr::RegisterCategory`，
   CLSID / GUID 需自行生成并写进 `.rgs` 或直接调 API；`regsvr32` 注册 DLL 需要管理员权限。
4. **加载 Rust 核心**：`LoadLibraryW(L"airtype_ffi.dll")` + `GetProcAddress` 取 6 个导出函数，
   或在链接期直接链 `airtype_ffi.dll.lib`（需为 cdylib 生成导入库）。
5. **线程模型**：`on_text` 在工作线程触发 → `PostMessage` 到外壳的隐藏窗口 →
   在 UI/STA 线程里请求 `ITfDocumentMgr` 的编辑会话写入。

## 调试注意事项

- TSF DLL 被注入到**别的进程**，调试器要附加到目标进程（如 notepad.exe）才能下断点。
- 32 位进程需要 32 位 DLL；Rust 侧需另装 `i686-pc-windows-msvc` target 编译对应版本。
- 卸载输入法前先 `airtype_stop()`，否则 tokio runtime 随 DLL 卸载可能导致挂起。
- 建议先用 `inject.mode = "none"` + 日志确认回调文本正确，再打开 TSF 写入。

## 何时点亮这一层

满足以下三条再开工，避免过早啃 COM：

1. `python tools/mock_airmic.py` 能稳定跑通 START/AUDIO/END 并拿到正确文本；
2. 真实 ASR（`command` 引擎）延迟稳定在 0.3～1 秒目标内；
3. 已确定输入注入在目标应用（编辑器 / 聊天框 / 浏览器）里用剪贴板方案有兼容问题。
