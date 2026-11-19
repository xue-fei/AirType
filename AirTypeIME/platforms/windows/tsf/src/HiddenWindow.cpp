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

bool HiddenWindow::Create(const wchar_t* title, Handler handler) {
    if (hwnd_ != nullptr) {
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
    hwnd_ = CreateWindowExW(0, class_name_, title, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        airtype::log_hr("CreateWindowExW(隐藏窗口)", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    handler_ = std::move(handler);
    return true;
}

void HiddenWindow::Destroy() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
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

    if (msg == WM_AIRTYPE_TEXT || msg == WM_AIRTYPE_PARTIAL) {
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

    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace airtype
