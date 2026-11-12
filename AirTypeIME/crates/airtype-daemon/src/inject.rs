//! 文本注入。第一版两种模式：
//!
//! - `none`：只写日志，不碰剪贴板和按键（无头验证、FFI 外壳接管时使用）
//! - `clipboard_paste`（Windows）：保存剪贴板 → 写入文本 → 模拟 Ctrl+V → 恢复剪贴板
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
        use std::time::Duration;

        let mut clipboard = arboard::Clipboard::new()
            .map_err(|e| InjectError::Clipboard(e.to_string()))?;
        let saved = if self.restore {
            clipboard.get_text().ok()
        } else {
            None
        };

        clipboard
            .set_text(text.to_owned())
            .map_err(|e| InjectError::Clipboard(e.to_string()))?;
        std::thread::sleep(Duration::from_millis(self.delay_ms));

        // SAFETY: keybd_event 只向当前焦点窗口发一组 Ctrl+V，不接收指针。
        unsafe { send_ctrl_v() };
        std::thread::sleep(Duration::from_millis(self.delay_ms));

        if self.restore {
            if let Some(previous) = saved {
                let _ = clipboard.set_text(previous);
            }
        }
        tracing::info!(target: "inject", "[INJECT:clipboard_paste] 已写入 {} 字节", text.len());
        Ok(())
    }
}

#[cfg(target_os = "windows")]
#[link(name = "user32")]
extern "system" {
    fn keybd_event(b_vk: u8, b_scan: u8, dw_flags: u32, dw_extra_info: usize);
}

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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn none_is_default_and_never_fails() {
        let cfg = InjectConfig::default();
        let i = build_injector(&cfg).unwrap();
        assert_eq!(i.name(), "none");
        assert!(i.inject("测试").is_ok());
    }

    #[test]
    fn unknown_mode_is_rejected() {
        let mut cfg = InjectConfig::default();
        cfg.mode = "tsf".into();
        assert!(build_injector(&cfg).is_err());
    }
}
