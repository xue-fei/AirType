#pragma once

// AirType TSF 文本服务。
//
// 职责：被系统注入到目标进程后，作为一个「输入法」存在；
// 从 Rust 核心（airtype_ffi.dll）拿到识别文本，写进当前输入框。
//
// 线程模型：TSF 在目标进程的 UI 线程（STA）上调用 Activate。
// Rust 回调在 tokio worker（MTA）触发，经 HiddenWindow marshal 回 STA。

#include <atlbase.h>
#include <msctf.h>

#include <atomic>
#include <string>
#include <thread>

#include "FfiBridge.h"
#include "HiddenWindow.h"
#include "StatusWindow.h"

namespace airtype {

class TsfTextService : public ITfTextInputProcessorEx, public ITfThreadMgrEventSink,
                       public ITfInputProcessorProfileActivationSink {
public:
    TsfTextService();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfInputProcessorProfileActivationSink：系统活动键盘变化时收到广播。
    // 用它替代 GetActiveProfile 轮询——后者查的是调用线程（我们的 STA 线程
    // 并非焦点线程），恒返回 AirType，导致切到其他输入法后图标不消失。
    STDMETHODIMP OnActivated(DWORD dwProfileType, LANGID langid, REFCLSID clsid,
                             REFGUID catid, REFGUID guidProfile, HKL hkl,
                             DWORD dwFlags) override;

    // ITfTextInputProcessor
    STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override;
    STDMETHODIMP Deactivate() override;

    // ITfTextInputProcessorEx
    STDMETHODIMP ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) override;

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* dim) override;
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* dim) override;
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr* previous) override;
    STDMETHODIMP OnPushContext(ITfContext* ctx) override;
    STDMETHODIMP OnPopContext(ITfContext* ctx) override;

    /// 从回调线程把文本投递到 STA 线程（失败时自行释放）。
    /// 公开给 Rust 回调用的自由函数（userdata 传 this）。
    void PostText(std::wstring text, bool final);

    /// 从 Rust 音频线程把电平投递到 STA 线程。level 为 0~1000 毫电平，
    /// **-1 表示 PTT 结束**（需立即回落填充）。公开理由同 PostText。
    void PostLevel(int level);

private:
    ~TsfTextService();

    /// 隐藏窗口收到文本后调用（STA 线程）。
    void OnPayload(AirTypePayload* payload);
    /// 隐藏窗口收到电平后调用（STA 线程）：平滑后写入状态图标。
    void OnLevel(int level);
    /// 焦点到达时补写之前因「拿不到焦点文档」而缓存的文本。
    void FlushPending();
    /// 把文本写进当前输入框。返回 false 表示本帧没落地（缓存待焦点、
    /// 拿不到 context 或编辑会话失败），日志不能报「已写入」。
    bool WriteText(const std::wstring& text);
    /// 换文档/丢焦点时清空「已写入范围」，避免跨文档误替换。
    void ResetWrittenRange();
    /// 查询插入符屏幕坐标并把状态图标移到其右侧（STA 线程）。
    void UpdateStatusPosition();
    /// 后台轮询：定期 PostMessage 到隐藏窗口，让 STA 线程重新定位图标。
    /// 解决「Activate 时焦点未就绪 / OnSetFocus 不触发」导致图标一直隐藏的问题。
    void StartStatusPolling();
    void StopStatusPolling();
    std::wstring ResolveConfigPath() const;
    bool StartCore();
    void StopCore();

    CComPtr<ITfThreadMgr> thread_mgr_;
    TfClientId client_id_ = 0;
    DWORD sink_cookie_ = 0;
    DWORD profile_sink_cookie_ = 0;  // ITfInputProcessorProfileActivationSink
    bool active_ = false;

    HiddenWindow hidden_;
    StatusWindow status_;
    FfiBridge ffi_;

    /// 核心初始化（加载 sherpa 模型）耗时数百毫秒，放后台线程，
    /// 不能堵在目标进程的 UI 线程上。
    std::thread core_thread_;
    std::atomic<bool> stopping_{false};

    /// 状态图标定位轮询线程。只做 PostMessage，不碰 COM/TSF，线程安全。
    std::thread status_poll_thread_;
    std::atomic<bool> poll_stop_{false};

    /// 焦点未就绪时缓存的文本，等 OnSetFocus 补写（都在 STA 线程，无需加锁）。
    std::wstring pending_text_;
    bool has_pending_ = false;

    /// 音量平滑值（0.0~1.0）。只在 STA 线程读写。
    /// 电平是 20ms 一帧的瞬时 RMS，直接用会让填充高度逐帧跳动；
    /// 做指数平滑后上升快、下降慢，视觉上像真实电平表。
    float level_smoothed_ = 0.0f;

    /// 当前焦点文档（来自事件回调），用于判断「已写入范围」是否还有效。
    CComPtr<ITfDocumentMgr> current_doc_;
    CComPtr<ITfDocumentMgr> written_doc_;
    CComPtr<ITfRange> written_range_;

    LONG ref_ = 1;
};

}  // namespace airtype
