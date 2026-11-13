//! 文本注入。第一版两种模式：
//!
//! - `none`：只写日志，不碰剪贴板和按键（无头验证、FFI 外壳接管时使用）
//! - `clipboard_paste`（Windows）：保存剪贴板 → 写入文本 → 模拟 Ctrl+V → 恢复剪贴板
//!
//! 流式识别下用 `inject_replace(delete_chars, append)`：先退格撤掉上一版
//! 已上屏文本的尾巴，再粘贴本次增量。这是「覆盖式实时上屏」的代价所在——
//! 它依赖焦点窗口里的光标位置不动，且会反复占用剪贴板；正规解法是
//! TSF composition（预编辑串），需要 C++ COM 外壳，留待后续。
//!
//! macOS / Linux 后续在各自原生外壳里用 IMK / Fcitx5 的编辑会话写入，
//! 不需要走剪贴板；这里预留同一个 trait 即可。

use std::sync::Arc;
use airtype_core::config::InjectConfig;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum InjectError {
    #[error("clipboard: {0}")]
    Clipboard(String),
    #[error("injector config: {0}")]
    Config(String),
}

pub trait TextInjector: Send + Sync {
    fn name(&self) -> &str;
    fn inject(&self, text: &str) -> Result<(), InjectError>;
    /// 覆盖式注入：先退格 `delete_chars` 个字符撤掉上一版已上屏文本的尾巴，
    /// 再写入 `text`（**只是本次追加的增量**，不是累积全文）。
    ///
    /// 流式识别的中间结果会反复改写（「你」→「你好」→「你好吗」），
    /// 只能靠「撤销 + 追加」把文字铺进输入框。`delete_chars == 0` 时退化为
    /// `inject`，但**不恢复剪贴板**（高频调用下反复写剪贴板代价太大）。
    fn inject_replace(&self, delete_chars: usize, text: &str) -> Result<(), InjectError>;
}

pub fn build_injector(cfg: &InjectConfig) -> Result<Arc<dyn TextInjector>, InjectError> {
    match cfg.mode.as_str() {
        "none" | "" => Ok(Arc::new(NullInjector)),
        "clipboard_paste" => {
            #[cfg(target_os = "windows")]
            {
                Ok(Arc::new(ClipboardPasteInjector {
                    restore: cfg.restore_clipboard,
                    delay_ms: cfg.paste_delay_ms,
                }))
            }
            #[cfg(not(target_os = "windows"))]
            {
                tracing::warn!(
                    "clipboard_paste 目前仅实现 Windows，本次回退到 none；\
                     macOS/Linux 请在原生外壳里实现注入"
                );
                Ok(Arc::new(NullInjector))
            }
        }
        other => Err(InjectError::Config(format!(
            "unknown inject mode `{other}` (supported: none, clipboard_paste)"
        ))),
    }
}

pub struct NullInjector;

impl TextInjector for NullInjector {
    fn name(&self) -> &str {
        "none"
    }
    fn inject(&self, text: &str) -> Result<(), InjectError> {
        tracing::info!(target: "inject", "[INJECT:none] {text}");
        Ok(())
    }
    fn inject_replace(&self, delete_chars: usize, text: &str) -> Result<(), InjectError> {
        tracing::info!(
            target: "inject",
            "[INJECT:none] replace delete={delete_chars} append={text}"
        );
        Ok(())
    }
}

#[cfg(target_os = "windows")]
pub struct ClipboardPasteInjector {
    pub restore: bool,
    pub delay_ms: u64,
}

#[cfg(target_os = "windows")]
impl TextInjector for ClipboardPasteInjector {
    fn name(&self) -> &str {
        "clipboard_paste"
    }

    fn inject(&self, text: &str) -> Result<(), InjectError> {
        paste_via_clipboard(text, self.restore, self.delay_ms)?;
        tracing::info!(target: "inject", "[INJECT:clipboard_paste] 已写入 {} 字节", text.len());
        Ok(())
    }

    fn inject_replace(&self, delete_chars: usize, text: &str) -> Result<(), InjectError> {
        use std::time::Duration;

        if delete_chars == 0 && text.is_empty() {
            return Ok(());
        }
        // 1) 先退格撤掉上一版 partial 的尾巴。
        if delete_chars > 0 {
            std::thread::sleep(Duration::from_millis(self.delay_ms));
            // SAFETY: 只向当前焦点窗口发退格键，不接收指针。
            unsafe { send_backspaces(delete_chars) };
            std::thread::sleep(Duration::from_millis(self.delay_ms));
        }
        // 2) 只粘贴本次追加的增量。restore=false：高频 partial 下
        //    反复写剪贴板会明显拖慢，交给用户自己决定要不要保留原剪贴板。
        if !text.is_empty() {
            paste_via_clipboard(text, false, self.delay_ms)?;
            tracing::info!(
                target: "inject",
                "[INJECT:clipboard_paste] replace delete={delete_chars} 追加 {} 字节",
                text.len()
            );
        } else {
            tracing::info!(
                target: "inject",
                "[INJECT:clipboard_paste] replace delete={delete_chars}（纯删除）"
            );
        }
        Ok(())
    }
}

/// 保存剪贴板 → 写入文本 → Ctrl+V →（可选）恢复剪贴板。
#[cfg(target_os = "windows")]
fn paste_via_clipboard(text: &str, restore: bool, delay_ms: u64) -> Result<(), InjectError> {
    use std::time::Duration;

    let mut clipboard = arboard::Clipboard::new()
        .map_err(|e| InjectError::Clipboard(e.to_string()))?;
    let saved = if restore { clipboard.get_text().ok() } else { None };

    clipboard
        .set_text(text.to_owned())
        .map_err(|e| InjectError::Clipboard(e.to_string()))?;
    std::thread::sleep(Duration::from_millis(delay_ms));

    // SAFETY: keybd_event 只向当前焦点窗口发一组 Ctrl+V，不接收指针。
    unsafe { send_ctrl_v() };
    std::thread::sleep(Duration::from_millis(delay_ms));

    if let Some(previous) = saved {
        let _ = clipboard.set_text(previous);
    }
    Ok(())
}

#[cfg(target_os = "windows")]
#[link(name = "user32")]
extern "system" {
    fn keybd_event(b_vk: u8, b_scan: u8, dw_flags: u32, dw_extra_info: usize);
}

#[cfg(target_os = "windows")]
const VK_BACK: u8 = 0x08;
#[cfg(target_os = "windows")]
const VK_CONTROL: u8 = 0x11;
#[cfg(target_os = "windows")]
const VK_V: u8 = 0x56;
#[cfg(target_os = "windows")]
const KEYEVENTF_KEYUP: u32 = 0x0002;

#[cfg(target_os = "windows")]
unsafe fn send_ctrl_v() {
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event(VK_V, 0, 0, 0);
    keybd_event(VK_V, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
}

/// 退格 `n` 次，逐个 keydown/keyup，避免某些应用把连续 down 当成长按重复。
#[cfg(target_os = "windows")]
unsafe fn send_backspaces(n: usize) {
    for _ in 0..n {
        keybd_event(VK_BACK, 0, 0, 0);
        keybd_event(VK_BACK, 0, KEYEVENTF_KEYUP, 0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn none_is_default_and_never_fails() {
        let cfg = InjectConfig::default();
        let i = build_injector(&cfg).unwrap();
        assert_eq!(i.name(), "none");
        assert!(i.inject("测试").is_ok());
        assert!(i.inject_replace(2, "试").is_ok());
        assert!(i.inject_replace(0, "").is_ok());
    }

    #[test]
    fn unknown_mode_is_rejected() {
        let mut cfg = InjectConfig::default();
        cfg.mode = "tsf".into();
        assert!(build_injector(&cfg).is_err());
    }
}
