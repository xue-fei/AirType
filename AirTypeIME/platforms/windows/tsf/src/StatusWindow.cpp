#include "StatusWindow.h"

#include <atomic>
#include <cwchar>
#include <mutex>

// WIN32_LEAN_AND_MEAN 下 windows.h 不引入这些 COM 基础类型，
// 而 gdiplus.h 直接用到 IStream / PROPID / GUID / byte，必须显式包含。
#include <objidl.h>
#include <gdiplus.h>

#include "Log.h"
#include "TsfTypes.h"

#pragma comment(lib, "gdiplus.lib")

namespace airtype {

namespace {

// GDI+ 进程级初始化。TSF DLL 常驻目标进程，进程退出时系统回收，
// 不在 DLL_PROCESS_DETACH 里 Shutdown（那时其他线程可能仍在使用 GDI+）。
std::once_flag g_gdiplus_once;
ULONG_PTR g_gdiplus_token = 0;

// 进程级" suppressed"标志：收到卸载广播后，本进程不再重建状态图标。
// TSF 可能在卸载广播之后又对本进程调用 Activate，这里要挡住，避免图标复活。
std::atomic<bool> g_shutdown_received{false};

void EnsureGdiplus() {
    std::call_once(g_gdiplus_once, [] {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, nullptr);
    });
}

constexpr wchar_t kClassPrefix[] = L"AirTypeTsfStatus-";

// 圆形图标尺寸（逻辑像素，96 DPI）：直径即 kStatusIconSize（见头文件）。
constexpr int kBubbleW = kStatusIconSize;
constexpr int kBubbleH = kStatusIconSize;
constexpr int kBelowGap = 6;    // 图标与插入符所在行的垂直间距

// 画圆角矩形路径（麦克风帽头用）
void RoundedRect(Gdiplus::GraphicsPath& path, int x, int y, int w, int h, int r) {
    const int d = r * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

}  // namespace

StatusWindow::~StatusWindow() {
    Destroy();
}

StatusWindow* StatusWindow::From(HWND hwnd) {
    return reinterpret_cast<StatusWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

bool StatusWindow::Create() {
    if (g_shutdown_received.load(std::memory_order_acquire)) {
        return false;  // 卸载广播已到达，本进程不再显示图标
    }
    if (hwnd_ != nullptr) {
        return true;
    }
    EnsureGdiplus();

    static unsigned counter = 0;
    std::swprintf(class_name_, 64, L"%s%u-%p", kClassPrefix, counter++, this);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = class_name_;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        airtype::log_hr("RegisterClassExW(状态窗口)", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    // 分层 + 不激活 + 工具窗口 + 鼠标穿透；WS_POPUP 无边框。
    const DWORD ex = WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT;
    width_ = kBubbleW;
    height_ = kBubbleH;
    HWND created = CreateWindowExW(ex, class_name_, L"AirType 状态", WS_POPUP,
                                   0, 0, width_, height_, nullptr, nullptr,
                                   GetModuleHandleW(nullptr), this);
    if (created == nullptr) {
        airtype::log_hr("CreateWindowExW(状态窗口)", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    hwnd_ = created;
    SetWindowLongPtrW(created, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    Render();
    // 置顶但不激活，且【创建时不显示】。定位成功前不能 SHOWWINDOW，
    // 否则光标矩形拿不到时图标会永远钉在 (0,0) 左上角。
    // 真正显示由 FollowCaret 在拿到有效矩形后负责。
    SetWindowPos(created, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
    airtype::log("状态图标已创建 %dx%d（等待定位后显示）", width_, height_);
    return true;
}

void StatusWindow::Destroy() {
    // 先置失效：排队中的异步定位回调可能还持有本对象的裸指针。
    alive.store(false, std::memory_order_release);
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (class_name_[0] != L'\0') {
        UnregisterClassW(class_name_, GetModuleHandleW(nullptr));
        class_name_[0] = L'\0';
    }
}

void StatusWindow::Render() {
    if (hwnd_ == nullptr) {
        return;
    }
    using namespace Gdiplus;

    Bitmap bitmap(width_, height_, PixelFormat32bppARGB);
    Graphics g(&bitmap);
    g.Clear(Color(0, 0, 0, 0));                 // 全透明底
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    // 圆形背景：深蓝半透明 + 浅色描边。直径铺满窗口，留 1px 给描边。
    SolidBrush fill(Color(235, 24, 95, 165));
    g.FillEllipse(&fill, 1, 1, width_ - 2, height_ - 2);

    // 音量填充：绿色从圆形底部向上升，高度 = 电平比例。
    // 用 SetClip 把填充限制在圆内——矩形画满整个窗口宽度，圆外部分被裁掉，
    // 视觉上就是"圆里灌了绿色液体"。描边在填充之后画，边界才干净。
    if (level_milli_ > 0) {
        GraphicsPath clip;
        clip.AddEllipse(1, 1, width_ - 2, height_ - 2);
        g.SetClip(&clip);
        const int inner_h = height_ - 2;
        const int fill_h = (inner_h * level_milli_) / 1000;
        if (fill_h > 0) {
            SolidBrush green(Color(232, 34, 197, 94));  // emerald-500
            g.FillRectangle(&green, 0, 1 + inner_h - fill_h, width_, fill_h);
        }
        g.ResetClip();
    }

    Pen border(Color(255, 150, 200, 240), 1.2f);
    g.DrawEllipse(&border, 1, 1, width_ - 2, height_ - 2);

    // 麦克风图标（白色），在整个圆形里居中。
    // 以 40px 直径、圆心 (20,20) 为基准设计，再按实际尺寸做等比缩放，
    // 保证将来改 kStatusIconSize 时图形不会跑偏。
    const float s = static_cast<float>(width_) / 40.0f;
    const float cx = static_cast<float>(width_) / 2.0f;  // 圆心 x
    SolidBrush white(Color(255, 255, 255, 255));
    Pen whitePen(Color(255, 255, 255, 255), 2.0f * s);
    whitePen.SetStartCap(LineCapRound);
    whitePen.SetEndCap(LineCapRound);

    // 帽头：圆角胶囊（宽 8、高 14，水平居中，顶端留 8px）
    GraphicsPath capsule;
    RoundedRect(capsule, static_cast<int>(cx - 4 * s), static_cast<int>(8 * s),
                static_cast<int>(8 * s), static_cast<int>(14 * s), static_cast<int>(4 * s));
    g.FillPath(&white, &capsule);
    // U 形托架：下半圆弧，从水平中线向下包住帽头
    GraphicsPath cradle;
    cradle.AddArc(cx - 8 * s, 14 * s, 16 * s, 13 * s, 0.0f, 180.0f);
    g.DrawPath(&whitePen, &cradle);
    // 竖杆 + 底座
    g.DrawLine(&whitePen, cx, 27 * s, cx, 31 * s);
    g.DrawLine(&whitePen, cx - 4 * s, 31 * s, cx + 4 * s, 31 * s);

    // 把带 alpha 的位图提交给分层窗口
    HBITMAP hbmp = nullptr;
    bitmap.GetHBITMAP(Color(0, 0, 0, 0), &hbmp);
    HDC screen = GetDC(nullptr);
    HDC memdc = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(memdc, hbmp);

    SIZE size{width_, height_};
    POINT src{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd_, screen, nullptr, &size, memdc, &src, 0, &blend, ULW_ALPHA);

    SelectObject(memdc, old);
    DeleteObject(hbmp);
    DeleteDC(memdc);
    ReleaseDC(nullptr, screen);
}

void StatusWindow::FollowCaret(const RECT& caret) {
    if (hwnd_ == nullptr) {
        return;
    }
    // 落点：圆形图标的**水平中心对准光标**、整体位于当前行下方 6px。
    // 圆形小图标居中比左对齐更符合"光标正下方"的直觉。
    //
    // 部分应用（Chromium/自绘控件）对折叠 range 会返回高度≈0 的退化矩形，
    // 此时按一个 11pt 字的身位估算行底，避免图标压在文字上。
    const int caret_w = caret.right - caret.left;
    const int caret_center_x = caret.left + (caret_w > 0 ? caret_w / 2 : 0);
    int x = caret_center_x - width_ / 2;
    const int caret_h = caret.bottom - caret.top;
    const int line_bottom = caret.top + (caret_h >= 18 ? caret_h : 18);
    int y = line_bottom + kBelowGap;

    const int screen_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int screen_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int screen_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // 右侧溢出：整体左移进屏幕内。不再翻到插入符左侧——那样图标离光标太远。
    if (x + width_ > screen_x + screen_w) {
        x = screen_x + screen_w - width_;
    }
    if (x < screen_x) {
        x = screen_x;
    }
    // 下方溢出：翻到插入符上方，保持同样的 6px 间距。
    if (y + height_ > screen_y + screen_h) {
        const int above = caret.top - kBelowGap - height_;
        y = above >= screen_y ? above : screen_y + screen_h - height_;
    }
    if (y < screen_y) {
        y = screen_y;
    }

    // 位置没变就别动窗口。跟随轮询现在跑到 25Hz，若每拍都 SetWindowPos，
    // 光标静止时也在持续触发分层窗口位置更新与桌面合成，纯属白烧。
    if (x == last_x_ && y == last_y_) {
        return;
    }
    last_x_ = x;
    last_y_ = y;

    SetWindowPos(hwnd_, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void StatusWindow::SetLevel(int milli) {
    if (milli < 0) {
        milli = 0;
    }
    if (milli > 1000) {
        milli = 1000;
    }
    if (milli == level_milli_) {
        return;  // 数值没变不重绘：静止时 50fps 的空转没必要
    }
    level_milli_ = milli;
    if (hwnd_ != nullptr) {
        // Render 内的 UpdateLayeredWindow 用 pptDst = nullptr，只换内容不动位置，
        // 所以不会和 FollowCaret 的定位互相干扰。
        Render();
    }
}

void StatusWindow::Hide() {
    if (hwnd_ != nullptr) {
        ShowWindow(hwnd_, SW_HIDE);
    }
    // 必须清掉坐标去重缓存：否则 Hide 后以相同坐标 FollowCaret 会在去重处
    // 直接 return，不带 SWP_SHOWWINDOW —— 图标永远不再出现。
    // 这表现为「切换输入法后图标消失/复现状态错乱」。
    last_x_ = INT_MIN;
    last_y_ = INT_MIN;
}

LRESULT CALLBACK StatusWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }

    // 卸载广播：收到后本进程自毁图标，并置位抑制标志防止 TSF 后续 Activate 重建。
    // 用进程级一次性标志而非逐窗口，因为一个进程可能有多个 StatusWindow 实例。
    static const UINT shutdown_msg = AirTypeMessages::Get().shutdown;
    if (shutdown_msg != 0 && msg == shutdown_msg) {
        g_shutdown_received.store(true, std::memory_order_release);
        auto* self = From(hwnd);
        if (self != nullptr) {
            self->Destroy();
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
