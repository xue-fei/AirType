#include "TsfTextService.h"

#include <atomic>
#include <utility>

#include <uiautomation.h>

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

// 音频电平回调（Rust 音频线程，约 20ms 一次）。只投递，不碰 UI/TSF。
void CALLBACK OnLevelFromFfi(int32_t level, void* userdata) {
    auto* self = static_cast<TsfTextService*>(userdata);
    if (self != nullptr) {
        self->PostLevel(level);
    }
}

// 查询「当前活动键盘输入法」是否仍是 AirType，带进程级结果缓存。
//
// 这是**唯一**的显示门禁。早期版本还额外要求「本进程必须是前台进程」，
// 那是错的：切到 AirType 后用户常常并不把宿主窗口放前台（例如刚用切换器
// 切完输入法、焦点还在别处），此时前台窗口属于别的进程，图标被无条件
// 隐藏 —— 表现为「切换到 AirType 后悬浮框完全不出现」。
// 图标是跟着光标走的，只要 AirType 是会话的活动键盘就应显示；宿主在前台
// 与否不影响该判断。后台残留实例的回收由本函数的活动键盘判据统一负责。
//
// ⚠️ 为什么不用 GetActiveProfile 轮询：它查的是**调用线程**（我们的隐藏窗口
// STA 线程）的活动 profile，而真正决定"当前输入法"的是键盘焦点所在的线程。
// 在所有进程里这个 STA 线程都不是焦点线程，于是它恒返回 AirType（真实日志
// 400KB 里从未出现"活动键盘 = 其它"）——切到微软拼音后图标不消失。
//
// 改用 ITfInputProcessorProfileActivationSink 事件：切换输入法时 TSF 向所有
// 注册进程广播 OnActivated，参数里带新激活的 TIP clsid。事件驱动、跨进程、
// 无线程歧义。下面这个原子即事件维护的"AirType 是否当前键盘"。
std::atomic<bool> g_airtype_active{true};
std::atomic<bool> g_got_activation_event{false};

// 初值兜底：sink 注册那一刻主动查一次，覆盖"注入时已在用 AirType、
// 但之后用户没切过输入法、收不到任何事件"的场景。仅调用一次，不再轮询。
//
// ⚠️ 只在**明确查到 AirType** 时置 true，查到别的或查不到都不改初值。
//    原因：本函数在隐藏窗口的 STA 线程执行，而 GetActiveProfile 的语义是
//    "调用线程的活动 profile"——该线程不一定是键盘焦点线程，可能查不准。
//    若无条件写入 false，一旦 activation sink 注册失败（实测在某些环境返回
//    TF_E_NOOBJECT），图标就会永远不显示，比"不消失"更糟。宁可乐观显示：
//    切走的情况由 Deactivate 与「聚焦元素必须属于本进程」两条独立判据兜住。
void SeedActiveKeyboardFromProfile() {
    CComPtr<ITfInputProcessorProfileMgr> mgr;
    const HRESULT ci = CoCreateInstance(
        CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(&mgr));
    if (FAILED(ci) || !mgr) {
        return;
    }
    TF_INPUTPROCESSORPROFILE profile{};
    if (SUCCEEDED(mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile))) {
        if (profile.clsid == CLSID_AIRTYPE_TSF && profile.guidProfile == GUID_AIRTYPE_PROFILE) {
            g_airtype_active.store(true, std::memory_order_release);
        }
    }
}

bool IsAirTypeActiveKeyboard() {
    return g_airtype_active.load(std::memory_order_acquire);
}

// UIA 定位回退：为 Qt / Chromium 等自绘控件准备。
//
// 为什么需要它：这类程序不实现 TSF 的 GetTextExt（实测微信返回 E_NOINTERFACE），
// 也不向系统注册 GUI caret，前几条回退全落空，最后掉进「窗口左上角」的兜底
// 分支——表现为「图标出现在窗口左上角，而不是输入框附近」。
//
// 两级策略：
//   ① UIA TextPattern2.GetCaretRange() 拿真实光标矩形（最精确，少数程序支持）；
//   ② 退而求其次，取聚焦控件自身的 BoundingRectangle（输入框外接矩形）。
//      UIA 规范要求 provider 必须提供这个属性，微信的 Edit 控件（type 50007）
//      即使没有 TextPattern2 也能拿到。图标贴到输入框下沿，已足够接近光标。
//
// 返回屏幕坐标矩形；失败返回 false（调用方继续下一条回退）。
#ifdef AIRTYPE_TRACE_STATUS
#define TRACE_UA(...) airtype::log(__VA_ARGS__)
#else
#define TRACE_UA(...) ((void)0)
#endif

bool GetCaretRectViaUia(RECT& out) {
    // UIA 客户端调用要求调用线程已初始化 COM。本函数只在 STA 线程执行
    // （异步编辑会话回调投递回同一 STA），语义安全。
    CComPtr<IUIAutomation> uia;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, reinterpret_cast<void**>(&uia));
    if (FAILED(hr) || !uia) {
        TRACE_UA("[定位②.5] UIA CoCreateInstance 失败 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    CComPtr<IUIAutomationElement> focused;
    hr = uia->GetFocusedElement(&focused);
    if (FAILED(hr) || !focused) {
        TRACE_UA("[定位②.5] GetFocusedElement 失败 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    // 记录聚焦控件类型，便于判断 Qt/自绘控件是否暴露了 UIA 模式。
    CONTROLTYPEID ctl_type = 0;
    UIA_HWND native = nullptr;
    if (SUCCEEDED(focused->get_CurrentControlType(&ctl_type))) {
        focused->get_CurrentNativeWindowHandle(&native);
        TRACE_UA("[定位②.5] focused control type=%ld hwnd=%p", static_cast<LONG>(ctl_type),
                 reinterpret_cast<void*>(static_cast<HWND>(native)));
    }

    // ① 精确路径：TextPattern2 的折叠光标矩形。
    CComPtr<IUIAutomationTextPattern2> tp2;
    hr = focused->GetCurrentPatternAs(UIA_TextPattern2Id, IID_IUIAutomationTextPattern2,
                                      reinterpret_cast<void**>(&tp2));
    if (SUCCEEDED(hr) && tp2) {
        CComPtr<IUIAutomationTextRange> caret;
        // isActive 是 [out] 参数，部分 provider（实测微信）要求非空指针，
        // 传 nullptr 会直接返回 E_POINTER (0x80004003) —— 之前误注释为"可空"。
        BOOL caret_active = FALSE;
        hr = tp2->GetCaretRange(&caret_active, &caret);
        if (SUCCEEDED(hr) && caret) {
            SAFEARRAY* arr = nullptr;
            hr = caret->GetBoundingRectangles(&arr);
            if (SUCCEEDED(hr) && arr != nullptr) {
                const LONG count = static_cast<LONG>(arr->rgsabound[0].cElements);
                bool ok = false;
                if (count >= 4) {
                    double* data = nullptr;
                    if (SUCCEEDED(SafeArrayAccessData(arr, reinterpret_cast<void**>(&data))) &&
                        data != nullptr) {
                        // 返回 double 数组，每 4 个一组 = left/top/width/height（屏幕坐标）。
                        const double l = data[0], t = data[1], w = data[2], h = data[3];
                        SafeArrayUnaccessData(arr);
                        if (h > 0.0) {
                            // 折叠光标宽度可能为 0，仍可用：以左边界当插入点。
                            out.left = static_cast<LONG>(l);
                            out.right = static_cast<LONG>(l + (w > 0.0 ? w : 0.0));
                            out.top = static_cast<LONG>(t);
                            out.bottom = static_cast<LONG>(t + h);
                            ok = true;
                        }
                    }
                }
                SafeArrayDestroy(arr);
                TRACE_UA("[定位②.5] caret range 矩形个数=%ld ok=%d", count,
                         static_cast<int>(ok));
                if (ok) {
                    return true;
                }
            } else {
                TRACE_UA("[定位②.5] GetBoundingRectangles 失败 0x%08X",
                         static_cast<unsigned>(hr));
                if (arr != nullptr) {
                    SafeArrayDestroy(arr);
                }
            }
        } else {
            TRACE_UA("[定位②.5] GetCaretRange 失败 0x%08X", static_cast<unsigned>(hr));
        }
    } else {
        // 实测微信：hr=S_OK 但 tp2 为空；Qt 自绘控件普遍不支持 TextPattern2。
        TRACE_UA("[定位②.5] TextPattern2 不可用 hr=0x%08X tp2=%d",
                 static_cast<unsigned>(hr), static_cast<int>(tp2 != nullptr));
    }

    // ①.5 折中路径：TextPattern1.GetSelection() 的折叠选区即光标位置。
    //     微信的 Edit 控件不提供 caret range（矩形个数=0），但通常实现了
    //     基础的 TextPattern。取 selection 里第一个 range 的外接矩形。
    CComPtr<IUIAutomationTextPattern> tp1;
    hr = focused->GetCurrentPatternAs(UIA_TextPatternId, IID_IUIAutomationTextPattern,
                                      reinterpret_cast<void**>(&tp1));
    if (SUCCEEDED(hr) && tp1) {
        CComPtr<IUIAutomationTextRangeArray> sels;
        hr = tp1->GetSelection(&sels);
        if (SUCCEEDED(hr) && sels) {
            int count = 0;
            hr = sels->get_Length(&count);
            CComPtr<IUIAutomationTextRange> first;
            if (SUCCEEDED(hr) && count > 0) {
                sels->GetElement(0, &first);
            }
            if (first) {
                SAFEARRAY* arr = nullptr;
                const HRESULT hrb = first->GetBoundingRectangles(&arr);
                if (SUCCEEDED(hrb) && arr != nullptr) {
                    const LONG n = static_cast<LONG>(arr->rgsabound[0].cElements);
                    bool ok = false;
                    if (n >= 4) {
                        double* data = nullptr;
                        if (SUCCEEDED(SafeArrayAccessData(arr, reinterpret_cast<void**>(&data))) &&
                            data != nullptr) {
                            // 取最后一组矩形（selection 末端 = 插入点），每组 4 个 double。
                            const double l = data[n - 4], t = data[n - 3];
                            const double w = data[n - 2], h = data[n - 1];
                            SafeArrayUnaccessData(arr);
                            if (h > 0.0) {
                                out.left = out.right = static_cast<LONG>(l + (w > 0.0 ? w : 0.0));
                                out.top = static_cast<LONG>(t);
                                out.bottom = static_cast<LONG>(t + h);
                                ok = true;
                            }
                        }
                    }
                    SafeArrayDestroy(arr);
                    TRACE_UA("[定位②.5] selection 矩形个数=%ld ok=%d", n, static_cast<int>(ok));
                    if (ok) {
                        return true;
                    }
                } else {
                    TRACE_UA("[定位②.5] selection GetBoundingRectangles 失败 0x%08X",
                             static_cast<unsigned>(hrb));
                    if (arr != nullptr) {
                        SafeArrayDestroy(arr);
                    }
                }
            } else {
                TRACE_UA("[定位②.5] selection 无 range count=%d", count);
            }
        } else {
            TRACE_UA("[定位②.5] GetSelection 失败 0x%08X", static_cast<unsigned>(hr));
        }
    } else {
        TRACE_UA("[定位②.5] TextPattern1 不可用 hr=0x%08X", static_cast<unsigned>(hr));
    }

    // ② 兜底：聚焦控件自身的外接矩形（即输入框）。UIA 规范要求 provider
    //    必须提供该属性，不依赖 caret 支持。图标贴到这个矩形下沿即可落在
    //    输入框下方——对微信这类"认得输入框但不给光标"的程序是关键路径。
    //
    //    ⚠️ 必须校验控件类型与尺寸：微信里 GetFocusedElement 有时返回的是
    //    聊天消息列表（type=50007 List）而非输入框（type=50004 Edit），
    //    整个列表矩形高数百像素，图标贴其下沿就跑到「窗口左下角」。
    //    只接受 Edit/Document 且高度合理的矩形。
    const bool editable = ctl_type == UIA_EditControlTypeId ||
                          ctl_type == UIA_DocumentControlTypeId;
    RECT ur{};
    hr = focused->get_CurrentBoundingRectangle(&ur);
    const LONG ur_h = ur.bottom - ur.top;
    const LONG ur_w = ur.right - ur.left;
    if (editable && SUCCEEDED(hr) && ur_w > 0 && ur_h > 0 && ur_h <= 200) {
        out.left = ur.left;
        out.top = ur.top;
        out.right = ur.right;
        out.bottom = ur.bottom;
        TRACE_UA("[定位②.5] 用输入框矩形 rect=(%d,%d,%d,%d)", ur.left, ur.top,
                 ur.right, ur.bottom);
        return true;
    }
    TRACE_UA("[定位②.5] 放弃矩形 type=%ld editable=%d hr=0x%08X rect=(%d,%d,%d,%d)",
             static_cast<LONG>(ctl_type), static_cast<int>(editable),
             static_cast<unsigned>(hr), ur.left, ur.top, ur.right, ur.bottom);

    // ②.7 主动搜索输入框：微信的聚焦元素常是整个内容区（Document，实测高 877px），
    //     尺寸过滤后没有候选，于是掉进前台窗口兜底（图标压在标题栏上）。
    //     从聚焦元素所在的顶层窗口遍历 UIA 子树，找所有 Edit/Document 节点，
    //     取宽>150、高 20~200 且最靠下（top 最大）的那个——聊天输入框几乎总在
    //     窗口底部，这个启发式比"赌聚焦元素恰好是输入框"稳得多。
    {
        HWND root_hwnd = static_cast<HWND>(native);
        if (root_hwnd == nullptr) {
            root_hwnd = GetForegroundWindow();
        }
        CComPtr<IUIAutomationElement> root;
        if (root_hwnd != nullptr &&
            SUCCEEDED(uia->ElementFromHandle(static_cast<UIA_HWND>(root_hwnd), &root)) && root) {
            CComVariant v_edit(UIA_EditControlTypeId), v_doc(UIA_DocumentControlTypeId);
            CComPtr<IUIAutomationCondition> cond_edit, cond_doc, cond;
            if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v_edit,
                                                       &cond_edit)) &&
                SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v_doc,
                                                       &cond_doc)) &&
                SUCCEEDED(uia->CreateOrCondition(cond_edit, cond_doc, &cond))) {
                CComPtr<IUIAutomationElementArray> found;
                if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &found)) && found) {
                    int n = 0;
                    found->get_Length(&n);
                    RECT best{};
                    bool have = false;
                    for (int i = 0; i < n; ++i) {
                        CComPtr<IUIAutomationElement> el;
                        found->GetElement(i, &el);
                        if (!el) {
                            continue;
                        }
                        RECT r{};
                        if (FAILED(el->get_CurrentBoundingRectangle(&r))) {
                            continue;
                        }
                        const LONG w = r.right - r.left, h = r.bottom - r.top;
                        if (w > 150 && h >= 20 && h <= 200) {
                            if (!have || r.top > best.top) {
                                best = r;
                                have = true;
                            }
                        }
                    }
                    if (have) {
                        out = best;
                        TRACE_UA("[定位②.5] FindAll 找到输入框 rect=(%d,%d,%d,%d) count=%d",
                                 best.left, best.top, best.right, best.bottom, n);
                        return true;
                    }
                    TRACE_UA("[定位②.5] FindAll 无合格输入框 count=%d", n);
                } else {
                    TRACE_UA("[定位②.5] FindAll 失败");
                }
            }
        }
    }
    return false;
}

// 焦点是否落在可编辑文本控件上——"焦点不在输入框就隐藏图标"的判据。
//
// 为什么需要：UIA 的 FindAll 能在页面里找到输入框，但用户去点设置面板、
// 聊天记录、侧边栏时，图标不该继续挂着。以 UIA 聚焦元素为准：
//   · Edit / Document / ComboBox —— 典型文本输入控件，放行；
//   · 其它类型 —— 看它是否实现 TextPattern（Slate/Qt 富文本容器的常见形态）；
//   · UIA 不可用或调用失败 —— 放行。宁可多显示，也不因为查不到而误隐藏。
//
// 结果缓存 120ms：焦点切换本就有延迟，40ms 轮询下没必要每拍都走 UIA。
bool FocusIsTextEditable() {
    static CComPtr<IUIAutomation> uia;
    static bool have_result = false;
    static bool cached = true;
    static DWORD last_tick = 0;

    const DWORD now = GetTickCount();
    if (have_result && (now - last_tick) < 120 && now >= last_tick) {
        return cached;
    }

    if (!uia &&
        (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_IUIAutomation, reinterpret_cast<void**>(&uia))) ||
         !uia)) {
        return true;
    }

    CComPtr<IUIAutomationElement> focused;
    if (FAILED(uia->GetFocusedElement(&focused)) || !focused) {
        return true;
    }
    CONTROLTYPEID type = 0;
    if (FAILED(focused->get_CurrentControlType(&type))) {
        return true;
    }

    // ① 聚焦元素必须属于本进程。UIA 的 GetFocusedElement 返回的是**整个桌面**
    //    的焦点：切到别的程序后，本进程查询会拿到那个程序里的输入框，若只看
    //    控件类型就会误判为「焦点还在输入框」，图标一直挂着——这正是"切换
    //    程序/输入法后图标不消失"的主因。以进程归属做硬性收窄。
    int focused_pid = 0;
    const bool pid_ok =
        FAILED(focused->get_CurrentProcessId(&focused_pid)) ||
        focused_pid == static_cast<int>(GetCurrentProcessId());

    bool editable = false;
    if (pid_ok) {
        editable = type == UIA_EditControlTypeId || type == UIA_DocumentControlTypeId ||
                   type == UIA_ComboBoxControlTypeId;
        if (!editable) {
            CComPtr<IUIAutomationTextPattern> tp;
            if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_TextPatternId,
                                                       IID_IUIAutomationTextPattern,
                                                       reinterpret_cast<void**>(&tp))) &&
                tp) {
                editable = true;
            }
        }
    }

    cached = editable;
    have_result = true;
    last_tick = now;
#ifdef AIRTYPE_TRACE_STATUS
    // 只在状态翻转时打印：40ms 轮询下每条都写会把日志刷爆（单日已 8MB）。
    static bool last_logged = true;
    if (last_logged != editable) {
        if (!pid_ok) {
            airtype::log("[焦点] 焦点在其它进程 pid=%d -> 隐藏图标", focused_pid);
        } else {
            airtype::log("[焦点] 非文本控件 type=%ld -> 隐藏图标", static_cast<LONG>(type));
        }
        last_logged = editable;
    }
#endif
    return editable;
}

// 独立于 TSF 上下文的定位路径。
//
// 为什么必须独立：微信等 Qt 程序在「刚切到输入法、还没开始打字」时不会给
// TSF 活动文档（GetFocus/GetTop 失败），编辑会话整条链路走不通；若在这些
// 地方静默 return，图标就一直保持创建时的隐藏状态——用户看到的就是
// 「切到 AirType 后完全没显示」。UIA 不依赖 TSF，仍能从聚焦元素拿输入框。
void UpdateStatusViaUiaOnly(StatusWindow& status) {
    RECT rc{};
    if (GetCaretRectViaUia(rc)) {
        status.FollowCaret(rc);
#ifdef AIRTYPE_TRACE_STATUS
        airtype::log("[定位] TSF 无上下文 -> UIA 输入框 rect=(%d,%d,%d,%d)", rc.left,
                     rc.top, rc.right, rc.bottom);
#endif
        return;
    }

    // UIA 也拿不到：退到前台窗口客户区底部中央，宁可粗略也要可见。
    // 早期放在左上角（origin+8），图标会压在标题栏/搜索栏上，比不显示还糟；
    // 放底部中央至少贴近"输入框大概在窗口下方"的直觉。
    HWND fg = GetForegroundWindow();
    if (fg != nullptr) {
        RECT cr{};
        if (GetClientRect(fg, &cr) != FALSE) {
            POINT origin{cr.left, cr.top};
            ClientToScreen(fg, &origin);
            const LONG cw = cr.right - cr.left;
            const LONG ch = cr.bottom - cr.top;
            // StatusWindow 是 kStatusIconSize 见方的圆；底部留 12px 边距。
            const LONG cx = origin.x + (cw > kStatusIconSize ? (cw - kStatusIconSize) / 2 : 8);
            const LONG cy =
                origin.y + (ch > kStatusIconSize + 12 ? ch - kStatusIconSize - 12 : ch / 2);
            rc.left = cx;
            rc.right = cx + kStatusIconSize;
            rc.top = cy;
            rc.bottom = cy + kStatusIconSize;
            status.FollowCaret(rc);
#ifdef AIRTYPE_TRACE_STATUS
            airtype::log("[定位] TSF/UIA 均无锚点 -> 前台窗口底部兜底 rect=(%d,%d,%d,%d)",
                         rc.left, rc.top, rc.right, rc.bottom);
#endif
            return;
        }
    }
#ifdef AIRTYPE_TRACE_STATUS
    airtype::log("[定位] TSF/UIA/窗口均无锚点 -> Hide");
#endif
    status.Hide();
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
        // 窗口已销毁（Deactivate 在会话排队期间触发）：对象可能已被释放，
        // 直接放弃，绝不再碰 status_。这是 TF_ES_ASYNC 引入的裸指针风险点。
        if (!status_->alive.load(std::memory_order_acquire)) {
            return S_OK;
        }
        // 代次已过期：这帧是排队期间被更新的旧请求，丢弃，别把图标拖回旧位置。
        if (generation_ != status_->generation.load(std::memory_order_acquire)) {
            return S_OK;
        }

        RECT rc{};
        bool have_rect = false;
        const char* src_name = "无锚点";
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

        #ifdef AIRTYPE_TRACE_STATUS
        airtype::log("[定位①] view_hr=0x%08X sel_hr=0x%08X fetched=%lu has_view=%d has_sel=%d",
                     static_cast<unsigned>(view_hr), static_cast<unsigned>(sel_hr),
                     static_cast<unsigned long>(fetched), static_cast<int>(view != nullptr),
                     static_cast<int>(selected != nullptr));
#endif
        if (SUCCEEDED(view_hr) && view != nullptr && selected != nullptr) {
            CComPtr<ITfRange> caret;
            if (SUCCEEDED(selected->Clone(&caret)) &&
                SUCCEEDED(caret->Collapse(ec, TF_ANCHOR_END))) {
                BOOL clipped = FALSE;
                const HRESULT hr = view->GetTextExt(tid_, caret, &rc, &clipped);
#ifdef AIRTYPE_TRACE_STATUS
                airtype::log("[定位①] GetTextExt(折叠) -> 0x%08X rect=(%d,%d,%d,%d)",
                             static_cast<unsigned>(hr), rc.left, rc.top, rc.right, rc.bottom);
#endif
                if (SUCCEEDED(hr) && (rc.right > rc.left || rc.bottom > rc.top)) {
                    have_rect = true;
                    src_name = "GetTextExt(折叠)";
                }
            }
        }

        // ② 回退：不折叠，直接用真实选区（非空 range）取矩形。
        //    某些应用对折叠 range 返回 TF_E_*，但对非空 range 能给出坐标。
        if (!have_rect && SUCCEEDED(view_hr) && view != nullptr && selected != nullptr) {
            BOOL clipped = FALSE;
            RECT sel_rc{};
            const HRESULT hr = view->GetTextExt(tid_, selected, &sel_rc, &clipped);
            const bool ok = SUCCEEDED(hr) && (sel_rc.right > sel_rc.left || sel_rc.bottom > sel_rc.top);
#ifdef AIRTYPE_TRACE_STATUS
            airtype::log("[定位②] GetTextExt(选区) -> 0x%08X rect=(%d,%d,%d,%d)",
                         static_cast<unsigned>(hr), sel_rc.left, sel_rc.top, sel_rc.right,
                         sel_rc.bottom);
#endif
            if (ok) {
                // 选区可能非折叠：取末端作为插入符位置。
                rc.left = rc.right = sel_rc.right;
                rc.top = sel_rc.top;
                rc.bottom = sel_rc.bottom;
                have_rect = true;
                src_name = "GetTextExt(选区)";
            }
        }

        // ②.5 UIA 光标回退：Qt / Chromium 等自绘控件的标准定位手段。
        //     放在 Win32 caret 类回退之前——它们在这些程序上必然失败。
        if (!have_rect) {
            RECT uia_rc{};
            if (GetCaretRectViaUia(uia_rc)) {
                rc = uia_rc;
                have_rect = true;
                src_name = "UIA caret";
            }
        }

        // ③ 回退：GUI 线程的 caret 矩形（对 Chromium / Qt 自绘等现代控件有效，
        //    GetTextExt 常在这些应用失败，但系统仍维护 GUI caret）。
        //
        //    ⚠️ rcCaret 是**客户区坐标**（相对 hwndCaret），不是屏幕坐标。
        //    早年直接当屏幕坐标用，导致 Qt/自绘类应用图标整体偏移一个窗口
        //    标题栏高度。必须 ClientToScreen(hwndCaret) 转换后再用。
        if (!have_rect) {
            GUITHREADINFO gti{};
            gti.cbSize = sizeof(gti);
            if (GetGUIThreadInfo(0, &gti) && gti.hwndCaret != nullptr) {
                RECT cr = gti.rcCaret;  // 客户区坐标
                const bool ok = cr.right > cr.left || cr.bottom > cr.top;
#ifdef AIRTYPE_TRACE_STATUS
                airtype::log("[定位③] GUI caret hwnd=%p rect=(%d,%d,%d,%d) ok=%d",
                             static_cast<void*>(gti.hwndCaret), cr.left, cr.top, cr.right,
                             cr.bottom, static_cast<int>(ok));
#endif
                if (ok) {
                    // 折叠成零宽插入符点：FollowCaret 只用到 right 与垂直区间。
                    POINT cpt{cr.left, cr.top};
                    ClientToScreen(gti.hwndCaret, &cpt);
                    const int h = cr.bottom - cr.top;
                    rc.left = rc.right = cpt.x;
                    rc.top = cpt.y;
                    rc.bottom = cpt.y + h;
                    have_rect = true;
                    src_name = "GUI caret";
                }
            }
        }

        // ④ 回退：窗口自带 caret 的应用（部分 Win32 编辑框）用 GetCaretPos。
        if (!have_rect) {
            POINT pt{};
            HWND focus = GetFocus();
            const BOOL caret_ok = GetCaretPos(&pt);
            const bool got = caret_ok != FALSE && (pt.x != 0 || pt.y != 0);
#ifdef AIRTYPE_TRACE_STATUS
            airtype::log("[定位④] GetCaretPos -> %d pt=(%d,%d) focus=%p",
                         static_cast<int>(caret_ok), pt.x, pt.y,
                         static_cast<void*>(focus));
#endif
            if (got) {
                if (focus != nullptr) {
                    ClientToScreen(focus, &pt);
                }
                rc.left = rc.right = pt.x;
                rc.top = rc.bottom = pt.y;
                have_rect = true;
                src_name = "GetCaretPos";
            }
        }

        // ⑤ 兜底：所有定位手段都失败时的最后手段。图标「宁可位置粗略也要可见」——
        //    光标矩形拿不到时若直接 Hide，用户会看到「完全没有图标」，无法判断
        //    输入法是否已切换。
        //
        //    位置选在**聚焦控件自身**的左上角附近，而不是主窗口客户区左上角：
        //    Qt/自绘程序的 GetFocus() 常返回外层主窗口，落在那里就是用户看到的
        //    「图标出现在窗口左上角」。改用 UIA 的 focused element 拿真实输入控件
        //    矩形（比 GetFocus 精确），拿不到才退回主窗口内侧。
        if (!have_rect) {
            HWND target = GetFocus();
            RECT cr{};
            bool got_rect = false;
            if (target != nullptr) {
                got_rect = GetClientRect(target, &cr) != FALSE;
            }
            if (got_rect) {
                POINT origin{cr.left, cr.top};
                ClientToScreen(target, &origin);
                rc.left = rc.right = origin.x + 8;
                rc.top = origin.y + 6;
                rc.bottom = origin.y + 30;
                have_rect = true;
                src_name = "窗口兜底";
            }
        }

        #ifdef AIRTYPE_TRACE_STATUS
        // 只打原始矩形与来源；图标落点由 StatusWindow::FollowCaret 计算。
        airtype::log("[定位] 结果 source=%s rect=(%d,%d,%d,%d)", src_name, rc.left, rc.top,
                     rc.right, rc.bottom);
#endif
        if (have_rect) {
            status_->FollowCaret(rc);
        } else {
            // 实在拿不到任何锚点才隐藏，绝不让它停在错误的旧位置或左上角。
#ifdef AIRTYPE_TRACE_STATUS
            airtype::log("[定位] 无任何锚点 -> Hide");
#endif
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
    if (riid == IID_ITfInputProcessorProfileActivationSink) {
        *ppv = static_cast<ITfInputProcessorProfileActivationSink*>(this);
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

    if (!hidden_.Create(
            L"AirType TSF 消息窗口",
            [this](AirTypePayload* p) { this->OnPayload(p); },
            [this]() { this->UpdateStatusPosition(); },
            [this](int level) { this->OnLevel(level); })) {
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

    // 注册活动键盘变化通知：切到其他输入法时 TSF 广播 OnActivated，
    // 这是"切走即隐藏图标"的可靠信号源（GetActiveProfile 轮询查的是本
    // STA 线程，恒返回 AirType，实测失效）。
    //
    // ⚠️ 必须从 IID_ITfInputProcessorProfileMgr 上 QI ITfSource：早先对
    // IID_ITfInputProcessorProfiles 做 QI 再 AdviseSink，返回 TF_E_NOOBJECT
    // (0x80040202)，sink 根本没挂上（--dllselftest 实测）。
    CComPtr<ITfInputProcessorProfileMgr> profile_mgr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr,
                                   reinterpret_cast<void**>(&profile_mgr))) &&
        profile_mgr) {
        CComPtr<ITfSource> profile_src;
        if (SUCCEEDED(profile_mgr->QueryInterface(IID_ITfSource,
                                                  reinterpret_cast<void**>(&profile_src)))) {
            const HRESULT advised = profile_src->AdviseSink(
                IID_ITfInputProcessorProfileActivationSink,
                static_cast<ITfInputProcessorProfileActivationSink*>(this),
                &profile_sink_cookie_);
            if (FAILED(advised)) {
                airtype::log_hr("AdviseSink(ProfileActivationSink)", advised);
            } else {
                airtype::log("活动键盘通知已注册 cookie=%lu",
                             static_cast<unsigned long>(profile_sink_cookie_));
            }
        }
        // 注册后主动查一次当前活动键盘作为初值（之后完全靠事件更新）。
        SeedActiveKeyboardFromProfile();
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
    if (profile_sink_cookie_ != 0) {
        CComPtr<ITfInputProcessorProfileMgr> profile_mgr;
        if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                       CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr,
                                       reinterpret_cast<void**>(&profile_mgr))) &&
            profile_mgr) {
            CComPtr<ITfSource> profile_src;
            if (SUCCEEDED(profile_mgr->QueryInterface(IID_ITfSource,
                                                      reinterpret_cast<void**>(&profile_src)))) {
                profile_src->UnadviseSink(profile_sink_cookie_);
            }
        }
        profile_sink_cookie_ = 0;
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

STDMETHODIMP TsfTextService::OnActivated(DWORD dwProfileType, LANGID /*langid*/, REFCLSID clsid,
                                         REFGUID /*catid*/, REFGUID /*guidProfile*/,
                                         HKL /*hkl*/, DWORD dwFlags) {
    // 只关心键盘输入处理器（不是键盘布局、不是语音/手写）。
    if (dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR) {
        return S_OK;
    }
    // dwFlags 带 TF_IPSINK_FLAG_ACTIVE = 该 profile 刚被激活为当前键盘。
    // 未带标志的是"即将停用"通知，忽略——下一条带标志的通知会给出新的键盘。
    if ((dwFlags & TF_IPSINK_FLAG_ACTIVE) == 0) {
        return S_OK;
    }

    const bool now_airtype = (clsid == CLSID_AIRTYPE_TSF);
    const bool was_airtype = g_airtype_active.exchange(now_airtype, std::memory_order_acq_rel);
    g_got_activation_event.store(true, std::memory_order_release);

#ifdef AIRTYPE_TRACE_STATUS
    airtype::log("[键盘切换] 新活动键盘=%s (clsid=%08lX-%04X-...)",
                 now_airtype ? "AirType" : "其它（收起图标）",
                 static_cast<unsigned long>(clsid.Data1), static_cast<unsigned>(clsid.Data2));
#endif

    // 切走时立即收起图标，不等下一次 40ms 轮询。
    if (was_airtype && !now_airtype && active_) {
        status_.Hide();
    }
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
    // 音量可视化。旧版 ffi 没有该导出时返回 ERR_STATE，只影响填充，不影响识别。
    if (ffi_.SetLevelCallback(OnLevelFromFfi, this) != AIRTYPE_OK) {
        airtype::log("airtype_set_level_callback 不可用（ffi 版本过旧），音量填充将不显示");
    }

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
    //
    // 版本目录隔离后 DLL 在 {app}\versions\<版本>\，而 config 仍在 {app}\config\
    // （配置不随版本走，用户改过的要保留），所以必须**逐级向上**找：
    //   {app}\versions\<版本>\config\airtype.toml   （没有）
    //   {app}\versions\config\airtype.toml          （没有）
    //   {app}\config\airtype.toml                   ← 命中
    // 宿主 exe 目录作为兜底：独立探针把配置放在自己旁边时仍然可用。
    const std::wstring names[] = {L"config\\airtype.toml", L"airtype.toml"};

    auto try_dir = [&names](const std::wstring& dir) -> std::wstring {
        if (dir.empty()) {
            return {};
        }
        for (const std::wstring& name : names) {
            const std::wstring path = dir + name;
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return path;
            }
        }
        return {};
    };

    std::wstring dir = SelfModuleDir();
    for (int depth = 0; depth < 4 && !dir.empty(); ++depth) {
        const std::wstring hit = try_dir(dir);
        if (!hit.empty()) {
            return hit;
        }
        // 去掉末尾分隔符后截到上一级（SelfModuleDir 带尾反斜杠）。
        std::wstring trimmed = dir;
        while (trimmed.size() > 1 && (trimmed.back() == L'\\' || trimmed.back() == L'/')) {
            trimmed.pop_back();
        }
        const size_t pos = trimmed.find_last_of(L"\\/");
        if (pos == std::wstring::npos || pos < 2) {
            break;  // 到盘根，别再往上
        }
        dir = trimmed.substr(0, pos + 1);
    }

    return try_dir(HostExeDir());
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

void TsfTextService::PostLevel(int level) {
    const HWND hwnd = hidden_.Handle();
    if (hwnd == nullptr) {
        return;  // 未激活 / 已销毁：直接丢，不打日志（20ms 一帧会刷爆）
    }
    // wParam 直接带值：0..1000 为毫电平，-1 表示 PTT 结束。
    // 转 WPARAM 再转回 int 走补码，负数不会丢信息。
    PostMessageW(hwnd, AirTypeMessages::Get().level, static_cast<WPARAM>(level), 0);
}

void TsfTextService::OnLevel(int level) {
    // SetLevel 内部会判 hwnd_，窗口没建好时自然忽略，这里不必再挡。
    if (level < 0) {
        // PTT 结束/取消：立即回落，避免停在残值。
        level_smoothed_ = 0.0f;
        status_.SetLevel(0);
        return;
    }
    const float target = static_cast<float>(level) / 1000.0f;
    // 上升快、下降慢：音量逐帧抖动时填充不闪，视觉上更像电平表。
    const float alpha = target > level_smoothed_ ? 0.55f : 0.22f;
    level_smoothed_ += (target - level_smoothed_) * alpha;
    status_.SetLevel(static_cast<int>(level_smoothed_ * 1000.0f + 0.5f));
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
    // 唯一的显示门禁：AirType 是否仍是会话的活动键盘。
    // 曾被切到其他输入法时（即便本进程没收到 Deactivate），把图标收起来；
    // 回到 AirType 后由轮询在下一拍（≤40ms）恢复显示。
    const bool active = IsAirTypeActiveKeyboard();
#ifdef AIRTYPE_TRACE_STATUS
    // 只在状态变化时打印，否则 25Hz 会刷满日志。
    static bool last_active = true;
    if (active != last_active) {
        airtype::log("[定位] 活动键盘 = %s", active ? "AirType" : "其它（隐藏图标）");
        last_active = active;
    }
#endif
    if (!active) {
        status_.Hide();
        return;
    }
    // 焦点不在可编辑文本控件（点了设置面板/聊天列表/桌面等）时收起图标。
    // 放在活动键盘判据之后：先做便宜的键盘状态判断，再走 UIA。
    if (!FocusIsTextEditable()) {
        status_.Hide();
        return;
    }
    CComPtr<ITfDocumentMgr> doc = current_doc_;
    if (!doc && FAILED(thread_mgr_->GetFocus(&doc))) {
        // 微信等 Qt 程序在没开始打字时拿不到 TSF 文档。不能在这里静默 return，
        // 否则图标永远停在「创建即隐藏」状态（切到 AirType 完全没显示）。
#ifdef AIRTYPE_TRACE_STATUS
        airtype::log("[定位] TSF 无焦点文档 -> UIA 独立路径");
#endif
        UpdateStatusViaUiaOnly(status_);
        return;
    }
    if (!doc) {
#ifdef AIRTYPE_TRACE_STATUS
        airtype::log("[定位] TSF 焦点文档为空 -> UIA 独立路径");
#endif
        UpdateStatusViaUiaOnly(status_);
        return;
    }
    CComPtr<ITfContext> ctx;
    if (FAILED(doc->GetTop(&ctx)) || !ctx) {
#ifdef AIRTYPE_TRACE_STATUS
        airtype::log("[定位] TSF 无上下文 -> UIA 独立路径");
#endif
        UpdateStatusViaUiaOnly(status_);
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
        // 某些应用拒绝只读会话：改走 UIA 独立路径，别让图标停在隐藏状态。
#ifdef AIRTYPE_TRACE_STATUS
        airtype::log("[定位] RequestEditSession 失败 0x%08X -> UIA 独立路径",
                     static_cast<unsigned>(hr));
#endif
        UpdateStatusViaUiaOnly(status_);
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
