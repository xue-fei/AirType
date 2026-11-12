//! TOML 配置。所有字段都有默认值，缺表/缺字段都能正常启动。

use serde::Deserialize;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum ConfigError {
    #[error("read config: {0}")]
    Io(#[from] std::io::Error),
    #[error("parse config: {0}")]
    Toml(#[from] toml::de::Error),
}

#[derive(Debug, Clone, Deserialize)]
pub struct Config {
    #[serde(default)]
    pub server: ServerConfig,
    #[serde(default)]
    pub audio: AudioConfig,
    #[serde(default)]
    pub asr: AsrConfig,
    #[serde(default)]
    pub postprocess: PostProcessConfig,
    #[serde(default)]
    pub inject: InjectConfig,
}

#[derive(Debug, Clone, Deserialize)]
pub struct ServerConfig {
    #[serde(default = "default_bind")]
    pub bind: String,
    #[serde(default = "default_port")]
    pub port: u16,
    #[serde(default = "default_path")]
    pub path: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct AudioConfig {
    #[serde(default = "default_sample_rate")]
    pub sample_rate: u32,
    #[serde(default = "default_channels")]
    pub channels: u16,
    #[serde(default = "default_bits")]
    pub bits_per_sample: u16,
    #[serde(default = "default_frame_ms")]
    pub frame_ms: u32,
    #[serde(default = "default_max_utterance_ms")]
    pub max_utterance_ms: u32,
}

#[derive(Debug, Clone, Deserialize)]
pub struct AsrConfig {
    /// `mock` | `command`
    #[serde(default = "default_engine")]
    pub engine: String,
    /// `command` 模式的命令模板，`{wav}` 会替换成临时 WAV 路径（路径含空格也安全）。
    #[serde(default = "default_command")]
    pub command: String,
    #[serde(default = "default_timeout_ms")]
    pub timeout_ms: u64,
    #[serde(default = "default_mock_text")]
    pub mock_text: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct PostProcessConfig {
    #[serde(default = "yes")]
    pub trim_filler_words: bool,
    #[serde(default = "yes")]
    pub normalize_numbers: bool,
    #[serde(default = "yes")]
    pub ensure_punctuation: bool,
    #[serde(default = "yes")]
    pub collapse_whitespace: bool,
}

#[derive(Debug, Clone, Deserialize)]
pub struct InjectConfig {
    /// `none` | `clipboard_paste`
    #[serde(default = "default_inject_mode")]
    pub mode: String,
    #[serde(default = "yes")]
    pub restore_clipboard: bool,
    #[serde(default = "default_paste_delay_ms")]
    pub paste_delay_ms: u64,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            server: ServerConfig::default(),
            audio: AudioConfig::default(),
            asr: AsrConfig::default(),
            postprocess: PostProcessConfig::default(),
            inject: InjectConfig::default(),
        }
    }
}

impl Config {
    pub fn load(path: impl AsRef<std::path::Path>) -> Result<Self, ConfigError> {
        let text = std::fs::read_to_string(path)?;
        Ok(toml::from_str(&text)?)
    }

    pub fn from_toml(text: &str) -> Result<Self, ConfigError> {
        Ok(toml::from_str(text)?)
    }
}

macro_rules! defaults {
    ($ty:ty, $($field:ident : $value:expr),* $(,)?) => {
        impl Default for $ty {
            fn default() -> Self {
                Self { $($field: $value),* }
            }
        }
    };
}

defaults!(ServerConfig,
    bind: default_bind(),
    port: default_port(),
    path: default_path(),
);
defaults!(AudioConfig,
    sample_rate: default_sample_rate(),
    channels: default_channels(),
    bits_per_sample: default_bits(),
    frame_ms: default_frame_ms(),
    max_utterance_ms: default_max_utterance_ms(),
);
defaults!(AsrConfig,
    engine: default_engine(),
    command: default_command(),
    timeout_ms: default_timeout_ms(),
    mock_text: default_mock_text(),
);
defaults!(PostProcessConfig,
    trim_filler_words: true,
    normalize_numbers: true,
    ensure_punctuation: true,
    collapse_whitespace: true,
);
defaults!(InjectConfig,
    mode: default_inject_mode(),
    restore_clipboard: true,
    paste_delay_ms: default_paste_delay_ms(),
);

fn default_bind() -> String {
    "127.0.0.1".into()
}
fn default_port() -> u16 {
    8765
}
fn default_path() -> String {
    "/ws".into()
}
fn default_sample_rate() -> u32 {
    16000
}
fn default_channels() -> u16 {
    1
}
fn default_bits() -> u16 {
    16
}
fn default_frame_ms() -> u32 {
    20
}
fn default_max_utterance_ms() -> u32 {
    60_000
}
fn default_engine() -> String {
    "mock".into()
}
fn default_command() -> String {
    "python tools/asr_bridge.py {wav}".into()
}
fn default_timeout_ms() -> u64 {
    20_000
}
fn default_mock_text() -> String {
    "你好，这是一次 AirType 语音输入测试。".into()
}
fn default_inject_mode() -> String {
    "none".into()
}
fn default_paste_delay_ms() -> u64 {
    40
}
fn yes() -> bool {
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_toml_uses_defaults() {
        let c = Config::from_toml("").unwrap();
        assert_eq!(c.server.port, 8765);
        assert_eq!(c.audio.sample_rate, 16000);
        assert_eq!(c.asr.engine, "mock");
        assert_eq!(c.inject.mode, "none");
    }

    #[test]
    fn partial_toml_overrides_only_given_fields() {
        let c = Config::from_toml("[asr]\nengine = \"command\"\n").unwrap();
        assert_eq!(c.asr.engine, "command");
        assert_eq!(c.asr.timeout_ms, 20_000);
        assert_eq!(c.server.port, 8765);
    }
}
