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
    pub sherpa: SherpaConfig,
    #[serde(default)]
    pub streaming: StreamingConfig,
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
    /// `mock` | `command` | `sherpa`
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

/// 内嵌 sherpa-onnx 引擎参数（`asr.engine = "sherpa"` 时生效）。
#[derive(Debug, Clone, Deserialize)]
pub struct SherpaConfig {
    /// 模型目录，缺省指向仓库内已下载的 fp32 流式 zipformer。
    #[serde(default = "default_sherpa_model_dir")]
    pub model_dir: String,
    /// 显式指定 encoder 文件（相对 `model_dir` 或绝对路径）。留空则自动扫目录。
    #[serde(default)]
    pub encoder: String,
    /// 显式指定 decoder 文件。留空则自动扫目录。
    #[serde(default)]
    pub decoder: String,
    /// 显式指定 joiner 文件。留空则自动扫目录。
    #[serde(default)]
    pub joiner: String,
    /// 显式指定 tokens 文件。留空则自动找 `tokens.txt`。
    #[serde(default)]
    pub tokens: String,
    /// ONNX Runtime 线程数。
    #[serde(default = "default_sherpa_num_threads")]
    pub num_threads: i32,
    /// 执行提供器，本机 CPU 用 `cpu`。
    #[serde(default = "default_sherpa_provider")]
    pub provider: String,
    /// 建模单元，中英混说用 `cjkchar`。
    #[serde(default = "default_sherpa_modeling_unit")]
    pub modeling_unit: String,
    /// 解码方法：`greedy_search` 或 `modified_beam_search`。
    #[serde(default = "default_sherpa_decoding_method")]
    pub decoding_method: String,
    /// 是否启用端点检测。AirType 用 PTT 松键收尾，保持 false。
    #[serde(default)]
    pub enable_endpoint: bool,
}

/// 流式识别（边说边出字）参数。`streaming.enabled` 为 true 时生效；
/// 关掉即回到「松键后整段解码」的旧路径。
#[derive(Debug, Clone, Deserialize)]
pub struct StreamingConfig {
    #[serde(default = "default_streaming_enabled")]
    pub enabled: bool,
    /// 攒够多少毫秒的增量音频请求一次识别。太小则请求频繁，
    /// 太大则首字延迟变差；16kHz/单声道下 200ms ≈ 3200 samples。
    #[serde(default = "default_chunk_ms")]
    pub chunk_ms: u32,
    /// true：partial 变化时按最长公共前缀撤销重写，实时上屏（需可覆盖式注入）；
    /// false：partial 只回传给上层，松键后一次性注入完整文本。
    #[serde(default = "default_replace")]
    pub replace: bool,
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
            sherpa: SherpaConfig::default(),
            streaming: StreamingConfig::default(),
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
defaults!(SherpaConfig,
    model_dir: default_sherpa_model_dir(),
    encoder: String::new(),
    decoder: String::new(),
    joiner: String::new(),
    tokens: String::new(),
    num_threads: default_sherpa_num_threads(),
    provider: default_sherpa_provider(),
    modeling_unit: default_sherpa_modeling_unit(),
    decoding_method: default_sherpa_decoding_method(),
    enable_endpoint: false,
);
defaults!(StreamingConfig,
    enabled: default_streaming_enabled(),
    chunk_ms: default_chunk_ms(),
    replace: default_replace(),
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
fn default_sherpa_model_dir() -> String {
    "models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20".into()
}
fn default_sherpa_num_threads() -> i32 {
    4
}
fn default_sherpa_provider() -> String {
    "cpu".into()
}
fn default_sherpa_modeling_unit() -> String {
    "cjkchar".into()
}
fn default_sherpa_decoding_method() -> String {
    "greedy_search".into()
}
fn default_streaming_enabled() -> bool {
    false
}
fn default_chunk_ms() -> u32 {
    200
}
fn default_replace() -> bool {
    true
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
        assert!(!c.streaming.enabled, "默认整段解码，不启用流式");
    }

    #[test]
    fn streaming_config_is_parsed() {
        let c = Config::from_toml(
            "[asr]\nengine = \"sherpa\"\n\n[streaming]\nenabled = true\nchunk_ms = 250\nreplace = false\n",
        )
        .unwrap();
        assert!(c.streaming.enabled);
        assert_eq!(c.streaming.chunk_ms, 250);
        assert!(!c.streaming.replace, "replace=false 时只提交稳定段");
    }

    #[test]
    fn sherpa_config_defaults_are_safe() {
        let s = SherpaConfig::default();
        assert!(s.model_dir.contains("streaming-zipformer"));
        assert_eq!(s.num_threads, 4);
        assert_eq!(s.provider, "cpu");
        assert_eq!(s.modeling_unit, "cjkchar");
        assert_eq!(s.decoding_method, "greedy_search");
        assert!(!s.enable_endpoint, "PTT 松键收尾，不靠端点检测");
    }

    #[test]
    fn sherpa_section_overrides_only_given_fields() {
        let c = Config::from_toml("[sherpa]\nnum_threads = 8\n").unwrap();
        assert_eq!(c.sherpa.num_threads, 8);
        assert_eq!(c.sherpa.provider, "cpu");
        assert_eq!(c.asr.engine, "mock", "未指定的段保持默认");
    }

    #[test]
    fn streaming_defaults_are_safe() {
        let s = StreamingConfig::default();
        assert!(!s.enabled);
        assert_eq!(s.chunk_ms, 200);
        assert!(s.replace);
    }

    #[test]
    fn partial_toml_overrides_only_given_fields() {
        let c = Config::from_toml("[asr]\nengine = \"command\"\n").unwrap();
        assert_eq!(c.asr.engine, "command");
        assert_eq!(c.asr.timeout_ms, 20_000);
        assert_eq!(c.server.port, 8765);
    }
}
