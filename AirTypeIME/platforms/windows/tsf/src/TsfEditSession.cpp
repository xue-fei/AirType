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
            // 上次写入的 range 会被文档自身的改动（用户手打、应用重排、
            // 换焦点后重建）作废。此时不能直接失败——那会让后续每一帧
            // 都替换同一段死 range，整段文字再也不更新。回退到当前选区。
            airtype::log_hr("Clone(上次写入 range)，回退到当前选区", cloned);
            previous_.Release();
            target.Release();
        }
    }
    if (!target) {
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
        // GetSelection 返回的 sel.range 调用方必须 Release；Clone 后即可释放。
        // 用 CComPtr 接管，提前 return 或抛错时也能自动释放。
        CComPtr<ITfRange> selected;
        selected.Attach(sel.range);
        hr = selected->Clone(&target);
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
    // 注意：缓存的 written_ 不能折叠（下次还要靠它定位），折叠克隆出来的 caret。
    CComPtr<ITfRange> caret;
    if (SUCCEEDED(written_->Clone(&caret)) &&
        SUCCEEDED(caret->Collapse(ec, TF_ANCHOR_END))) {
        TF_SELECTION sel{};
        sel.range = caret;
        sel.style.ase = TF_AE_NONE;
        sel.style.fInterimChar = FALSE;
        ctx_->SetSelection(ec, 1, &sel);
    }
    return S_OK;
}

}  // namespace airtype
