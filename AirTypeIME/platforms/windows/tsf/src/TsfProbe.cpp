// M1 风险探针：在进程内验证 TSF 编辑会话写入路径。
//
// 目的：在动手写 airtype_tsf.dll 之前，先确认这条链路在本机可用——
//   ITfThreadMgr::Activate -> GetFocus -> ITfDocumentMgr::GetTop
//   -> ITfContext::RequestEditSession -> ITfRange::SetText
//
// 本探针**不注册、不注入**任何文本服务，只调用系统自带的 TSF。
//
// 用法：
//   airtype_tsf_probe.exe                仅诊断：报告焦点文档/上下文/编辑会话是否可用
//   airtype_tsf_probe.exe --write "你好"  把 UTF-8 文本写入当前焦点输入框
//   airtype_tsf_probe.exe --window       自建一个 EDIT 窗口作为写入目标（不依赖外部程序）
//   airtype_tsf_probe.exe --ffi <path>   顺带加载 airtype_ffi.dll 并打印版本
//
// 说明：TSF 接口要求 STA。本文件所有 COM 调用都在主线程的 STA 里完成。

#include <windows.h>
#include <msctf.h>
#include <richedit.h>
#include <uiautomation.h>

#include <atlbase.h>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "HiddenWindow.h"
#include "Log.h"
#include "FfiBridge.h"
#include "StatusWindow.h"
#include "TsfGuids.h"

#include "../include/airtype_ffi.h"

namespace {

// UTF-8 -> UTF-16。探针只处理 BMP 之外的代理对，不做错误恢复。
std::wstring utf8_to_utf16(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// UTF-16 -> UTF-8。日志走窄 printf，宽字符必须先转回来。
std::string utf16_to_utf8(const std::wstring& s) {
    if (s.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

/// 一次编辑会话：把给定 UTF-16 文本写进当前选区（替换选区内容）。
class InsertEditSession : public ITfEditSession {
public:
    InsertEditSession(ITfContext* ctx, std::wstring text, bool move_caret, bool read_back,
                      bool dump_only)
        : ctx_(ctx),
          text_(std::move(text)),
          move_caret_(move_caret),
          read_back_(read_back),
          dump_only_(dump_only) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
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
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (r == 0) {
            delete this;
        }
        return r;
    }

    // ITfEditSession
    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        if (dump_only_) {
            return DumpDocument(ec);
        }
        TF_SELECTION sel{};
        ULONG fetched = 0;
        HRESULT hr = ctx_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched);
        if (FAILED(hr)) {
            airtype::log_hr("  GetSelection", hr);
            return hr;
        }
        if (fetched == 0) {
            airtype::log("  没有默认选区，写入失败");
            return E_FAIL;
        }

        const LONG len = static_cast<LONG>(text_.size());
        hr = sel.range->SetText(ec, 0, text_.c_str(), len);
        if (FAILED(hr)) {
            airtype::log_hr("  ITfRange::SetText", hr);
            return hr;
        }
        airtype::log("  已写入 %ld 个 UTF-16 字符", len);

        if (read_back_) {
            // 从 TSF 侧回读同一段 range，确认这个 context 真的收下了文本。
            // 必须在 Collapse 之前做：折叠后 range 只剩一个插入点。
            wchar_t back[1024]{};
            ULONG got = 0;
            HRESULT rh = sel.range->GetText(ec, 0, back, 1024, &got);
            airtype::log_hr("  ITfRange::GetText", rh);
            if (SUCCEEDED(rh)) {
                back[got < 1024 ? got : 1023] = L'\0';
                const bool same = text_ == back;
                airtype::log("  TSF 回读: %s", utf16_to_utf8(back).c_str());
                airtype::log("  TSF 回读一致: %s", same ? "是" : "否");
            }
        }

        if (move_caret_) {
            // 把插入后的光标放到文本末尾。
            TF_SELECTION end = sel;
            end.style.ase = TF_AE_NONE;
            end.style.fInterimChar = FALSE;
            hr = sel.range->Collapse(ec, TF_ANCHOR_END);
            if (SUCCEEDED(hr)) {
                ctx_->SetSelection(ec, 1, &end);
            }
        }
        return S_OK;
    }

private:
    ~InsertEditSession() = default;

    /// 转储整个文档的文本，用来判断这个 context 到底属于谁。
    HRESULT DumpDocument(TfEditCookie ec) {
        CComPtr<ITfRange> start;
        CComPtr<ITfRange> end;
        CComPtr<ITfRange> whole;
        HRESULT hr = ctx_->GetStart(ec, &start);
        if (FAILED(hr)) {
            airtype::log_hr("  GetStart", hr);
            return hr;
        }
        hr = ctx_->GetEnd(ec, &end);
        if (FAILED(hr)) {
            airtype::log_hr("  GetEnd", hr);
            return hr;
        }
        hr = start->Clone(&whole);
        if (FAILED(hr)) {
            return hr;
        }
        hr = whole->ShiftEndToRange(ec, end, TF_ANCHOR_END);
        if (FAILED(hr)) {
            airtype::log_hr("  ShiftEndToRange", hr);
            return hr;
        }
        wchar_t buf[2048]{};
        ULONG got = 0;
        hr = whole->GetText(ec, 0, buf, 2048, &got);
        if (SUCCEEDED(hr)) {
            buf[got < 2048 ? got : 2047] = L'\0';
            airtype::log("  整文档转储(%lu): %s", got, utf16_to_utf8(buf).c_str());
        } else {
            airtype::log_hr("  整文档 GetText", hr);
        }
        return hr;
    }

    CComPtr<ITfContext> ctx_;
    std::wstring text_;
    bool move_caret_;
    bool read_back_;
    bool dump_only_;
    ULONG ref_ = 1;
};

// ---------------------------------------------------------------------------
// 自建窗口模式：保证有一个 TSF 文档可写，不依赖外部程序是否获得焦点。
// ---------------------------------------------------------------------------

constexpr wchar_t kClassName[] = L"AirTypeTsfProbeWnd";
constexpr wchar_t kEditClass[] = L"EDIT";
constexpr int kEditId = 1001;
constexpr int kWriteId = 1002;

struct ProbeWindow {
    HWND hwnd = nullptr;
    HWND edit = nullptr;
    std::string pending_write;
    bool use_self_window = false;
};

ProbeWindow g_probe;

/// 尽力把窗口切到前台。跨进程 SetForegroundWindow 会被系统前台锁拦，
/// 需要 AttachThreadInput 借一次前台权限。
void ForceForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD cur_tid = GetCurrentThreadId();
    bool attached = false;
    if (fg_tid != 0 && fg_tid != cur_tid) {
        attached = AttachThreadInput(fg_tid, cur_tid, TRUE);
    }
    SetForegroundWindow(hwnd);
    if (attached) {
        AttachThreadInput(fg_tid, cur_tid, FALSE);
    }
}

void DoWindowWrite(HWND hwnd) {
    // 窗口模式：直接对 edit 控件写入，用来和 TSF 路径做对照。
    if (g_probe.pending_write.empty()) {
        return;
    }
    std::wstring text = utf8_to_utf16(g_probe.pending_write);
    SetWindowTextW(g_probe.edit, text.c_str());
    airtype::log("  [对照] SetWindowTextW 已写入 %zu 字符", text.size());
}

/// 把 32 位 BGRA 像素写成 BMP。
bool WriteBmp(const wchar_t* path, int w, int h, const std::vector<BYTE>& pixels) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 自顶向下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;  // 'BM'
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(pixels.size());

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || f == nullptr) {
        airtype::log("创建截图文件失败: %s", utf16_to_utf8(path).c_str());
        return false;
    }
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
    fwrite(pixels.data(), 1, pixels.size(), f);
    fclose(f);
    return true;
}

/// 抓整块虚拟屏幕（含所有显示器），用于核对真实落字结果。
bool SaveScreenBmp(const wchar_t* path) {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) {
        return false;
    }
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> pixels(static_cast<size_t>(w) * h * 4);
    const int got = GetDIBits(mem, bmp, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (got == 0) {
        airtype::log("GetDIBits 失败");
        return false;
    }
    return WriteBmp(path, w, h, pixels);
}

/// 把窗口内容抓成 BMP，用于肉眼核对写入结果。
bool SaveWindowBmp(HWND hwnd, const wchar_t* path) {
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) {
        return false;
    }
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        return false;
    }

    HDC screen = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    // PW_RENDERFULLCONTENT = 0x2，Win8.1+ 才能抓到硬件加速内容。
    const BOOL printed = PrintWindow(hwnd, mem, 0x2);
    SelectObject(mem, old);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 自顶向下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<BYTE> pixels(static_cast<size_t>(w) * h * 4);
    const int got = GetDIBits(mem, bmp, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);

    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, screen);

    if (got == 0) {
        airtype::log("GetDIBits 失败");
        return false;
    }

    const bool ok = WriteBmp(path, w, h, pixels);
    airtype::log("截图已保存: %s (PrintWindow=%d)", utf16_to_utf8(path).c_str(), printed);
    return ok;
}

LRESULT CALLBACK ProbeWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            // RichEdit（msftedit）是 TSF 文本存储，标准 EDIT 不是——
            // 用 EDIT 时 TSF 只会给窗口建一个空文档，写进去看不到。
            // 加载失败时退回 EDIT，至少还能验证 COM 链路。
            HWND edit = nullptr;
            if (HMODULE msftedit = LoadLibraryW(L"msftedit.dll")) {
                edit = CreateWindowExW(0, L"RICHEDIT50W", nullptr,
                                       WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                                           ES_AUTOVSCROLL | WS_VSCROLL,
                                       8, 8, 400, 200, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kEditId)),
                                       nullptr, nullptr);
                airtype::log("使用 RichEdit 控件（msftedit.dll %p）", msftedit);
            }
            if (edit == nullptr) {
                edit = CreateWindowExW(0, L"EDIT", nullptr,
                                       WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                                           ES_AUTOVSCROLL | WS_VSCROLL,
                                       8, 8, 400, 200, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kEditId)),
                                       nullptr, nullptr);
                airtype::log("回退到标准 EDIT 控件（不保证是 TSF 文本存储）");
            }
            g_probe.edit = edit;
            CreateWindowExW(0, L"BUTTON", L"TSF 写入",
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 8, 216, 120, 28, hwnd,
                            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kWriteId)), nullptr,
                            nullptr);
            return 0;
        }
        case WM_SIZE: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if (g_probe.edit) {
                SetWindowPos(g_probe.edit, nullptr, 8, 8, rc.right - 16, rc.bottom - 48,
                             SWP_NOZORDER);
            }
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == kWriteId) {
                DoWindowWrite(hwnd);
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool CreateProbeWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ProbeWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        airtype::log_hr("RegisterClassExW", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    g_probe.hwnd = CreateWindowExW(0, kClassName, L"AirType TSF 探针（写入目标）",
                                   WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, CW_USEDEFAULT,
                                   CW_USEDEFAULT, 460, 320, nullptr, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
    if (g_probe.hwnd == nullptr) {
        airtype::log_hr("CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    ShowWindow(g_probe.hwnd, SW_SHOW);
    UpdateWindow(g_probe.hwnd);
    SetForegroundWindow(g_probe.hwnd);
    SetFocus(g_probe.edit);
    airtype::log("已创建写入目标窗口 hwnd=%p edit=%p", g_probe.hwnd, g_probe.edit);
    return true;
}

// ---------------------------------------------------------------------------
// FFI 加载（可选）：验证 DLL 能被 C++ 侧加载并取到导出函数。
// ---------------------------------------------------------------------------

void LoadAndReportFfi(const std::wstring& path) {
    airtype::FfiBridge ffi;
    if (!ffi.Load(path)) {
        airtype::log("加载失败: %s", ffi.LastError().c_str());
        return;
    }
    airtype::log("airtype_ffi.dll 已加载");
    airtype::log("airtype_version = %s", ffi.Version());
    airtype::log("last_error = %s", ffi.ReadLastError());
}

// ---------------------------------------------------------------------------
// HiddenWindow 跨线程 marshal 验证。
//
// 场景复刻真实链路：一个 STA 线程创建隐藏窗口并跑消息循环，另一个线程
// （模拟 Rust tokio worker）只 PostMessage 投递 payload。
// 验收点：
//   1) handler 一定在创建窗口的 STA 线程上执行，而不是投递线程；
//   2) 投递 N 帧全部按序到达，文本与 final 标志不丢不变；
//   3) Destroy 之后 PostMessage 失败，且窗口类被注销（生命周期干净）。
// ---------------------------------------------------------------------------

struct MarshalTest {
    airtype::HiddenWindow hidden;
    DWORD sta_tid = 0;
    DWORD main_tid = 0;
    int expected = 0;
    std::vector<std::pair<std::wstring, bool>> received;
    std::mutex mtx;
    HANDLE ready = nullptr;
    HANDLE done = nullptr;
    HANDLE quit = nullptr;
    bool create_ok = false;
};

void MarshalStaThread(MarshalTest* t) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // std::thread 默认没有消息队列，PeekMessage 会把它建出来。
    MSG probe{};
    PeekMessageW(&probe, nullptr, 0, 0, PM_NOREMOVE);
    t->sta_tid = GetCurrentThreadId();

    t->create_ok = t->hidden.Create(L"AirType marshal 测试窗口", [t](airtype::AirTypePayload* p) {
        if (p == nullptr) {
            return;
        }
        bool signal = false;
        {
            std::lock_guard<std::mutex> lk(t->mtx);
            t->received.emplace_back(p->text, p->final);
            signal = t->expected > 0 && static_cast<int>(t->received.size()) == t->expected;
        }
        delete p;
        if (signal) {
            SetEvent(t->done);
        }
    });
    SetEvent(t->ready);

    while (true) {
        const DWORD w = MsgWaitForMultipleObjects(1, &t->quit, FALSE, INFINITE, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) {
            break;
        }
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    t->hidden.Destroy();
    // 泵掉 WM_NCDESTROY 之类的收尾消息。
    MSG msg{};
    for (int i = 0; i < 10 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
}

int RunMarshalTest() {
    MarshalTest t;
    t.main_tid = GetCurrentThreadId();
    t.ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    t.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    t.quit = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!t.ready || !t.done || !t.quit) {
        airtype::log("创建事件失败");
        return 1;
    }

    std::thread sta(MarshalStaThread, &t);
    if (WaitForSingleObject(t.ready, 5000) != WAIT_OBJECT_0) {
        airtype::log("STA 线程启动超时");
        SetEvent(t.quit);
        sta.join();
        return 1;
    }

    airtype::log("STA 线程 tid=%lu，主线程 tid=%lu", t.sta_tid, t.main_tid);
    if (!t.create_ok) {
        airtype::log("隐藏窗口创建失败");
        SetEvent(t.quit);
        sta.join();
        return 1;
    }
    airtype::log("隐藏窗口已创建 hwnd=%p", t.hidden.Handle());

    // 从「另一个线程」投递 6 帧：partial 递增 + 1 个 final，模拟真实回调流。
    const std::pair<std::wstring, bool> frames[] = {
        {L"你", false}, {L"你好", false}, {L"你好，", false},
        {L"你好，世界", false}, {L"你好，世界！", false}, {L"你好，世界！", true},
    };
    t.expected = static_cast<int>(std::size(frames));
    for (const auto& f : frames) {
        auto* payload = new airtype::AirTypePayload{f.first, f.second};
        if (PostMessageW(t.hidden.Handle(),
                         f.second ? airtype::AirTypeMessages::Get().text
                                  : airtype::AirTypeMessages::Get().partial,
                         0, reinterpret_cast<LPARAM>(payload)) == 0) {
            airtype::log("PostMessage 失败，丢弃该帧");
            delete payload;
        }
    }

    const DWORD got = WaitForSingleObject(t.done, 5000);
    int received = 0;
    std::vector<std::pair<std::wstring, bool>> copy;
    {
        std::lock_guard<std::mutex> lk(t.mtx);
        copy = t.received;
        received = static_cast<int>(t.received.size());
    }
    airtype::log("收到 %d/%d 帧（等待 %s）", received, t.expected,
                 got == WAIT_OBJECT_0 ? "完成" : "超时");
    bool content_ok = got == WAIT_OBJECT_0 && received == t.expected;
    if (content_ok) {
        for (size_t i = 0; i < copy.size(); ++i) {
            if (copy[i] != frames[i]) {
                content_ok = false;
                airtype::log("第 %zu 帧不一致", i);
                break;
            }
        }
    }
    if (content_ok) {
        airtype::log("文本与 final 标志逐帧一致");
    }

    // 关闭窗口后再投递，应当失败（旧句柄已失效）。
    // 注意不能传 nullptr：PostMessageW(NULL, ...) 是广播语义，会返回成功。
    const HWND dead = t.hidden.Handle();
    SetEvent(t.quit);
    sta.join();
    const BOOL post_after = PostMessageW(
        dead, airtype::AirTypeMessages::Get().partial, 0,
        reinterpret_cast<LPARAM>(new airtype::AirTypePayload{L"x", false}));
    airtype::log("窗口销毁后 PostMessage 返回 %d（期望 0）", static_cast<int>(post_after));
    const bool lifecycle_ok = (post_after == 0) && !t.hidden.Valid();

    const bool ok = content_ok && lifecycle_ok;
    airtype::log("=== marshal 测试%s ===", ok ? "通过" : "未通过");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// DLL 自激活测试（不注册、不改系统）。
//
// 直接 LoadLibrary + DllGetClassObject 拿类工厂，创建 TsfTextService，手动
// Activate，验证：.def 导出可用、类工厂可创建实例、ITfTextInputProcessorEx
// 与 ITfThreadMgrEventSink 可 QI、Activate 里隐藏窗口创建 / AdviseSink /
// FFI 加载与启动整条链路无崩溃。
//
// 不验证「文字真的落进目标输入框」——那必须由系统注册后注入目标进程。
// ---------------------------------------------------------------------------

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

int RunDllSelfTest(const std::wstring& dll_path) {
    airtype::log("DLL 自激活测试: %s", utf16_to_utf8(dll_path).c_str());

    HMODULE mod = LoadLibraryW(dll_path.c_str());
    if (mod == nullptr) {
        airtype::log("LoadLibrary 失败: %lu", GetLastError());
        return 1;
    }
    auto* get_class_object =
        reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(mod, "DllGetClassObject"));
    if (get_class_object == nullptr) {
        airtype::log("GetProcAddress(DllGetClassObject) 失败：导出表缺 .def？");
        FreeLibrary(mod);
        return 1;
    }
    airtype::log("DllGetClassObject 已取到");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        airtype::log_hr("CoInitializeEx(STA)", hr);
        FreeLibrary(mod);
        return 1;
    }

    bool ok = true;
    CComPtr<IClassFactory> factory;
    hr = get_class_object(CLSID_AIRTYPE_TSF, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    airtype::log_hr("DllGetClassObject(IClassFactory)", hr);
    ok = ok && SUCCEEDED(hr) && factory;

    CComPtr<ITfTextInputProcessorEx> service;
    if (ok) {
        hr = factory->CreateInstance(nullptr, IID_ITfTextInputProcessorEx,
                                     reinterpret_cast<void**>(&service));
        airtype::log_hr("IClassFactory::CreateInstance", hr);
        ok = SUCCEEDED(hr) && service;
    }

    if (ok) {
        CComPtr<ITfThreadMgrEventSink> sink;
        hr = service->QueryInterface(IID_ITfThreadMgrEventSink, reinterpret_cast<void**>(&sink));
        airtype::log_hr("QI(ITfThreadMgrEventSink)", hr);
    }

    CComPtr<ITfThreadMgr> thread_mgr;
    if (ok) {
        hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                              reinterpret_cast<void**>(&thread_mgr));
        airtype::log_hr("CoCreateInstance(ITfThreadMgr)", hr);
        ok = SUCCEEDED(hr) && thread_mgr;
    }

    if (ok) {
        TfClientId client_id = 0;
        hr = thread_mgr->Activate(&client_id);
        airtype::log_hr("ITfThreadMgr::Activate", hr);
        ok = SUCCEEDED(hr);
        if (ok) {
            hr = service->ActivateEx(thread_mgr, client_id, 0);
            airtype::log_hr("ITfTextInputProcessorEx::ActivateEx", hr);
            ok = SUCCEEDED(hr);

            // 泵消息 3 秒：隐藏窗口的回调处理、FFI 启动都在这里发生。
            ULONGLONG start = GetTickCount64();
            MSG msg{};
            while (GetTickCount64() - start < 3000) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                Sleep(16);
            }

            airtype::log_hr("ITfTextInputProcessorEx::Deactivate", service->Deactivate());
            thread_mgr->Deactivate();
        }
    }

    service.Release();
    factory.Release();
    CoUninitialize();
    FreeLibrary(mod);
    airtype::log("=== DLL 自激活测试%s ===", ok ? "通过" : "未通过");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 真实落字辅助：把 AirType 切成当前桌面的活动输入处理器

void ClickAtCursor() {
    INPUT input[2]{};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, input, sizeof(INPUT));
}

void DescribeWindow(HWND hwnd, const char* tag) {
    if (hwnd == nullptr) {
        airtype::log("%s: 无", tag);
        return;
    }
    wchar_t cls[256]{};
    wchar_t title[256]{};
    GetClassNameW(hwnd, cls, 256);
    GetWindowTextW(hwnd, title, 256);
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    airtype::log("%s: hwnd=%p class=%s title='%s' pid=%lu tid=%lu", tag, hwnd,
                 utf16_to_utf8(cls).c_str(), utf16_to_utf8(title).c_str(), pid, tid);
}

int RunCursor() {
    POINT pt{};
    if (!GetCursorPos(&pt)) {
        airtype::log("GetCursorPos 失败: %lu", GetLastError());
        return 1;
    }
    airtype::log("=== 鼠标位置 x=%ld y=%ld ===", pt.x, pt.y);
    const HWND under = WindowFromPoint(pt);
    DescribeWindow(under, "鼠标下窗口");
    DescribeWindow(GetAncestor(under, GA_PARENT), "鼠标下窗口的父窗口");
    DescribeWindow(GetForegroundWindow(), "前台窗口");
    DescribeWindow(GetFocus(), "Win32 焦点窗口");
    return 0;
}

const char* ControlTypeName(LONG id) {
    switch (id) {
        case UIA_EditControlTypeId: return "Edit";
        case UIA_DocumentControlTypeId: return "Document";
        case UIA_TextControlTypeId: return "Text";
        case UIA_ButtonControlTypeId: return "Button";
        case UIA_ComboBoxControlTypeId: return "ComboBox";
        case UIA_ListControlTypeId: return "List";
        case UIA_HyperlinkControlTypeId: return "Hyperlink";
        default: return "其他";
    }
}

// 只读：用 UI Automation 看清鼠标下到底是可编辑控件还是别的什么东西，
// 避免盲目点击误触按钮。
int RunUia() {
    airtype::log("=== UI Automation：鼠标下的元素 ===");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        airtype::log_hr("CoInitializeEx(STA)", hr);
        return 1;
    }

    CComPtr<IUIAutomation> uia;
    hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                          reinterpret_cast<void**>(&uia));
    airtype::log_hr("CoCreateInstance(CLSID_CUIAutomation)", hr);
    if (FAILED(hr) || !uia) {
        CoUninitialize();
        return 1;
    }

    POINT pt{};
    GetCursorPos(&pt);
    CComPtr<IUIAutomationElement> el;
    hr = uia->ElementFromPoint(pt, &el);
    airtype::log_hr("ElementFromPoint", hr);
    if (FAILED(hr) || !el) {
        CoUninitialize();
        return 1;
    }

    CONTROLTYPEID type = 0;
    el->get_CurrentControlType(&type);
    BSTR name = nullptr;
    el->get_CurrentName(&name);
    BOOL focusable = FALSE;
    el->get_CurrentIsKeyboardFocusable(&focusable);
    BOOL focused = FALSE;
    el->get_CurrentHasKeyboardFocus(&focused);
    UIA_HWND native = nullptr;
    el->get_CurrentNativeWindowHandle(&native);

    airtype::log("控件类型: %s (%ld)", ControlTypeName(type), static_cast<LONG>(type));
    if (name != nullptr) {
        airtype::log("名称: %s", utf16_to_utf8(name).c_str());
    } else {
        airtype::log("名称: (无)");
    }
    airtype::log("可键盘聚焦: %s   已聚焦: %s   原生窗口: %p", focusable ? "是" : "否",
                 focused ? "是" : "否", native);
    airtype::log("是否 Edit/Document: %s",
                 (type == UIA_EditControlTypeId || type == UIA_DocumentControlTypeId) ? "是" : "否");

    // 读控件当前内容，作为「文字是否真的落进去」的硬证据。
    CComPtr<IUIAutomationValuePattern> value;
    if (SUCCEEDED(el->GetCurrentPattern(UIA_ValuePatternId,
                                        reinterpret_cast<IUnknown**>(&value))) &&
        value) {
        BSTR text = nullptr;
        if (SUCCEEDED(value->get_CurrentValue(&text)) && text != nullptr) {
            airtype::log("ValuePattern 内容: '%s'", utf16_to_utf8(text).c_str());
            SysFreeString(text);
        } else {
            airtype::log("ValuePattern 内容: (空)");
        }
    } else {
        CComPtr<IUIAutomationTextPattern> text_pattern;
        if (SUCCEEDED(el->GetCurrentPattern(UIA_TextPatternId,
                                            reinterpret_cast<IUnknown**>(&text_pattern))) &&
            text_pattern) {
            CComPtr<IUIAutomationTextRange> range;
            if (SUCCEEDED(text_pattern->get_DocumentRange(&range)) && range) {
                BSTR text = nullptr;
                if (SUCCEEDED(range->GetText(256, &text)) && text != nullptr) {
                    airtype::log("TextPattern 内容: '%s'", utf16_to_utf8(text).c_str());
                    SysFreeString(text);
                }
            }
        } else {
            airtype::log("控件内容: (不支持 Value/TextPattern)");
        }
    }

    SysFreeString(name);
    value.Release();
    CoUninitialize();
    return 0;
}


// 只读布局探测会话：在编辑会话内取插入符/选区的屏幕矩形。
//
// 用途：UIA 与 Win32 caret 都拿不到光标的宿主（典型微信/Qt），看它是否通过
// TSF 提供 layout。这是唯一的精确来源，所以单独做成探针命令，先验证再决定
// 要不要接进正式定位链路。
class CaretProbeSession : public ITfEditSession {
public:
    CaretProbeSession(ITfContext* ctx, TfClientId tid) : ctx_(ctx), tid_(tid) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_INVALIDARG;
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

    // ITfEditSession
    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        CComPtr<ITfContextView> view;
        const HRESULT view_hr = ctx_->GetActiveView(&view);

        TF_SELECTION sel{};
        ULONG fetched = 0;
        const HRESULT sel_hr = ctx_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched);

        airtype::log("  [会话] view_hr=0x%08X sel_hr=0x%08X fetched=%lu has_view=%d",
                     static_cast<unsigned>(view_hr), static_cast<unsigned>(sel_hr),
                     static_cast<unsigned long>(fetched), static_cast<int>(view != nullptr));

        if (SUCCEEDED(sel_hr) && fetched > 0 && sel.range != nullptr && view != nullptr) {
            CComPtr<ITfRange> selected;
            selected.Attach(sel.range);  // 接管所有权

            // ① 折叠成插入符
            CComPtr<ITfRange> caret;
            if (SUCCEEDED(selected->Clone(&caret)) &&
                SUCCEEDED(caret->Collapse(ec, TF_ANCHOR_END))) {
                RECT rc{};
                BOOL clipped = FALSE;
                const HRESULT hr = view->GetTextExt(tid_, caret, &rc, &clipped);
                airtype::log("  [会话] GetTextExt(折叠) -> 0x%08X rect=(%d,%d,%d,%d) w=%d h=%d "
                             "clipped=%d", static_cast<unsigned>(hr), rc.left, rc.top,
                             rc.right, rc.bottom, rc.right - rc.left, rc.bottom - rc.top,
                             static_cast<int>(clipped));
            } else {
                airtype::log("  [会话] Clone/Collapse 失败");
            }

            // ② 不折叠，直接用真实 range（有些 provider 只对非空 range 给坐标）
            RECT rc2{};
            BOOL clipped2 = FALSE;
            const HRESULT hr2 = view->GetTextExt(tid_, selected, &rc2, &clipped2);
            airtype::log("  [会话] GetTextExt(原始) -> 0x%08X rect=(%d,%d,%d,%d) w=%d h=%d "
                         "clipped=%d", static_cast<unsigned>(hr2), rc2.left, rc2.top,
                         rc2.right, rc2.bottom, rc2.right - rc2.left,
                         rc2.bottom - rc2.top, static_cast<int>(clipped2));
        }
        return S_OK;
    }

private:
    ~CaretProbeSession() = default;

    CComPtr<ITfContext> ctx_;
    TfClientId tid_;
    ULONG ref_ = 1;
};

// 验证当前焦点宿主的 TSF layout 可用性。异步优先（不阻塞宿主），再看同步。
int RunTsfExt() {
    airtype::log("=== TSF layout 探测（当前系统焦点宿主）===");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        airtype::log_hr("CoInitializeEx(STA)", hr);
        return 1;
    }

    CComPtr<ITfThreadMgr> thread_mgr;
    hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                          reinterpret_cast<void**>(&thread_mgr));
    if (FAILED(hr) || !thread_mgr) {
        airtype::log_hr("CoCreateInstance(ITfThreadMgr)", hr);
        CoUninitialize();
        return 1;
    }

    TfClientId client_id = 0;
    hr = thread_mgr->Activate(&client_id);
    if (FAILED(hr)) {
        airtype::log_hr("ITfThreadMgr::Activate", hr);
        CoUninitialize();
        return 1;
    }

    HWND fg = GetForegroundWindow();
    wchar_t title[256]{};
    if (fg != nullptr) {
        GetWindowTextW(fg, title, 256);
    }
    airtype::log("前台窗口: hwnd=%p title=%s", static_cast<void*>(fg),
                 utf16_to_utf8(title).c_str());

    CComPtr<ITfDocumentMgr> doc;
    hr = thread_mgr->GetFocus(&doc);
    airtype::log_hr("ITfThreadMgr::GetFocus", hr);
    if (FAILED(hr) || !doc) {
        airtype::log("拿不到焦点文档管理器（当前线程没有 TSF 焦点）");
        thread_mgr->Deactivate();
        CoUninitialize();
        return 1;
    }

    CComPtr<ITfContext> ctx;
    hr = doc->GetTop(&ctx);
    airtype::log_hr("ITfDocumentMgr::GetTop", hr);
    if (FAILED(hr) || !ctx) {
        thread_mgr->Deactivate();
        CoUninitialize();
        return 1;
    }

    // 先异步：不阻塞宿主 UI 线程，代价是部分宿主（Chromium）只在同步下就绪布局。
    airtype::log("--- ASYNC 会话 ---");
    {
        auto* session = new CaretProbeSession(ctx, client_id);
        HRESULT session_hr = E_FAIL;
        const HRESULT req =
            ctx->RequestEditSession(client_id, session, TF_ES_ASYNC | TF_ES_READ, &session_hr);
        airtype::log_hr("RequestEditSession(ASYNC)", req);
        airtype::log_hr("  排队结果", session_hr);
        session->Release();
    }
    // 泵消息，等异步回调投递回本线程。
    MSG msg{};
    for (int i = 0; i < 200; ++i) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }

    airtype::log("--- SYNC 会话 ---");
    {
        auto* session = new CaretProbeSession(ctx, client_id);
        HRESULT session_hr = E_FAIL;
        const HRESULT req =
            ctx->RequestEditSession(client_id, session, TF_ES_SYNC | TF_ES_READ, &session_hr);
        airtype::log_hr("RequestEditSession(SYNC)", req);
        airtype::log_hr("  DoEditSession", session_hr);
        session->Release();
    }

    thread_mgr->Deactivate();
    CoUninitialize();
    return 0;
}

int RunShotFg(const std::string& path_utf8) {
    const std::wstring path = utf8_to_utf16(path_utf8);
    airtype::log("=== 抓取整块屏幕 ===");
    return SaveScreenBmp(path.c_str()) ? 0 : 1;
}

// 自绘预览窗口：画一行文字 + 假光标，用于直观验证图标跟随光标定位。
static LRESULT CALLBACK StatusPreviewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        const wchar_t* text = L"你好，这是一次语音输入测试";
        RECT rc{};
        GetClientRect(hwnd, &rc);
        SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, 30, 40, text, static_cast<int>(wcslen(text)));
        // 假光标：紧跟文字末尾。用 GetTextExtent 算末尾 X。
        SIZE sz{};
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &sz);
        const int caret_x = 30 + sz.cx + 2;
        MoveToEx(dc, caret_x, 40, nullptr);
        LineTo(dc, caret_x, 40 + 22);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 状态图标外观 + 定位预览：自绘文字和光标，显示图标并抓屏。
// 只验证 StatusWindow 本身，不涉及 TSF 文本服务或 FFI。
int RunStatusPreview(const std::string& path_utf8, int level_milli) {
    airtype::log("=== 状态图标预览 ===");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        airtype::log_hr("CoInitializeEx(STA)", hr);
        return 1;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = StatusPreviewWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AirTypeStatusPreview";
    RegisterClassExW(&wc);

    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    HWND host = CreateWindowExW(0, wc.lpszClassName, L"状态图标预览",
                                WS_OVERLAPPEDWINDOW, sw / 2 - 280, sh / 2 - 120,
                                560, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(host, SW_SHOW);
    UpdateWindow(host);

    airtype::StatusWindow status;
    if (!status.Create()) {
        airtype::log("状态窗口创建失败");
        DestroyWindow(host);
        UnregisterClassW(wc.lpszClassName, GetModuleHandleW(nullptr));
        CoUninitialize();
        return 1;
    }

    // 计算假光标的屏幕坐标：和客户区绘制用同样的字体与起点。
    RECT caret{};
    {
        HDC dc = GetDC(host);
        const wchar_t* text = L"你好，这是一次语音输入测试";
        SIZE sz{};
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &sz);
        ReleaseDC(host, dc);
        POINT client{30 + sz.cx + 2, 40};
        ClientToScreen(host, &client);
        caret.left = caret.right = client.x;
        caret.top = client.y;
        caret.bottom = client.y + 22;
    }
    status.FollowCaret(caret);
    // 音量填充预览：--level 指定毫电平（0~1000），验证绿色从底部升起的高度。
    status.SetLevel(level_milli);

    Sleep(400);  // 让分层窗口完成合成
    const std::wstring path = utf8_to_utf16(path_utf8);
    const bool ok = SaveScreenBmp(path.c_str());

    status.Destroy();
    DestroyWindow(host);
    UnregisterClassW(wc.lpszClassName, GetModuleHandleW(nullptr));
    CoUninitialize();
    airtype::log("状态图标预览截图: %s", path_utf8.c_str());
    return ok ? 0 : 1;
}

// 诊断：直接打印「悬浮图标两道门禁」在当前真实状态下的判定结果。
//
// 为什么需要它：UpdateStatusPosition() 里那两个判据（本进程是否前台、
// AirType 是否为活动键盘 TIP）一旦误判就会直接 Hide()，表现为「切到 AirType
// 后图标完全不出现」。这两个函数是匿名命名空间里的静态函数，探针无法调用，
// 所以在这里等价复现一遍用于对照。
int RunGateDiagnostic() {
    airtype::log("=== 悬浮图标门禁诊断 ===");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        airtype::log_hr("CoInitializeEx(STA)", hr);
        return 1;
    }

    // 门禁①：本进程是否前台进程。
    HWND fg = GetForegroundWindow();
    DWORD fg_pid = 0;
    if (fg != nullptr) {
        GetWindowThreadProcessId(fg, &fg_pid);
    }
    const bool gate_fg = (fg != nullptr) && fg_pid != 0 && fg_pid == GetCurrentProcessId();
    wchar_t cls[256]{};
    wchar_t title[256]{};
    if (fg != nullptr) {
        GetClassNameW(fg, cls, 256);
        GetWindowTextW(fg, title, 256);
    }
    airtype::log("门禁① 前台进程: %s", gate_fg ? "通过" : "未通过 -> 会隐藏图标");
    airtype::log("  本进程 pid=%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    airtype::log("  前台 hwnd=%p pid=%lu class=%s title='%s'", static_cast<void*>(fg),
                 static_cast<unsigned long>(fg_pid), utf16_to_utf8(cls).c_str(),
                 utf16_to_utf8(title).c_str());

    // 门禁②：活动键盘 TIP 是否是 AirType。
    CComPtr<ITfInputProcessorProfileMgr> mgr;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(&mgr));
    if (FAILED(hr) || !mgr) {
        airtype::log_hr("门禁② CoCreateInstance", hr);
        airtype::log("  查询失败时函数返回 true（放行）");
        CoUninitialize();
        return 1;
    }
    TF_INPUTPROCESSORPROFILE profile{};
    hr = mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile);
    if (FAILED(hr)) {
        airtype::log_hr("门禁② GetActiveProfile", hr);
        airtype::log("  查询失败时函数返回 true（放行）");
        CoUninitialize();
        return 1;
    }
    const bool gate_active = profile.clsid == CLSID_AIRTYPE_TSF &&
                             profile.guidProfile == GUID_AIRTYPE_PROFILE;
    wchar_t clsid[64]{};
    wchar_t prof[64]{};
    StringFromGUID2(profile.clsid, clsid, 64);
    StringFromGUID2(profile.guidProfile, prof, 64);
    airtype::log("门禁② 活动键盘为 AirType: %s", gate_active ? "通过" : "未通过 -> 会隐藏图标");
    airtype::log("  实际 clsid=%s profile=%s lang=0x%04X flags=0x%08lX",
                 utf16_to_utf8(clsid).c_str(), utf16_to_utf8(prof).c_str(), profile.langid,
                 profile.dwFlags);

    // 结论：两个门禁都通过时才可能显示。
    airtype::log("=== 结论：%s ===",
                 (gate_fg && gate_active) ? "两道门禁均通过，图标应显示"
                                          : "存在未通过的门禁，图标被主动隐藏");

    mgr.Release();
    CoUninitialize();
    return (gate_fg && gate_active) ? 0 : 3;
}

int RunActivate(bool click) {
    airtype::log("=== 把 AirType 切成当前桌面的活动输入处理器 ===");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        airtype::log_hr("CoInitializeEx(STA)", hr);
        return 1;
    }

    if (click) {
        airtype::log("在鼠标当前位置点击左键，让目标输入框拿到焦点");
        ClickAtCursor();
        Sleep(400);
    }

    CComPtr<ITfInputProcessorProfileMgr> mgr;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(&mgr));
    airtype::log_hr("CoCreateInstance(ITfInputProcessorProfileMgr)", hr);
    if (FAILED(hr) || !mgr) {
        CoUninitialize();
        return 1;
    }

    // FORSESSION：对当前桌面所有线程生效，前台程序那个线程也会切过来。
    // DONTCARECURRENTINPUTLANGUAGE：当前输入法语言可能不是 zh-CN，带上否则会失败。
    const DWORD flags = TF_IPPMF_FORSESSION | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE;

    // 先记下当前是哪个输入处理器，便于测完切回去。
    {
        TF_INPUTPROCESSORPROFILE before{};
        const HRESULT before_hr = mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &before);
        if (SUCCEEDED(before_hr)) {
            wchar_t clsid[64]{};
            StringFromGUID2(before.clsid, clsid, 64);
            airtype::log("切换前活动键盘: lang=0x%04X clsid=%s hkl=%p", before.langid,
                         utf16_to_utf8(clsid).c_str(), reinterpret_cast<void*>(before.hkl));
        } else {
            airtype::log_hr("GetActiveProfile(切换前)", before_hr);
        }
    }

    hr = mgr->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, CLSID_AIRTYPE_TSF,
                              GUID_AIRTYPE_PROFILE, nullptr, flags);
    airtype::log_hr("ActivateProfile(FORSESSION, zh-CN)", hr);

    TF_INPUTPROCESSORPROFILE active{};
    const HRESULT active_hr = mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active);
    airtype::log_hr("GetActiveProfile(TIP_KEYBOARD)", active_hr);
    if (SUCCEEDED(active_hr)) {
        wchar_t clsid[64]{};
        wchar_t profile[64]{};
        StringFromGUID2(active.clsid, clsid, 64);
        StringFromGUID2(active.guidProfile, profile, 64);
        airtype::log("当前活动键盘配置: type=%lu lang=0x%04X clsid=%s profile=%s hkl=%p flags=0x%08lX",
                     active.dwProfileType, active.langid, utf16_to_utf8(clsid).c_str(),
                     utf16_to_utf8(profile).c_str(), reinterpret_cast<void*>(active.hkl),
                     active.dwFlags);
        airtype::log("AirType 是否已激活: %s", active.clsid == CLSID_AIRTYPE_TSF ? "是" : "否");
    }
    // 必须先释放 COM 接口再 CoUninitialize，否则析构在卸载之后进行会崩。
    mgr.Release();
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}

int RunDeactivate(DWORD flags) {
    airtype::log("=== 停用 AirType，让系统回退到默认键盘（flags=0x%08lX）===", flags);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        airtype::log_hr("CoInitializeEx(STA)", hr);
        return 1;
    }

    CComPtr<ITfInputProcessorProfileMgr> mgr;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(&mgr));
    airtype::log_hr("CoCreateInstance(ITfInputProcessorProfileMgr)", hr);
    if (FAILED(hr) || !mgr) {
        CoUninitialize();
        return 1;
    }

    hr = mgr->DeactivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, CLSID_AIRTYPE_TSF,
                                GUID_AIRTYPE_PROFILE, nullptr, flags);
    airtype::log_hr("DeactivateProfile(FORSESSION, zh-CN)", hr);

    TF_INPUTPROCESSORPROFILE active{};
    const HRESULT active_hr = mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active);
    airtype::log_hr("GetActiveProfile(TIP_KEYBOARD)", active_hr);
    if (SUCCEEDED(active_hr)) {
        wchar_t clsid[64]{};
        StringFromGUID2(active.clsid, clsid, 64);
        airtype::log("当前活动键盘: lang=0x%04X clsid=%s hkl=%p", active.langid,
                     utf16_to_utf8(clsid).c_str(), reinterpret_cast<void*>(active.hkl));
        airtype::log("是否已切回非 AirType: %s", active.clsid == CLSID_AIRTYPE_TSF ? "否" : "是");
    }
    mgr.Release();
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 安装器用：把 AirType 持久写入 / 移出当前用户的输入法列表。
//
// DllRegisterServer 只做机器级注册；要让它出现在任务栏和 Win+空格 切换列表，
// 还必须「启用」对应 profile。这里走官方接口
// ITfInputProcessorProfileMgr::ActivateProfile / DeactivateProfile，
// 等价于设置里的「添加键盘」。
//
// 注意别和上面的 --activate/--deactivate 混用：那两个用 FORSESSION 只做
// 运行期切换测试，不写用户配置。
// ---------------------------------------------------------------------------

// 枚举 zh-CN 下所有已注册 profile，返回目标 profile 是否带 TF_IPP_FLAG_ENABLED。
bool ReportProfiles(ITfInputProcessorProfileMgr* mgr) {
    CComPtr<IEnumTfInputProcessorProfiles> enum_profiles;
    HRESULT hr = mgr->EnumProfiles(0x0804, &enum_profiles);
    airtype::log_hr("EnumProfiles(zh-CN)", hr);
    if (FAILED(hr) || !enum_profiles) {
        return false;
    }

    bool found_enabled = false;
    for (;;) {
        TF_INPUTPROCESSORPROFILE profile{};
        ULONG fetched = 0;
        hr = enum_profiles->Next(1, &profile, &fetched);
        if (hr != S_OK || fetched == 0) {
            break;
        }
        wchar_t clsid[64]{};
        wchar_t guid[64]{};
        StringFromGUID2(profile.clsid, clsid, 64);
        StringFromGUID2(profile.guidProfile, guid, 64);
        const bool is_airtype =
            (profile.clsid == CLSID_AIRTYPE_TSF && profile.guidProfile == GUID_AIRTYPE_PROFILE);
        airtype::log("%s lang=0x%04X clsid=%s profile=%s enabled=%d active=%d",
                     is_airtype ? "[AirType]" : "          ", profile.langid,
                     utf16_to_utf8(clsid).c_str(), utf16_to_utf8(guid).c_str(),
                     (profile.dwFlags & TF_IPP_FLAG_ENABLED) ? 1 : 0,
                     (profile.dwFlags & TF_IPP_FLAG_ACTIVE) ? 1 : 0);
        if (is_airtype && (profile.dwFlags & TF_IPP_FLAG_ENABLED)) {
            found_enabled = true;
        }
    }
    return found_enabled;
}

int RunEnable() {
    airtype::log("=== 启用 AirType（写入当前用户输入法列表）===");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        airtype::log_hr("CoInitializeEx(STA)", hr);
        return 1;
    }

    CComPtr<ITfInputProcessorProfileMgr> mgr;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(&mgr));
    airtype::log_hr("CoCreateInstance(ITfInputProcessorProfileMgr)", hr);
    if (FAILED(hr) || !mgr) {
        CoUninitialize();
        return 1;
    }

    hr = mgr->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, CLSID_AIRTYPE_TSF,
                              GUID_AIRTYPE_PROFILE, nullptr, TF_IPPMF_ENABLEPROFILE);
    airtype::log_hr("ActivateProfile(ENABLEPROFILE)", hr);

    // 无论成败都枚举一次：从 dwFlags 判断系统当前对这个 profile 的看法。
    const bool enabled = ReportProfiles(mgr);
    mgr.Release();
    CoUninitialize();
    airtype::log("=== 启用%s ===", enabled ? "成功" : "未确认（见上方 profile 列表）");
    return enabled ? 0 : 1;
}

int RunDisable() {
    airtype::log("=== 停用 AirType（从当前用户输入法列表移除）===");
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        airtype::log_hr("CoInitializeEx(STA)", hr);
        return 1;
    }

    CComPtr<ITfInputProcessorProfileMgr> mgr;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(&mgr));
    airtype::log_hr("CoCreateInstance(ITfInputProcessorProfileMgr)", hr);
    if (FAILED(hr) || !mgr) {
        CoUninitialize();
        return 1;
    }

    hr = mgr->DeactivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, CLSID_AIRTYPE_TSF,
                                GUID_AIRTYPE_PROFILE, nullptr, TF_IPPMF_DISABLEPROFILE);
    airtype::log_hr("DeactivateProfile(DISABLEPROFILE)", hr);

    const bool cleared = !ReportProfiles(mgr);
    mgr.Release();
    CoUninitialize();
    airtype::log("=== 停用%s ===", cleared ? "成功" : "未确认（见上方 profile 列表）");
    return cleared ? 0 : 1;
}

void usage(const char* exe) {
    printf("用法: %s [--write <utf8 text>] [--window] [--shot <out.bmp>] [--ffi <airtype_ffi.dll>]\n",
           exe);
    printf("      %s --marshal    验证 HiddenWindow 跨线程投递与生命周期\n", exe);
    printf("      %s --dllselftest <airtype_tsf.dll>   直接激活文本服务（不注册）\n", exe);
    printf("      %s --activate [--click]  把 AirType 切进当前桌面（FORSESSION）\n", exe);
    printf("      %s --deactivate [--flags <n>]  停用 AirType，让系统回退默认键盘\n", exe);
    printf("      %s --enable      启用 AirType（写入当前用户输入法列表，安装器用）\n", exe);
    printf("      %s --disable     停用 AirType（从当前用户输入法列表移除，卸载器用）\n", exe);
    printf("      %s --cursor    报告鼠标位置与其下的窗口\n", exe);
    printf("      %s --uia       用 UI Automation 报告鼠标下的控件类型（只读）\n", exe);
    printf("      %s --shotfg <out.bmp>  抓整块屏幕，用于核对落字结果\n", exe);
    printf("      %s --status-preview <out.bmp> [--level 0~1000]  预览悬浮状态图标并抓屏\n", exe);
}

}  // namespace

int main(int argc, char** argv) {
    std::string write_text;
    std::string ffi_path;
    std::string shot_path;
    std::string preset_text;
    bool use_self_window = false;
    bool marshal_test = false;
    std::string dll_selftest;
    bool activate = false;
    bool deactivate = false;
    bool enable_profile = false;
    bool disable_profile = false;
    bool gate_diag = false;
    bool click = false;
    // DeactivateProfile 不接受 TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE（会 E_INVALIDARG）。
    DWORD deactivate_flags = 0;
    bool cursor = false;
    bool uia = false;
    bool tsf_ext = false;
    std::string shotfg_path;
    std::string status_preview_path;
    int status_preview_level = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--write") == 0 && i + 1 < argc) {
            write_text = argv[++i];
        } else if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) {
            preset_text = argv[++i];
        } else if (strcmp(argv[i], "--window") == 0) {
            use_self_window = true;
        } else if (strcmp(argv[i], "--marshal") == 0) {
            marshal_test = true;
        } else if (strcmp(argv[i], "--gate-diag") == 0) {
            gate_diag = true;
        } else if (strcmp(argv[i], "--activate") == 0) {
            activate = true;
        } else if (strcmp(argv[i], "--deactivate") == 0) {
            deactivate = true;
        } else if (strcmp(argv[i], "--enable") == 0) {
            enable_profile = true;
        } else if (strcmp(argv[i], "--disable") == 0) {
            disable_profile = true;
        } else if (strcmp(argv[i], "--flags") == 0 && i + 1 < argc) {
            deactivate_flags = static_cast<DWORD>(strtoul(argv[++i], nullptr, 0));
        } else if (strcmp(argv[i], "--click") == 0) {
            click = true;
        } else if (strcmp(argv[i], "--cursor") == 0) {
            cursor = true;
        } else if (strcmp(argv[i], "--uia") == 0) {
            uia = true;
        } else if (strcmp(argv[i], "--tsf-ext") == 0) {
            tsf_ext = true;
        } else if (strcmp(argv[i], "--shotfg") == 0 && i + 1 < argc) {
            shotfg_path = argv[++i];
        } else if (strcmp(argv[i], "--status-preview") == 0 && i + 1 < argc) {
            status_preview_path = argv[++i];
        } else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            status_preview_level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dllselftest") == 0 && i + 1 < argc) {
            dll_selftest = argv[++i];
        } else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            shot_path = argv[++i];
        } else if (strcmp(argv[i], "--ffi") == 0 && i + 1 < argc) {
            ffi_path = argv[++i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (cursor) {
        return RunCursor();
    }
    if (uia) {
        return RunUia();
    }
    if (tsf_ext) {
        return RunTsfExt();
    }
    if (!shotfg_path.empty()) {
        return RunShotFg(shotfg_path);
    }
    if (!status_preview_path.empty()) {
        return RunStatusPreview(status_preview_path, status_preview_level);
    }
    if (marshal_test) {
        airtype::log("=== AirType HiddenWindow marshal 测试 ===");
        return RunMarshalTest();
    }
    if (!dll_selftest.empty()) {
        airtype::log("=== AirType TSF DLL 自激活测试 ===");
        return RunDllSelfTest(utf8_to_utf16(dll_selftest));
    }
    if (enable_profile) {
        return RunEnable();
    }
    if (disable_profile) {
        return RunDisable();
    }
    if (gate_diag) {
        return RunGateDiagnostic();
    }
    if (activate) {
        return RunActivate(click);
    }
    if (deactivate) {
        return RunDeactivate(deactivate_flags);
    }

    airtype::log("=== AirType TSF M1 探针 ===");
    airtype::log("模式: %s", use_self_window ? "自建窗口" : "系统焦点");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        airtype::log_hr("CoInitializeEx(STA)", hr);
        return 1;
    }

    if (!ffi_path.empty()) {
        LoadAndReportFfi(utf8_to_utf16(ffi_path));
    }

    // --- TSF 链路：先激活线程管理器，再让窗口拿焦点 ---
    // Activate 必须早于窗口创建/聚焦，否则 TSF 还没开始跟踪本线程的焦点，
    // GetFocus 会返回 S_FALSE（没有焦点文档）。
    CComPtr<ITfThreadMgr> thread_mgr;
    hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                          reinterpret_cast<void**>(&thread_mgr));
    if (FAILED(hr)) {
        airtype::log_hr("CoCreateInstance(ITfThreadMgr)", hr);
        CoUninitialize();
        return 1;
    }
    airtype::log("ITfThreadMgr 已创建");

    TfClientId client_id = 0;
    hr = thread_mgr->Activate(&client_id);
    if (FAILED(hr)) {
        airtype::log_hr("ITfThreadMgr::Activate", hr);
        CoUninitialize();
        return 1;
    }
    airtype::log("Activate 成功 client_id=%lu", client_id);

    if (use_self_window) {
        if (!CreateProbeWindow()) {
            CoUninitialize();
            return 1;
        }
        // 让窗口完成第一次绘制并拿到焦点。
        MSG msg{};
        for (int i = 0; i < 20 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ForceForeground(g_probe.hwnd);
        SetFocus(g_probe.edit);
        if (!preset_text.empty()) {
            SetWindowTextW(g_probe.edit, utf8_to_utf16(preset_text).c_str());
            airtype::log("已预置控件文本: %s", preset_text.c_str());
        }
        g_probe.pending_write = write_text;
    }

    // 自建窗口模式下焦点是异步到位的：轮询 GetFocus，期间持续切前台 + 泵消息。
    CComPtr<ITfDocumentMgr> doc_mgr;
    for (int attempt = 0; attempt < 100 && doc_mgr == nullptr; ++attempt) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (use_self_window && attempt % 5 == 0) {
            ForceForeground(g_probe.hwnd);
            SetFocus(g_probe.edit);
        }
        HRESULT focus_hr = thread_mgr->GetFocus(&doc_mgr);
        airtype::log_hr("ITfThreadMgr::GetFocus", focus_hr);
        if (FAILED(focus_hr) || doc_mgr == nullptr) {
            doc_mgr.Release();
            Sleep(20);
        }
    }
    if (doc_mgr == nullptr) {
        airtype::log("拿不到焦点文档管理器：当前线程没有 TSF 焦点");
        CoUninitialize();
        return 1;
    }
    {
        HWND fg = GetForegroundWindow();
        wchar_t title[256]{};
        if (fg) {
            GetWindowTextW(fg, title, 256);
        }
        airtype::log("前台窗口: hwnd=%p title=%s", fg, utf16_to_utf8(title).c_str());
        airtype::log("活动窗口: %p  Win32焦点: %p  控件: %p", GetActiveWindow(), GetFocus(),
                     g_probe.edit);
        airtype::log("焦点文档管理器: %p", doc_mgr.p);
    }

    CComPtr<ITfContext> context;
    hr = doc_mgr->GetTop(&context);
    airtype::log_hr("ITfDocumentMgr::GetTop", hr);
    if (FAILED(hr) || context == nullptr) {
        airtype::log("拿不到顶层 context");
        CoUninitialize();
        return 1;
    }
    airtype::log("已拿到 ITfContext");

    // 空跑一次编辑会话，确认 RequestEditSession 能成功。
    {
        auto* session = new InsertEditSession(context, L"", false, false, false);
        HRESULT sync_hr = S_OK;
        hr = context->RequestEditSession(client_id, session, TF_ES_SYNC | TF_ES_READWRITE, &sync_hr);
        airtype::log_hr("RequestEditSession(空写)", hr);
        airtype::log_hr("  DoEditSession 结果", sync_hr);
        session->Release();
    }

    // 整文档转储：判断这个 context 是不是控件本身（预置文本是否在里面）。
    if (use_self_window && !preset_text.empty()) {
        auto* session = new InsertEditSession(context, L"", false, false, true);
        HRESULT sync_hr = S_OK;
        hr = context->RequestEditSession(client_id, session, TF_ES_SYNC | TF_ES_READWRITE, &sync_hr);
        airtype::log_hr("RequestEditSession(转储)", hr);
        session->Release();
    }

    if (!write_text.empty()) {
        std::wstring wide = utf8_to_utf16(write_text);
        auto* session = new InsertEditSession(context, wide, true, true, false);
        HRESULT sync_hr = S_OK;
        hr = context->RequestEditSession(client_id, session, TF_ES_SYNC | TF_ES_READWRITE, &sync_hr);
        airtype::log_hr("RequestEditSession(写入)", hr);
        airtype::log_hr("  DoEditSession 结果", sync_hr);
        session->Release();

        // 回读控件内容，确认写入真的落到了编辑框，而不只是返回了 S_OK。
        // 用 WM_GETTEXTLENGTH + EM_GETTEXTEX：RichEdit 对 WM_GETTEXT 的支持不稳定。
        if (use_self_window && g_probe.edit != nullptr) {
            const LRESULT len = SendMessageW(g_probe.edit, WM_GETTEXTLENGTH, 0, 0);
            wchar_t back[1024]{};
            GETTEXTEX gt{};
            gt.cb = sizeof(back);
            gt.flags = GT_DEFAULT;
            gt.codepage = 1200;  // CP_UTF16
            SendMessageW(g_probe.edit, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&gt),
                         reinterpret_cast<LPARAM>(back));
            const bool same = wide == back;
            airtype::log("控件文本长度: %ld", len);
            airtype::log("回读控件: %s", utf16_to_utf8(back).c_str());
            airtype::log("回读一致: %s", same ? "是" : "否");
        }
    }

    thread_mgr->Deactivate();
    airtype::log("=== 探针结束 ===");

    if (use_self_window && !shot_path.empty()) {
        SaveWindowBmp(g_probe.hwnd, utf8_to_utf16(shot_path).c_str());
    }

    if (use_self_window) {
        // 窗口模式：跑一小段消息循环，方便人工核对写入结果。
        airtype::log("进入消息循环 5 秒，可在窗口里核对写入结果；也可点「TSF 写入」按钮对照。");
        ULONGLONG start = GetTickCount64();
        MSG msg{};
        while (GetTickCount64() - start < 5000) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(16);
        }
        DestroyWindow(g_probe.hwnd);
    }

    CoUninitialize();
    return 0;
}
