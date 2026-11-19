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

namespace airtype {

class TsfTextService : public ITfTextInputProcessorEx, public ITfThreadMgrEventSink {
public:
    TsfTextService();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

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

private:
    ~TsfTextService();

    /// 隐藏窗口收到文本后调用（STA 线程）。
    void OnPayload(AirTypePayload* payload);
    /// 焦点到达时补写之前因「拿不到焦点文档」而缓存的文本。
    void FlushPending();
    /// 把文本写进当前输入框。返回 false 表示本帧没落地（缓存待焦点、
    /// 拿不到 context 或编辑会话失败），日志不能报「已写入」。
    bool WriteText(const std::wstring& text);
    /// 换文档/丢焦点时清空「已写入范围」，避免跨文档误替换。
    void ResetWrittenRange();
    std::wstring ResolveConfigPath() const;
    bool StartCore();
    void StopCore();

    CComPtr<ITfThreadMgr> thread_mgr_;
    TfClientId client_id_ = 0;
    DWORD sink_cookie_ = 0;
    bool active_ = false;

    HiddenWindow hidden_;
    FfiBridge ffi_;

    /// 核心初始化（加载 sherpa 模型）耗时数百毫秒，放后台线程，
    /// 不能堵在目标进程的 UI 线程上。
    std::thread core_thread_;
    std::atomic<bool> stopping_{false};

    /// 焦点未就绪时缓存的文本，等 OnSetFocus 补写（都在 STA 线程，无需加锁）。
    std::wstring pending_text_;
    bool has_pending_ = false;

    /// 当前焦点文档（来自事件回调），用于判断「已写入范围」是否还有效。
    CComPtr<ITfDocumentMgr> current_doc_;
    CComPtr<ITfDocumentMgr> written_doc_;
    CComPtr<ITfRange> written_range_;

    LONG ref_ = 1;
};

}  // namespace airtype
