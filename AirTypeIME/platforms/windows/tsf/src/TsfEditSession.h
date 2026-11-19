#pragma once

// 写入用编辑会话：整体替换。
//
// 第一次写入落在当前选区；之后每次替换「上一次写入的那段 range」，
// 这样 partial -> final 的累积文本可以原地刷新，不会越写越长。

#include <atlbase.h>
#include <msctf.h>

#include <string>

namespace airtype {

class ReplaceEditSession : public ITfEditSession {
public:
    ReplaceEditSession(ITfContext* ctx, std::wstring text, ITfRange* previous);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfEditSession
    STDMETHODIMP DoEditSession(TfEditCookie ec) override;

    /// 本次写入覆盖的 range，供下次替换复用。
    ITfRange* written_range() const { return written_.p; }

private:
    ~ReplaceEditSession() = default;

    CComPtr<ITfContext> ctx_;
    std::wstring text_;
    CComPtr<ITfRange> previous_;
    CComPtr<ITfRange> written_;
    ULONG ref_ = 1;
};

}  // namespace airtype
