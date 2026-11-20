#include "TsfTextService.h"

#include <utility>

#include "Log.h"
#include "ModuleUtil.h"
#include "TsfEditSession.h"
#include "TsfString.h"

namespace airtype {

namespace {

// Rust 侧的回调是裸函数指针，userdata 传 this。
void CALLBACK OnFinalText(const char* utf8, void* userdata) {
    auto* self = static_cast<TsfTextService*>(userdata);
    if (self != nullptr && utf8 != nullptr) {
        self->PostText(Utf8ToWide(utf8), true);
    }
}

void CALLBACK OnPartialText(const char* text, int32_t /*delete_chars*/, const char* /*append*/,
                            void* userdata) {
    auto* self = static_cast<TsfTextService*>(userdata);
    if (self != nullptr && text != nullptr) {
        // TSF 侧统一走「整体替换」：只取累积文本。
        self->PostText(Utf8ToWide(text), false);
    }
}

}  // namespace

TsfTextService::TsfTextService() = default;

TsfTextService::~TsfTextService() {
    // 正常情况下 Deactivate 已经 StopCore；这里是兜底。
    // 必须 join：后台 StartCore 持有 this 指针，不回收就 destructor 等于 UAF，
    // 且 std::thread 析构遇到 joinable 线程会 terminate。
    StopCore();
    hidden_.Destroy();
}

STDMETHODIMP TsfTextService::QueryInterface(REFIID riid, void** ppv) {
    if (ppv == nullptr) {
        return E_INVALIDARG;
    }
    if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessor ||
        riid == IID_ITfTextInputProcessorEx) {
        *ppv = static_cast<ITfTextInputProcessor*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_ITfThreadMgrEventSink) {
        *ppv = static_cast<ITfThreadMgrEventSink*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) TsfTextService::AddRef() {
    return ++ref_;
}

STDMETHODIMP_(ULONG) TsfTextService::Release() {
    const LONG r = --ref_;
    if (r == 0) {
        delete this;
    }
    return r;
}

STDMETHODIMP TsfTextService::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) {
    // 先按普通 Activate 走；dwFlags 目前没有需要区分的语义。
    if (dwFlags != 0) {
        airtype::log("ActivateEx flags=0x%08X（忽略）", static_cast<unsigned>(dwFlags));
    }
    return Activate(ptim, tid);
}

STDMETHODIMP TsfTextService::Activate(ITfThreadMgr* ptim, TfClientId tid) {
    if (active_) {
        return S_OK;
    }
    if (ptim == nullptr) {
        return E_INVALIDARG;
    }
    thread_mgr_ = ptim;
    client_id_ = tid;

    if (!hidden_.Create(L"AirType TSF 消息窗口", [this](AirTypePayload* p) {
            this->OnPayload(p);
        })) {
        return E_FAIL;
    }

    // 跟踪焦点文档，用来判断已写入范围是否还有效。
    // AdviseSink 挂在 ITfSource 上（ITfThreadMgr 本身不直接暴露）。
    CComPtr<ITfSource> source;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source)))) {
        const HRESULT advised = source->AdviseSink(
            IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this), &sink_cookie_);
        if (FAILED(advised)) {
            airtype::log_hr("AdviseSink(ITfThreadMgrEventSink)", advised);
            // 拿不到焦点事件也能用：写入时回退到 GetFocus。
        }
    } else {
        airtype::log("ITfThreadMgr 不支持 ITfSource，焦点事件不可用");
    }

    // 核心初始化（加载 sherpa 模型 + 起 tokio runtime）可能耗时数百毫秒，
    // 堵在目标进程 UI 线程上会让输入法激活时整机卡一下，放后台线程。
    stopping_ = false;
    core_thread_ = std::thread([this]() { StartCore(); });

    active_ = true;
    airtype::log("AirType 文本服务已激活 client_id=%lu", static_cast<unsigned long>(client_id_));
    return S_OK;
}

STDMETHODIMP TsfTextService::Deactivate() {
    if (!active_) {
        return S_OK;
    }
    StopCore();

    if (thread_mgr_ && sink_cookie_ != 0) {
        CComPtr<ITfSource> source;
        if (SUCCEEDED(
                thread_mgr_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source)))) {
            source->UnadviseSink(sink_cookie_);
        }
        sink_cookie_ = 0;
    }
    hidden_.Destroy();
    thread_mgr_.Release();
    client_id_ = 0;
    active_ = false;
    ResetWrittenRange();
    current_doc_.Release();
    airtype::log("AirType 文本服务已停用");
    return S_OK;
}

bool TsfTextService::StartCore() {
    if (stopping_) {
        return false;
    }
    if (!ffi_.Load()) {
        airtype::log("airtype_ffi.dll 加载失败: %s", ffi_.LastError().c_str());
        return false;
    }

    const std::wstring config = ResolveConfigPath();
    if (stopping_) {
        return false;
    }
    const int32_t rc = ffi_.Init(config.empty() ? std::string() : WideToUtf8(config));
    if (rc != AIRTYPE_OK) {
        airtype::log("airtype_init 失败 code=%d: %s", rc, ffi_.ReadLastError());
        return false;
    }
    if (stopping_) {
        // Activate 之后立刻 Deactivate：核心刚初始化好就收工，别再起服务。
        ffi_.Stop();
        return false;
    }

    ffi_.SetTextCallback(OnFinalText, this);
    ffi_.SetPartialCallback(OnPartialText, this);

    const int32_t started = ffi_.Start();
    if (started != AIRTYPE_OK) {
        airtype::log("airtype_start 失败 code=%d: %s", started, ffi_.ReadLastError());
        return false;
    }
    airtype::log("AirType 核心已启动 version=%s config=%s", ffi_.Version(),
                 config.empty() ? "(内置默认)" : "已指定");
    return true;
}

void TsfTextService::StopCore() {
    stopping_ = true;
    // 先 join 后台线程，再调 ffi_.Stop()。
    // StartCore 在后台线程独占 FfiBridge 的 Load/Init/SetCallback/Start 写入，
    // 若 STA 这边并发调 Stop()（读 stop_ 等函数指针），Activate 后立刻 Deactivate
    // 时会数据竞争（UB）。join 建立 happens-before：后台所有写入完成后 STA 才读。
    // airtype_stop 幂等，后台路径已停过时再停一次无害。
    if (core_thread_.joinable()) {
        core_thread_.join();
    }
    // 此时 airtype_stop 会阻塞等在途回调结束才返回，保证随后 delete this 安全。
    ffi_.Stop();
}

std::wstring TsfTextService::ResolveConfigPath() const {
    // 配置文件优先找本模块同目录的 config\airtype.toml，其次同目录 airtype.toml。
    // 宿主 exe 目录作为兜底：独立探针把配置放在自己旁边时仍然可用。
    const std::wstring dirs[] = {SelfModuleDir(), HostExeDir()};
    const std::wstring names[] = {L"config\\airtype.toml", L"airtype.toml"};
    for (const std::wstring& dir : dirs) {
        if (dir.empty()) {
            continue;
        }
        for (const std::wstring& name : names) {
            const std::wstring path = dir + name;
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return path;
            }
        }
    }
    return {};
}

// --- ITfThreadMgrEventSink ---

STDMETHODIMP TsfTextService::OnInitDocumentMgr(ITfDocumentMgr* /*dim*/) {
    return S_OK;
}

STDMETHODIMP TsfTextService::OnUninitDocumentMgr(ITfDocumentMgr* /*dim*/) {
    return S_OK;
}

STDMETHODIMP TsfTextService::OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr* /*previous*/) {
    current_doc_ = focus;
    // 只有真正换了文档才丢弃写入范围。
    // OnSetFocus 会在一次语音段内被反复触发（点击输入框、TSF 重建隐式文档、
    // 某些应用的光标重排），如果每次都 ResetWrittenRange()，下一帧就会退化成
    // 「往 caret 处插入」，于是同一段文字越写越长而不是原地刷新。
    if (written_range_ && focus != written_doc_.p) {
        airtype::log("焦点文档变化，丢弃上次写入范围");
        ResetWrittenRange();
    }
    FlushPending();
    return S_OK;
}

STDMETHODIMP TsfTextService::OnPushContext(ITfContext* /*ctx*/) {
    return S_OK;
}

STDMETHODIMP TsfTextService::OnPopContext(ITfContext* /*ctx*/) {
    return S_OK;
}

// --- 文本写入 ---

void TsfTextService::PostText(std::wstring text, bool final) {
    // 句柄只读一次。分成 Valid() + Handle() 两次读会形成 TOCTOU：
    // 中间 Destroy() 把 hwnd_ 置空时，PostMessageW(nullptr, ...) 是广播语义，
    // 会把 payload 指针发给全系统的顶层窗口。
    // 这里拿不到句柄只是丢本帧（未激活时每帧都会走到，所以不打日志）。
    const HWND hwnd = hidden_.Handle();
    if (hwnd == nullptr || text.empty()) {
        return;
    }
    const UINT msg = final ? AirTypeMessages::Get().text : AirTypeMessages::Get().partial;
    auto* payload = new AirTypePayload{std::move(text), final};
    if (PostMessageW(hwnd, msg, 0, reinterpret_cast<LPARAM>(payload)) == 0) {
        airtype::log("PostMessage 失败，丢弃本帧文本");
        delete payload;
    }
}

void TsfTextService::OnPayload(AirTypePayload* payload) {
    if (payload == nullptr) {
        return;
    }
    const std::wstring text = std::move(payload->text);
    const bool final = payload->final;
    delete payload;

    if (text.empty()) {
        return;
    }
    const bool written = WriteText(text);
    airtype::log("%s%s %d 字符", final ? "[最终] " : "[中间] ",
                 written ? "已写入" : "未写入（已缓存，等焦点）",
                 static_cast<int>(text.size()));
}

void TsfTextService::ResetWrittenRange() {
    written_doc_.Release();
    written_range_.Release();
}

void TsfTextService::FlushPending() {
    if (!has_pending_) {
        return;
    }
    const std::wstring text = std::move(pending_text_);
    has_pending_ = false;
    pending_text_.clear();
    if (text.empty()) {
        return;
    }
    airtype::log("焦点到达，补写缓存的 %d 字符", static_cast<int>(text.size()));
    WriteText(text);
}

bool TsfTextService::WriteText(const std::wstring& text) {
    if (!thread_mgr_) {
        return false;
    }

    CComPtr<ITfDocumentMgr> doc = current_doc_;
    if (!doc) {
        if (FAILED(thread_mgr_->GetFocus(&doc)) || !doc) {
            // 目标还没拿到 TSF 焦点（例如刚点进输入框、或焦点在另一个
            // 线程的隐式文档上）。缓存本帧，等 OnSetFocus 到达再补写，
            // 比直接丢弃整段文字安全。
            pending_text_ = text;
            has_pending_ = true;
            airtype::log("拿不到焦点文档，缓存本帧文本（%d 字符），等焦点到达",
                         static_cast<int>(text.size()));
            return false;
        }
    }

    CComPtr<ITfContext> ctx;
    if (FAILED(doc->GetTop(&ctx)) || !ctx) {
        return false;
    }

    // 换了文档就放弃上一段写入范围，否则会替换到别的应用里。
    if (written_range_ && written_doc_.p != doc.p) {
        ResetWrittenRange();
    }

    auto* session = new ReplaceEditSession(ctx, text, written_range_);
    HRESULT session_hr = E_FAIL;
    const HRESULT hr = ctx->RequestEditSession(client_id_, session,
                                               TF_ES_SYNC | TF_ES_READWRITE, &session_hr);
    if (SUCCEEDED(hr) && SUCCEEDED(session_hr)) {
        written_doc_ = doc;
        ITfRange* range = session->written_range();
        if (range != nullptr) {
            range->AddRef();
            written_range_.Attach(range);
        }
        session->Release();
        return true;
    }

    airtype::log_hr("RequestEditSession", hr);
    airtype::log_hr("  DoEditSession", session_hr);
    // 这次写入没落地，缓存的 range 已不可信（可能正是它导致失败），
    // 下帧改用当前选区，避免连续失败。
    ResetWrittenRange();
    session->Release();
    return false;
}

}  // namespace airtype
