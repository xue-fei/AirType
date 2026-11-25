#pragma once

// 悬浮状态图标：跟随文本插入符的可见分层窗口。
//
// 与 HiddenWindow 的区别：HiddenWindow 是 HWND_MESSAGE 消息窗口（不可见，
// 只做线程 marshal）；StatusWindow 是真正绘制在屏幕上的 WS_EX_LAYERED popup，
// 鼠标穿透、不抢焦点、置顶。所有方法只能在创建它的 STA 线程调用。

#include <atomic>
#include <climits>
#include <windows.h>

namespace airtype {

class StatusWindow {
public:
    StatusWindow() = default;
    ~StatusWindow();

    StatusWindow(const StatusWindow&) = delete;
    StatusWindow& operator=(const StatusWindow&) = delete;

    /// 创建状态图标（STA 线程），但先隐藏：只有 FollowCaret 定位成功后才显示，
    /// 避免定位失败时窗口钉在屏幕左上角 (0,0)。
    bool Create();
    void Destroy();

    /// 把图标放到插入符右侧：caret_right + 间距，垂直居中于 caret 行。
    /// rect 是 GetTextExt 返回的屏幕坐标。
    void FollowCaret(const RECT& caret);

    /// 焦点离开文本区时隐藏图标（窗口保留，下次定位成功再显示）。
    void Hide();

    bool Valid() const { return hwnd_ != nullptr; }

    /// 当前定位请求的代次。每次发起查询前递增；DoEditSession 执行时比对，
    /// 不一致说明这帧已被更新的请求取代，直接丢弃结果。
    /// 只在 STA 线程写、STA 线程读；用 atomic 是因为 Deactivate/析构可能从
    /// 其他线程观测状态。
    std::atomic<LONG> generation{0};

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static StatusWindow* From(HWND hwnd);

    /// 重绘分层窗口内容（圆角胶囊 + 麦克风 + 文字）。
    void Render();

    HWND hwnd_ = nullptr;
    wchar_t class_name_[64]{};
    int width_ = 0;
    int height_ = 0;

    /// 上次实际提交给 SetWindowPos 的坐标。位置没变就不重复调用——
    /// 高频轮询下避免无谓的窗口位置变更与桌面合成。
    int last_x_ = INT_MIN;
    int last_y_ = INT_MIN;
};

}  // namespace airtype
