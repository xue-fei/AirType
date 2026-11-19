#include "TsfEditSession.h"

#include "Log.h"

namespace airtype {

ReplaceEditSession::ReplaceEditSession(ITfContext* ctx, std::wstring text, ITfRange* previous)
    : ctx_(ctx), text_(std::move(text)), previous_(previous) {}

STDMETHODIMP ReplaceEditSession::QueryInterface(REFIID riid, void** ppv) {
    if (ppv == nullptr) {
        return E_INVALIDARG;
    }
    if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
        *ppv = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) ReplaceEditSession::AddRef() {
    return ++ref_;
}

STDMETHODIMP_(ULONG) ReplaceEditSession::Release() {
    const ULONG r = --ref_;
    if (r == 0) {
        delete this;
    }
    return r;
}

STDMETHODIMP ReplaceEditSession::DoEditSession(TfEditCookie ec) {
    CComPtr<ITfRange> target;
    if (previous_) {
        const HRESULT cloned = previous_->Clone(&target);
        if (FAILED(cloned)) {
            airtype::log_hr("Clone(上次写入 range)", cloned);
            return cloned;
        }
    } else {
        TF_SELECTION sel{};
        ULONG fetched = 0;
        HRESULT hr = ctx_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched);
        if (FAILED(hr)) {
            airtype::log_hr("GetSelection", hr);
            return hr;
        }
        if (fetched == 0) {
            // 没有可写选区（比如焦点在按钮上），本次放弃，下次重试。
            return TF_E_NOSELECTION;
        }
        hr = sel.range->Clone(&target);
        if (FAILED(hr)) {
            return hr;
        }
    }

    const HRESULT written =
        target->SetText(ec, 0, text_.c_str(), static_cast<LONG>(text_.size()));
    if (FAILED(written)) {
        airtype::log_hr("ITfRange::SetText", written);
        return written;
    }

    // 记住这段 range，下次 partial/final 原地替换它。
    written_ = target;

    // 光标停在新文本末尾，用户接着打字时不会插到中间。
    CComPtr<ITfRange> caret;
    if (SUCCEEDED(written_->Clone(&caret))) {
        caret->Collapse(ec, TF_ANCHOR_END);
    }
    return S_OK;
}

}  // namespace airtype
