#include "TsfTextService.h"

#include <utility>

#include "Log.h"
#include "ModuleUtil.h"
#include "TsfEditSession.h"
#include "TsfGuids.h"
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

// 本进程是否是当前前台进程。
//
// TSF 的激活状态按线程保留：别的程序里曾经用过 AirType 的线程，即使用户
// 已把前台切到其它输入法，也可能残留一个激活实例，图标就会一直挂着。
// 只依赖本进程收到 Deactivate 收不干净，所以用前台进程做统一裁决。
bool IsCurrentProcessForeground() {
    HWND fg = GetForegroundWindow();
    if (fg == nullptr) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid != 0 && pid == GetCurrentProcessId();
}

// 查询「当前活动键盘输入法」是否仍是 AirType，带进程级结果缓存。
//
// 为什么需要它：TSF 的激活状态是**按线程**维护的，切到其他输入法时，
// 本进程这个 TIP 实例不一定会收到 Deactivate（典型：后台进程/资源管理器里
// 残留的实例），图标就会一直挂在屏幕上。用官方接口做会话级兜底判断。
// 查询失败时返回 true（保持现状），宁可偶发多显示也不误隐藏。
//
// 为什么缓存：这是**高频路径**（跟随轮询每 40ms 一次）。原实现每次都新建
// ITfInputProcessorProfileMgr COM 对象再查一次，实测是持续的开销源。
// 这里缓存 COM 实例与最近一次结果，kActiveProbeIntervalMs 内直接复用；
// 只有缓存过期才真正查询。切换输入法本来就有约 250ms 的系统级延迟，
// 这个粒度对「切走就收」完全够用。
constexpr DWORD kActiveProbeIntervalMs = 250;

bool IsAirTypeActiveKeyboard() {
    static CComPtr<ITfInputProcessorProfileMgr> mgr;
    static bool have_cached = false;
    static bool cached_active = true;
    static DWORD last_tick = 0;

    const DWORD now = GetTickCount();
    const bool expired =
        !have_cached || (now - last_tick) >= kActiveProbeIntervalMs || now < last_tick;
    if (!expired) {
        return cached_active;
    }

    // 只创建一次；后续沿用同一实例（COM 对象本身线程亲和，但本函数只在
    // STA 线程调用，语义安全）。
    if (!mgr) {
        const HRESULT ci = CoCreateInstance(
            CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(&mgr));
        if (FAILED(ci) || !mgr) {
            return true;  // 拿不到管理器：放行，不要因为查不了就把图标藏掉
        }
    }

    bool active = true;
    TF_INPUTPROCESSORPROFILE profile{};
    const HRESULT hr = mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile);
    if (SUCCEEDED(hr)) {
        active = profile.clsid == CLSID_AIRTYPE_TSF && profile.guidProfile == GUID_AIRTYPE_PROFILE;
    } else {
        // 查询失败：不清缓存、不改结果，下一次再试（避免抖动）。
        return cached_active;
    }

    cached_active = active;
    have_cached = true;
    last_tick = now;
    return active;
}

// 只读查询会话：拿当前选区（折叠后即插入符）的屏幕矩形，定位状态图标。
// GetTextExt 必须在 edit session 内调用；某些应用不给真实坐标时回退 Win32 caret。
//
// 为什么是异步会话（TF_ES_ASYNC）：原实现用 TF_ES_SYNC，会在**目标进程的 UI
// 线程上原地等待** TSF 内部发编辑凭证。应用忙、上下文被锁、或正在滚动重排时，
// 这一等就是几十到几百毫秒——表现为悬浮图标"卡住不动、然后猛地跳一下"。
// 改异步后 DoEditSession 仍在请求线程（STA）执行，能直接更新分层窗口，但不再
// 阻塞调用方。配合 StatusWindow::generation 丢弃过期结果，避免旧坐标晚到把图标拖回去。
class CaretQuerySession : public ITfEditSession {
public:
    CaretQuerySession(ITfContext* ctx, TfClientId tid, StatusWindow* status, LONG generation)
        : ctx_(ctx), tid_(tid), status_(status), generation_(generation) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    // 所有方法只在 STA 线程执行：RequestEditSession 由 STA 发起，
    // 异步回调也由 TSF 投递回同一 STA 消息循环，不存在跨线程访问 status_。
    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        // 代次已过期：这帧是排队期间被更新的旧请求，丢弃，别把图标拖回旧位置。
        if (generation_ != status_->generation.load(std::memory_order_acquire)) {
            return S_OK;
        }

        RECT rc{};
        bool have_rect = false;
        CComPtr<ITfRange> selected;  // 复用取到的选区，供多次回退

        // ① 首选 TSF GetTextExt：把选区折叠到末端当插入符，取屏幕矩形。
        CComPtr<ITfContextView> view;
        const HRESULT view_hr =
            ctx_->QueryInterface(IID_ITfContextView, reinterpret_cast<void**>(&view));

        TF_SELECTION sel{};
        ULONG fetched = 0;
        const HRESULT sel_hr = ctx_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched);
        if (SUCCEEDED(sel_hr) && fetched > 0 && sel.range != nullptr) {
            selected.Attach(sel.range);  // 接管所有权，提前 return 也自动释放
        }

        if (SUCCEEDED(view_hr) && view != nullptr && selected != nullptr) {
            CComPtr<ITfRange> caret;
            if (SUCCEEDED(selected->Clone(&caret)) &&
                SUCCEEDED(caret->Collapse(ec, TF_ANCHOR_END))) {
                BOOL clipped = FALSE;
                const HRESULT hr = view->GetTextExt(tid_, caret, &rc, &clipped);
                have_rect = SUCCEEDED(hr) && (rc.right > rc.left || rc.bottom > rc.top);
            }
        }

        // ② 回退：不折叠，直接用真实选区（非空 range）取矩形。
        //    某些应用对折叠 range 返回 TF_E_*，但对非空 range 能给出坐标。
        if (!have_rect && SUCCEEDED(view_hr) && view != nullptr && selected != nullptr) {
            BOOL clipped = FALSE;
            RECT sel_rc{};
            const HRESULT hr = view->GetTextExt(tid_, selected, &sel_rc, &clipped);
            const bool ok = SUCCEEDED(hr) && (sel_rc.right > sel_rc.left || sel_rc.bottom > sel_rc.top);
            if (ok) {
                // 选区可能非折叠：取末端作为插入符位置。
                rc.left = rc.right = sel_rc.right;
                rc.top = sel_rc.top;
                rc.bottom = sel_rc.bottom;
                have_rect = true;
            }
        }

        // ③ 回退：GUI 线程的 caret 矩形（对 Chromium / 现代控件有效，
        //    GetTextExt 常在这些应用失败，但系统仍维护 GUI caret）。
        if (!have_rect) {
            GUITHREADINFO gti{};
            gti.cbSize = sizeof(gti);
            if (GetGUIThreadInfo(0, &gti) && gti.hwndCaret != nullptr) {
                const RECT cr = gti.rcCaret;
                if (cr.right > cr.left || cr.bottom > cr.top) {
                    rc = cr;
                    have_rect = true;
                }
            }
        }

        // ④ 回退：窗口自带 caret 的应用（部分 Win32 编辑框）用 GetCaretPos。
        if (!have_rect) {
            POINT pt{};
            HWND focus = GetFocus();
            const bool got = GetCaretPos(&pt) != FALSE && (pt.x != 0 || pt.y != 0);
            if (got) {
                if (focus != nullptr) {
                    ClientToScreen(focus, &pt);
                }
                rc.left = rc.right = pt.x;
                rc.top = rc.bottom = pt.y;
                have_rect = true;
            }
        }

        // ⑤ 兜底：焦点窗口的客户区左上角内侧。图标「宁可位置粗略也要可见」——
        //    光标矩形拿不到时若直接 Hide，用户会看到「完全没有图标」，无法判断
        //    输入法是否已切换。落在焦点窗口内至少证明服务活着，随后轮询拿到精确
        //    光标会立刻纠正位置。
        if (!have_rect) {
            HWND focus = GetFocus();
            if (focus != nullptr) {
                RECT cr{};
                if (GetClientRect(focus, &cr)) {
                    POINT origin{cr.left, cr.top};
                    ClientToScreen(focus, &origin);
                    rc.left = rc.right = origin.x + 12;
                    rc.top = origin.y + 12;
                    rc.bottom = origin.y + 34;
                    have_rect = true;
                }
            }
        }

        if (have_rect) {
            status_->FollowCaret(rc);
        } else {
            // 实在拿不到任何锚点才隐藏，绝不让它停在错误的旧位置或左上角。
            status_->Hide();
        }
        return S_OK;
    }

private:
    CComPtr<ITfContext> ctx_;
    TfClientId tid_ = 0;
    StatusWindow* status_ = nullptr;
    LONG generation_ = 0;
    LONG ref_ = 1;
};

}  // namespace

TsfTextService::TsfTextService() = default;

TsfTextService::~TsfTextService() {
    // 正常情况下 Deactivate 已经 StopCore；这里是兜底。
    // 必须 join：后台 StartCore 持有 this 指针，不回收就 destructor 等于 UAF，
    // 且 std::thread 析构遇到 joinable 线程会 terminate。
    StopCore();
    StopStatusPolling();
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
        }, [this]() { this->UpdateStatusPosition(); })) {
        return E_FAIL;
    }

    // 可见的悬浮状态图标（常驻，激活即显示）。失败不影响文本写入主流程。
    if (!status_.Create()) {
        airtype::log("状态图标创建失败，输入法仍可用");
    }
    // 激活时尝试定位一次：焦点文档已就绪则不必等 OnSetFocus 事件。
    UpdateStatusPosition();
    // 持续轮询兜底：Activate 常常早于焦点文档就绪，OnSetFocus 也不保证到达。
    // 后台线程定期投递刷新消息，STA 线程收到后重试定位，直到拿到光标。
    StartStatusPolling();

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
    // 先停定位轮询：它只 PostMessage 到隐藏窗口，必须在 hidden_.Destroy() 之前
    // 停下并 join，否则会往已销毁的 HWND 投递（句柄可能复用，语义危险）。
    StopStatusPolling();
    StopCore();

    if (thread_mgr_ && sink_cookie_ != 0) {
        CComPtr<ITfSource> source;
        if (SUCCEEDED(
                thread_mgr_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source)))) {
            source->UnadviseSink(sink_cookie_);
        }
        sink_cookie_ = 0;
    }
    status_.Destroy();
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

// 跟随轮询间隔。原来是 300ms——输入框移动/光标跳动时，图标最多要等 300ms 才
// 更新一次，视觉上就是"拖着走、一顿一顿"。40ms（25Hz）接近人眼连续感，
// 配合 StatusWindow 的坐标去重与异步编辑会话，静止时几乎零开销。
constexpr DWORD kStatusPollIntervalMs = 40;

void TsfTextService::StartStatusPolling() {
    if (status_poll_thread_.joinable()) {
        return;  // 幂等：重复 Activate 不重复起线程
    }
    poll_stop_ = false;
    status_poll_thread_ = std::thread([this]() {
        const UINT msg = AirTypeMessages::Get().refreshStatus;
        airtype::log("状态图标定位轮询已启动（%ums）", static_cast<unsigned>(kStatusPollIntervalMs));
        while (!poll_stop_.load(std::memory_order_acquire)) {
            Sleep(kStatusPollIntervalMs);
            if (poll_stop_.load(std::memory_order_acquire)) {
                break;
            }
            // 句柄只读一次：Destroy 在 STA 线程把它 exchange 成 nullptr，
            // 这里拿到旧值 PostMessage 会失败（返回 0），不会误投递到复用句柄。
            // 更稳的是拿到后立刻校验仍有效，但 PostMessage 到刚销毁的窗口本身
            // 只会返回 0，无害。
            const HWND hwnd = hidden_.Handle();
            if (hwnd == nullptr) {
                break;
            }
            PostMessageW(hwnd, msg, 0, 0);
        }
        airtype::log("状态图标定位轮询已停止");
    });
}

void TsfTextService::StopStatusPolling() {
    poll_stop_ = true;
    if (status_poll_thread_.joinable()) {
        status_poll_thread_.join();
    }
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
    UpdateStatusPosition();
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
    if (written) {
        // 写入后插入符移到新文本末尾，图标跟着更新位置。
        UpdateStatusPosition();
    }
}

void TsfTextService::ResetWrittenRange() {
    written_doc_.Release();
    written_range_.Release();
}

void TsfTextService::UpdateStatusPosition() {
    if (!status_.Valid() || !thread_mgr_) {
        return;
    }
    // 别的程序里残留的激活实例：本进程不是前台进程时图标一律收起。
    if (!IsCurrentProcessForeground()) {
        status_.Hide();
        return;
    }
    // 已被切到其他输入法：即便本进程没收到 Deactivate，也要把图标收起来。
    // Activate 瞬间可能还没登记成 active，隐藏后由轮询在下一拍恢复显示。
    if (!IsAirTypeActiveKeyboard()) {
        status_.Hide();
        return;
    }
    CComPtr<ITfDocumentMgr> doc = current_doc_;
    if (!doc && FAILED(thread_mgr_->GetFocus(&doc))) {
        return;
    }
    if (!doc) {
        return;
    }
    CComPtr<ITfContext> ctx;
    if (FAILED(doc->GetTop(&ctx)) || !ctx) {
        return;
    }
    // 递增代次：排队期间若有更新的请求发出，这次查询回来时会被丢弃，
    // 避免旧坐标晚到把图标拖回上一帧的位置（异步化后必须配的代次闸门）。
    const LONG generation = ++status_.generation;

    auto* query = new CaretQuerySession(ctx, client_id_, &status_, generation);
    HRESULT session_hr = E_FAIL;
    // TF_ES_ASYNC：不阻塞目标进程 UI 线程。这是"输入框移动时跟随卡顿"的主因修复——
    // 原来的 TF_ES_SYNC 会在 UI 线程上原地等 TSF 发编辑凭证，应用忙/滚动重排时
    // 一卡就是几十到几百毫秒，图标表现为"卡住不动然后猛地跳一下"。
    const HRESULT hr = ctx->RequestEditSession(client_id_, query,
                                               TF_ES_ASYNC | TF_ES_READ, &session_hr);
    query->Release();  // RequestEditSession 已 AddRef；会话结束会释放它自己那份
    if (FAILED(hr)) {
        // 某些应用拒绝只读会话，忽略：图标留在上次位置，不影响写入。
        return;
    }
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
