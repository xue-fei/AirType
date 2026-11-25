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

// 胶囊尺寸（逻辑像素，96 DPI）
constexpr int kBubbleW = 132;
constexpr int kBubbleH = 36;
constexpr int kCaretGap = 10;   // 图标与插入符的间距

// 画圆角矩形路径
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

    // 胶囊背景：深蓝半透明 + 浅色描边
    GraphicsPath bubble;
    RoundedRect(bubble, 1, 1, width_ - 2, height_ - 2, 10);
    SolidBrush fill(Color(235, 24, 95, 165));
    g.FillPath(&fill, &bubble);
    Pen border(Color(255, 150, 200, 240), 1);
    g.DrawPath(&border, &bubble);

    // 麦克风图标（白色），位于左侧。
    // my 让图形（帽头 y=my..my+14、托架到 my+19、底座 my+23）的整体中心
    // 落在胶囊中线 18 附近：my=7 时图形占 7..30，中心 18.5，与文字基线一致
    // （早先 my=9 时占 9..32、中心 20.5，视觉上明显偏下）。
    const int mx = 7;
    const int my = 7;
    SolidBrush white(Color(255, 255, 255, 255));
    Pen whitePen(Color(255, 255, 255, 255), 1.8f);
    whitePen.SetStartCap(LineCapRound);
    whitePen.SetEndCap(LineCapRound);

    // 帽头：圆角矩形胶囊
    GraphicsPath capsule;
    RoundedRect(capsule, mx + 5, my, 10, 14, 5);
    g.FillPath(&white, &capsule);
    // U 形托架（下半弧）
    GraphicsPath cradle;
    cradle.AddArc(mx + 2, my + 6, 16, 13, 0, 180);
    g.DrawPath(&whitePen, &cradle);
    // 竖杆 + 底座
    g.DrawLine(&whitePen, mx + 10, my + 19, mx + 10, my + 23);
    g.DrawLine(&whitePen, mx + 5, my + 23, mx + 15, my + 23);

    // 文字
    const wchar_t* text = L"AirType";
    FontFamily family(L"Microsoft YaHei");
    Font font(&family, 11, FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentNear);
    sf.SetLineAlignment(StringAlignmentCenter);
    RectF textRect(34, 0, width_ - 40, height_);
    g.DrawString(text, -1, &font, textRect, &sf, &white);

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
    int x = caret.right + kCaretGap;
    // 垂直居中于 caret 行。部分应用（Chromium/自绘控件）对折叠 range 的
    // GetTextExt 会返回高度≈0 的退化矩形，直接套公式会让图标中心比光标低
    // 几乎整个图标高度。给行高设下限（约一个 11pt 字的身位），保证对齐合理。
    int caret_h = caret.bottom - caret.top;
    if (caret_h < 18) {
        caret_h = 18;
    }
    int y = caret.top + (caret_h - height_) / 2;

    // 越过屏幕右边界时翻到插入符左侧
    const int screen_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int screen_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    if (x + width_ > screen_x + screen_w) {
        x = caret.left - kCaretGap - width_;
    }
    if (x < screen_x) {
        x = screen_x;
    }
    if (y < GetSystemMetrics(SM_YVIRTUALSCREEN)) {
        y = GetSystemMetrics(SM_YVIRTUALSCREEN);
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

void StatusWindow::Hide() {
    if (hwnd_ != nullptr) {
        ShowWindow(hwnd_, SW_HIDE);
    }
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
