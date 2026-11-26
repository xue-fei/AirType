//! TOML 配置。所有字段都有默认值，缺表/缺字段都能正常启动。
//!
//! 同时派生 `Serialize`，让设置页面能把当前配置写回 `airtype.toml`。

use std::path::PathBuf;

use serde::{Deserialize, Serialize};
use thiserror::Error;

#[derive(Debug, Error)]
pub enum ConfigError {
    #[error("read config: {0}")]
    Io(#[from] std::io::Error),
    #[error("parse config: {0}")]
    Toml(#[from] toml::de::Error),
    #[error("serialize config: {0}")]
    TomlSerialize(#[from] toml::ser::Error),
    #[error("invalid config: {0}")]
    Invalid(String),
}

#[derive(Debug, Clone, Deserialize, Serialize)]
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
    pub denoise: DenoiseConfig,
    #[serde(default)]
    pub streaming: StreamingConfig,
    #[serde(default)]
    pub postprocess: PostProcessConfig,
    #[serde(default)]
    pub inject: InjectConfig,
    #[serde(default)]
    pub web: WebConfig,

    /// 配置文件所在目录。不参与序列化（设置页回写不应把它写进 TOML）。
    ///
    /// 存在的原因：`sherpa.model_dir` 之类相对路径必须相对**配置文件**解析，
    /// 而不是相对进程当前目录——TSF DLL 会被注入到任意宿主进程（记事本、
    /// Word、浏览器…），CWD 完全不可控，按 CWD 找模型必然失败。
    #[serde(skip)]
    pub base_dir: Option<PathBuf>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct ServerConfig {
    #[serde(default = "default_bind")]
    pub bind: String,
    #[serde(default = "default_port")]
    pub port: u16,
    #[serde(default = "default_path")]
    pub path: String,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
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

#[derive(Debug, Clone, Deserialize, Serialize)]
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
#[derive(Debug, Clone, Deserialize, Serialize)]
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
    /// 热词文件，每行一个词/短语（UTF-8）。空 = 不启用。
    ///
    /// 用途：修正模型系统性听错的专有名词与高频词。相对路径按
    /// 「配置文件所在目录 → CWD」解析，与 `model_dir` 同一套规则。
    #[serde(default)]
    pub hotwords_file: String,
    /// 热词偏置强度。常用 1.0~3.0，过大（>4）容易把无关音频吸成热词。
    #[serde(default = "default_hotwords_score")]
    pub hotwords_score: f32,
}

/// 流式语音降噪（DPDFNet）。
///
/// 默认关闭：要额外加载 9.8MB 模型、多占约 20MB 内存，而且安静环境下
/// 降噪收益有限。嘈杂环境（办公室、车里）建议打开。
#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct DenoiseConfig {
    #[serde(default)]
    pub enabled: bool,
    /// 模型目录，相对「配置文件所在目录」解析（与 `sherpa.model_dir` 规则一致）。
    #[serde(default = "default_denoise_dir")]
    pub model_dir: String,
    /// 模型文件名。16 kHz 模型族，`dpdfnet2` 是速度/效果的平衡点
    /// （2.49M 参数，单线程 RTF 约 0.2）。
    #[serde(default = "default_denoise_model")]
    pub model: String,
    /// 抑制上限（dB），对应官方 `apply_attn_limit`。
    ///
    /// - 不写/`None`：纯模型输出（降噪最狠）
    /// - `0`：完全不降噪（等于旁路）
    /// - `3`~`6`：干湿混合，对干净语音的损伤小而噪声段仍有收益
    /// - 不写这一行 = 3.0（实测最稳的档位：干净语音 CER 0%，5dB 噪声 69%→21%）
    ///
    /// 实测（同一条 10s 中英混说，CER 相对干净音频）：
    /// ```text
    ///           原始   attn=0  attn=3  attn=6  attn=12  全降噪
    /// clean      0%      0%      0%      0%     2.4%   26.2%
    /// 15dB      31%     31%    19.0%   11.9%   19.0%   16.7%
    ///  5dB      69%   66.7%   21.4%   42.9%   35.7%   31.0%
    /// ```
    /// 全降噪（几百 dB）对干净语音是灾难，所以默认不取 None。
    #[serde(default = "default_attn_limit_db")]
    pub attn_limit_db: Option<f32>,
    /// onnxruntime.dll 路径，同样相对配置文件目录解析。
    ///
    /// 为什么必须显式给：ort 走 load-dynamic，而 sherpa-onnx 已经静态打包了
    /// 另一份 onnxruntime，两边的 CRT 和符号会冲突，不能共用。
    #[serde(default = "default_denoise_runtime")]
    pub runtime: String,
}

/// 流式识别（边说边出字）参数。`streaming.enabled` 为 true 时生效；
/// 关掉即回到「松键后整段解码」的旧路径。
#[derive(Debug, Clone, Deserialize, Serialize)]
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

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct PostProcessConfig {
    #[serde(default = "yes")]
    pub trim_filler_words: bool,
    #[serde(default = "yes")]
    pub normalize_numbers: bool,
    #[serde(default = "yes")]
    pub ensure_punctuation: bool,
    #[serde(default = "yes")]
    pub collapse_whitespace: bool,
    /// 英文全大写还原成正常大小写。
    ///
    /// zipformer 双语模型的英文 token 词表本身是大写（LIBRI 风格），
    /// 识别结果会是 `TODAY IS` 这种全大写。打开后按"每个词首字母大写、
    /// 其余小写"归一（`TODAY` → `Today`），单字母 `I` / `A` 保持原样。
    #[serde(default = "yes")]
    pub normalize_english_case: bool,
    /// 中英之间补空格（`Today is礼拜二` → `Today is 礼拜二`）。
    ///
    /// 只处理含字母的英文词，`3月` / `第2种` 这类数字+中文不动。
    /// 不想要可以关掉。
    #[serde(default = "yes")]
    pub space_cjk_latin: bool,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct InjectConfig {
    /// `none` | `clipboard_paste`
    #[serde(default = "default_inject_mode")]
    pub mode: String,
    #[serde(default = "yes")]
    pub restore_clipboard: bool,
    #[serde(default = "default_paste_delay_ms")]
    pub paste_delay_ms: u64,
}

/// 内置设置页面（daemon 里的手写 HTTP 服务）。
#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct WebConfig {
    /// 是否开启设置页面。默认开启，但只监听 127.0.0.1。
    #[serde(default = "default_web_enabled")]
    pub enabled: bool,
    /// 监听地址。**保持 127.0.0.1**；改成 "0.0.0.0" 会让同局域网的人
    /// 都能改你的 ASR 与注入设置。
    #[serde(default = "default_web_bind")]
    pub bind: String,
    #[serde(default = "default_web_port")]
    pub port: u16,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            server: ServerConfig::default(),
            audio: AudioConfig::default(),
            asr: AsrConfig::default(),
            sherpa: SherpaConfig::default(),
            denoise: DenoiseConfig::default(),
            streaming: StreamingConfig::default(),
            postprocess: PostProcessConfig::default(),
            inject: InjectConfig::default(),
            web: WebConfig::default(),
            base_dir: None,
        }
    }
}

impl Config {
    pub fn load(path: impl AsRef<std::path::Path>) -> Result<Self, ConfigError> {
        let text = std::fs::read_to_string(path.as_ref())?;
        let mut cfg: Self = toml::from_str(&text)?;
        // 相对路径（model_dir 等）以配置文件所在目录为基准。
        cfg.base_dir = path.as_ref().parent().map(|p| p.to_path_buf());
        cfg.validate()?;
        Ok(cfg)
    }

    pub fn from_toml(text: &str) -> Result<Self, ConfigError> {
        let cfg: Self = toml::from_str(text)?;
        cfg.validate()?;
        Ok(cfg)
    }

    /// 校验参与除法/缓冲计算的正数字段。缺字段会走 serde 默认值（均合法），
    /// 但显式写 0 会让 `duration_ms` / `audio_ms` / 分块计算在收到首帧时除零 panic，
    /// 且 panic 发生在 extern "C" 边界会直接 abort 整个宿主进程。
    fn validate(&self) -> Result<(), ConfigError> {
        let a = &self.audio;
        if a.sample_rate == 0 {
            return Err(ConfigError::Invalid("audio.sample_rate must be > 0".into()));
        }
        if a.frame_ms == 0 {
            return Err(ConfigError::Invalid("audio.frame_ms must be > 0".into()));
        }
        if a.max_utterance_ms == 0 {
            return Err(ConfigError::Invalid("audio.max_utterance_ms must be > 0".into()));
        }
        if a.channels == 0 {
            return Err(ConfigError::Invalid("audio.channels must be > 0".into()));
        }
        if a.bits_per_sample == 0 {
            return Err(ConfigError::Invalid("audio.bits_per_sample must be > 0".into()));
        }
        if self.streaming.enabled && self.streaming.chunk_ms == 0 {
            return Err(ConfigError::Invalid(
                "streaming.chunk_ms must be > 0 when streaming is enabled".into(),
            ));
        }
        Ok(())
    }

    /// 写成 TOML（设置页面保存用）。所有字段都会输出，
    /// 因此产物本身就是一份带注释锚点的完整配置。
    pub fn to_toml(&self) -> Result<String, ConfigError> {
        Ok(toml::to_string(self)?)
    }

    /// 读出来再写回去，保留未显式设置的默认值。
    pub fn save(&self, path: impl AsRef<std::path::Path>) -> Result<(), ConfigError> {
        std::fs::write(path, self.to_toml()?)?;
        Ok(())
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
    hotwords_file: String::new(),
    hotwords_score: default_hotwords_score(),
);
defaults!(DenoiseConfig,
    enabled: false,
    model_dir: default_denoise_dir(),
    model: default_denoise_model(),
    runtime: default_denoise_runtime(),
    attn_limit_db: default_attn_limit_db(),
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
    normalize_english_case: true,
    space_cjk_latin: true,
);
defaults!(InjectConfig,
    mode: default_inject_mode(),
    restore_clipboard: true,
    paste_delay_ms: default_paste_delay_ms(),
);
defaults!(WebConfig,
    enabled: default_web_enabled(),
    bind: default_web_bind(),
    port: default_web_port(),
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
fn default_hotwords_score() -> f32 {
    1.5
}

fn default_attn_limit_db() -> Option<f32> {
    Some(3.0)
}

fn default_denoise_dir() -> String {
    "../models/dpdfnet".to_string()
}

fn default_denoise_model() -> String {
    "dpdfnet2.onnx".to_string()
}

fn default_denoise_runtime() -> String {
    "../lib/onnxruntime.dll".to_string()
}

fn default_streaming_enabled() -> bool {
    // 默认开：边说边出字是输入法的常规体验（搜狗/微软拼音都是这样），
    // 整段解码要等松键后静默一两秒才冒字，用户会以为没反应。
    // 想要"松键后一次性给完整文本"的，在 [streaming] 里写 enabled = false。
    true
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
fn default_web_enabled() -> bool {
    true
}
fn default_web_bind() -> String {
    "127.0.0.1".into()
}
fn default_web_port() -> u16 {
    8766
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
    fn rejects_zero_sample_rate() {
        // 除零点：audio.rs::duration_ms / pipeline.rs::audio_ms 都拿它当除数。
        let err = Config::from_toml("[audio]\nsample_rate = 0\n").unwrap_err();
        assert!(err.to_string().contains("sample_rate"), "{err}");
    }

    #[test]
    fn rejects_zero_streaming_chunk_when_enabled() {
        let err = Config::from_toml("[streaming]\nenabled = true\nchunk_ms = 0\n").unwrap_err();
        assert!(err.to_string().contains("chunk_ms"), "{err}");
    }

    #[test]
    fn empty_toml_uses_defaults() {
        let c = Config::from_toml("").unwrap();
        assert_eq!(c.server.port, 8765);
        assert_eq!(c.audio.sample_rate, 16000);
        assert_eq!(c.asr.engine, "mock");
        assert_eq!(c.inject.mode, "none");
        assert!(c.streaming.enabled, "默认开启流式（边说边出字）");
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
        // 默认开启：边说边出字是输入法常规体验；chunk_ms 200 是解码频率与
        // 首字延迟的折中，replace 让 partial 按最长公共前缀覆盖重写。
        assert!(s.enabled);
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

    #[test]
    fn web_config_defaults_to_localhost() {
        let w = WebConfig::default();
        assert!(w.enabled, "设置页面默认开启，方便发现");
        assert_eq!(w.bind, "127.0.0.1", "默认只监听本机");
        assert_eq!(w.port, 8766);

        let c = Config::from_toml("").unwrap();
        assert_eq!(c.web.bind, "127.0.0.1");
        assert_eq!(c.web.port, 8766);
    }

    #[test]
    fn web_config_is_parsed() {
        let c = Config::from_toml("[web]\nenabled = false\nbind = \"0.0.0.0\"\nport = 9000\n")
            .unwrap();
        assert!(!c.web.enabled);
        assert_eq!(c.web.bind, "0.0.0.0");
        assert_eq!(c.web.port, 9000);
    }

    #[test]
    fn to_toml_round_trip_preserves_values() {
        let c = Config::from_toml(
            "[server]\nport = 9000\n\n[asr]\nengine = \"command\"\n\n[streaming]\nenabled = true\n",
        )
        .unwrap();
        let text = c.to_toml().unwrap();
        let back = Config::from_toml(&text).unwrap();
        assert_eq!(back.server.port, 9000);
        assert_eq!(back.asr.engine, "command");
        assert!(back.streaming.enabled);
        assert_eq!(back.server.bind, "127.0.0.1", "未指定的字段保持默认");
        assert_eq!(back.web.port, 8766);
    }

    #[test]
    fn to_toml_emits_every_section() {
        let text = Config::default().to_toml().unwrap();
        for section in [
            "[server]",
            "[audio]",
            "[asr]",
            "[sherpa]",
            "[streaming]",
            "[postprocess]",
            "[inject]",
            "[web]",
        ] {
            assert!(text.contains(section), "写出内容缺少 {section}:\n{text}");
        }
    }

    #[test]
    fn empty_config_round_trips_through_writer() {
        let c = Config::from_toml("").unwrap();
        let back = Config::from_toml(&c.to_toml().unwrap()).unwrap();
        assert_eq!(back.server.port, c.server.port);
        assert_eq!(back.audio.sample_rate, c.audio.sample_rate);
        assert_eq!(back.sherpa.model_dir, c.sherpa.model_dir);
        assert_eq!(back.inject.mode, c.inject.mode);
        assert_eq!(back.web.port, c.web.port);
    }
}
