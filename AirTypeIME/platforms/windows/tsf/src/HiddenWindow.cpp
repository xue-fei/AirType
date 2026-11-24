#include "HiddenWindow.h"

#include <cwchar>
#include <utility>

#include "Log.h"

namespace airtype {

namespace {
constexpr wchar_t kPrefix[] = L"AirTypeTsfHidden-";
}

HiddenWindow::~HiddenWindow() {
    // 正常路径由 Destroy() 关闭；析构兜底，避免泄漏窗口类。
    Destroy();
}

HiddenWindow* HiddenWindow::From(HWND hwnd) {
    return reinterpret_cast<HiddenWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

bool HiddenWindow::Create(const wchar_t* title, Handler handler, CommandHandler refresh) {
    if (hwnd_.load(std::memory_order_relaxed) != nullptr) {
        return true;
    }

    // 每个窗口用独立的窗口类，避免多实例互相覆盖。
    static unsigned counter = 0;
    std::swprintf(class_name_, 64, L"%s%u-%p", kPrefix, counter++, this);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = class_name_;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        airtype::log_hr("RegisterClassExW", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    // 先设 GWLP_USERDATA 再创建窗口，防止窗口过程在 CreateWindowEx 返回前触发。
    HWND created = CreateWindowExW(0, class_name_, title, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleW(nullptr), this);
    if (created == nullptr) {
        airtype::log_hr("CreateWindowExW(隐藏窗口)", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    hwnd_.store(created, std::memory_order_relaxed);
    SetWindowLongPtrW(created, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    handler_ = std::move(handler);
    refresh_ = std::move(refresh);
    return true;
}

void HiddenWindow::Destroy() {
    // 先原子取走再销毁：并发的 PostText 要么拿到旧句柄（PostMessage 返回 0），
    // 要么拿到 nullptr（直接丢弃），不会把消息广播出去。
    const HWND hwnd = hwnd_.exchange(nullptr, std::memory_order_relaxed);
    if (hwnd != nullptr) {
        // PostMessage 成功后 payload 所有权归消息队列；DestroyWindow 会直接丢弃
        // 尚未派发的消息，那些 AirTypePayload 就没人 delete 了。销毁前把本窗口
        // 队列里残留的自定义消息取出来释放。（Destroy 在创建的 STA 线程调用，
        // 与 WndProc 同线程，不会并发。）
    const UINT text_msg = AirTypeMessages::Get().text;
    const UINT partial_msg = AirTypeMessages::Get().partial;
    MSG msg;
    while (PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE)) {
        if (msg.message == text_msg || msg.message == partial_msg) {
            delete reinterpret_cast<AirTypePayload*>(msg.lParam);
        }
        // RefreshStatus 无 payload（lParam=0），无需释放，直接丢弃。
    }
    DestroyWindow(hwnd);
    }
    if (class_name_[0] != L'\0') {
        UnregisterClassW(class_name_, GetModuleHandleW(nullptr));
        class_name_[0] = L'\0';
    }
}

LRESULT CALLBACK HiddenWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // 建窗口时把 this 作为 lp 创建参数传进来。
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }

    HiddenWindow* self = From(hwnd);
    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    const UINT text_msg = AirTypeMessages::Get().text;
    const UINT partial_msg = AirTypeMessages::Get().partial;
    if (msg == text_msg || msg == partial_msg) {
        auto* payload = reinterpret_cast<AirTypePayload*>(lp);
        if (payload != nullptr) {
            // 所有权转移：处理完由 handler 负责 delete。
            if (self->handler_) {
                self->handler_(payload);
            } else {
                delete payload;
            }
        }
        return 0;
    }

    // 后台轮询线程投递的刷新命令：在 STA 线程回调，重新定位状态图标。
    if (msg == AirTypeMessages::Get().refreshStatus) {
        if (self->refresh_) {
            self->refresh_();
        }
        return 0;
    }

    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace airtype
