//! AirTypeIME 内嵌 sherpa-onnx 识别引擎。
//!
//! 替代原先的 Python 常驻服务（`tools/asr_server.py`）：模型在 daemon 进程内
//! 加载，没有 HTTP 往返、没有跨进程序列化，也没有 Python 运行时依赖。
//!
//! 用的是 k2-fsa 官方 Rust 绑定 [`sherpa_onnx`]（crate `sherpa-onnx`），
//! 静态链接 onnxruntime，release 二进制只增重约 18MB。
//!
//! 同一个 fp32 流式 zipformer 模型同时服务两条路径：
//!
//! - 流式：分块 `accept_waveform` + `input_finished`，逐步取回累积文本；
//! - 整段：整段一次喂入 + `input_finished`，等价于离线识别。
//!
//! **只接受 fp32 模型**：文件名含 `.int8` / `.fp16` / `.int4` 等量化标记的
//! 一律跳过，找不到 fp32 直接报错。量化版本识别质量明显更差，这是明确取舍。

use std::path::{Path, PathBuf};
use std::sync::Arc;

use sherpa_onnx::{
    OnlineRecognizer, OnlineRecognizerConfig, OnlineStream, OnlineTransducerModelConfig,
};

use airtype_core::asr::{AsrEngine, AsrError};
use airtype_core::config::SherpaConfig;
use airtype_core::streaming::{AsrStream, StreamOutcome, StreamingAsr};

/// 量化文件名标签。命中任一即视为量化模型，直接跳过。
/// 在原 `asr_server.py` 规则（`.int8` / `.fp16` / `.quant` / `.q4` …）基础上
/// 补上连字符变体，避免 `model-uint8.onnx` 这类名字漏网。
const QUANT_TAGS: &[&str] = &[
    ".int8", "-int8", ".int4", "-int4", ".uint8", "-uint8", ".fp16", "-fp16", ".fp8", "-fp8",
    ".quant", "-quant", ".q4", "-q4", ".q8", "-q8", ".awq", ".gptq",
];

/// 文件名是否为非量化的 fp32 版本。
pub fn is_fp32(name: &str) -> bool {
    let n = name.to_ascii_lowercase();
    !QUANT_TAGS.iter().any(|t| n.contains(t))
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModelFiles {
    pub encoder: PathBuf,
    pub decoder: PathBuf,
    pub joiner: PathBuf,
    pub tokens: PathBuf,
}

/// 解析模型文件。显式路径优先；留空则按 `stem` 扫目录并跳过量化文件。
///
/// `model_dir` 为相对路径时的基准（两级）：
///   1. 进程当前目录（保持既有约定，源码树内 `models/...` 仍可用）；
///   2. 配置文件所在目录（TSF DLL 注入任意宿主进程后 CWD 不可控，必须兜底）。
pub fn resolve_model_files(cfg: &SherpaConfig, base_dir: Option<&Path>) -> Result<ModelFiles, AsrError> {
    let raw = Path::new(&cfg.model_dir);
    let dir: PathBuf = if raw.is_absolute() {
        raw.to_path_buf()
    } else if raw.is_dir() {
        raw.to_path_buf()
    } else if let Some(base) = base_dir {
        base.join(raw)
    } else {
        raw.to_path_buf()
    };
    if !dir.is_dir() {
        return Err(AsrError::Config(format!(
            "sherpa.model_dir 不是目录: {}（模型下载见 README 第 6 节）",
            dir.display()
        )));
    }
    Ok(ModelFiles {
        encoder: resolve_onnx(&dir, &cfg.encoder, "encoder")?,
        decoder: resolve_onnx(&dir, &cfg.decoder, "decoder")?,
        joiner: resolve_onnx(&dir, &cfg.joiner, "joiner")?,
        tokens: resolve_tokens(&dir, &cfg.tokens)?,
    })
}

/// 在 `dir` 里找名字含 `stem`、以 `ext` 结尾且非量化的文件，按名字排序取第一个。
fn pick_fp32(dir: &Path, stem: &str, ext: &str) -> Option<PathBuf> {
    let mut hits: Vec<PathBuf> = std::fs::read_dir(dir)
        .ok()?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.is_file())
        .filter(|p| {
            let name = p
                .file_name()
                .and_then(|s| s.to_str())
                .unwrap_or("")
                .to_ascii_lowercase();
            name.contains(stem) && name.ends_with(ext)
        })
        .filter(|p| {
            let name = p.file_name().and_then(|s| s.to_str()).unwrap_or("");
            is_fp32(name)
        })
        .collect();
    hits.sort();
    hits.into_iter().next()
}

fn resolve_onnx(dir: &Path, explicit: &str, stem: &str) -> Result<PathBuf, AsrError> {
    if explicit.is_empty() {
        return pick_fp32(dir, stem, ".onnx").ok_or_else(|| {
            AsrError::Config(format!(
                "在 {} 里找不到 fp32 的 {stem}*.onnx（量化版本已按规则跳过）",
                dir.display()
            ))
        });
    }
    let p = {
        let raw = Path::new(explicit);
        if raw.is_absolute() {
            raw.to_path_buf()
        } else {
            dir.join(raw)
        }
    };
    let name = p.file_name().and_then(|s| s.to_str()).unwrap_or("");
    if !is_fp32(name) {
        return Err(AsrError::Config(format!(
            "sherpa.{stem} 指向量化模型 `{name}`，本引擎只接受 fp32"
        )));
    }
    if !p.is_file() {
        return Err(AsrError::Config(format!(
            "sherpa.{stem} 文件不存在: {}",
            p.display()
        )));
    }
    Ok(p)
}

fn resolve_tokens(dir: &Path, explicit: &str) -> Result<PathBuf, AsrError> {
    if explicit.is_empty() {
        let direct = dir.join("tokens.txt");
        if direct.is_file() {
            return Ok(direct);
        }
        return pick_fp32(dir, "tokens", ".txt").ok_or_else(|| {
            AsrError::Config(format!("在 {} 里找不到 tokens.txt", dir.display()))
        });
    }
    let p = {
        let raw = Path::new(explicit);
        if raw.is_absolute() {
            raw.to_path_buf()
        } else {
            dir.join(raw)
        }
    };
    if !p.is_file() {
        return Err(AsrError::Config(format!(
            "sherpa.tokens 文件不存在: {}",
            p.display()
        )));
    }
    Ok(p)
}

fn build_recognizer(cfg: &SherpaConfig, base_dir: Option<&Path>) -> Result<OnlineRecognizer, AsrError> {
    let files = resolve_model_files(cfg, base_dir)?;
    tracing::info!(
        encoder = %files.encoder.display(),
        decoder = %files.decoder.display(),
        joiner = %files.joiner.display(),
        tokens = %files.tokens.display(),
        "sherpa-onnx 模型文件已解析（fp32）"
    );

    let mut config = OnlineRecognizerConfig::default();
    config.model_config.transducer = OnlineTransducerModelConfig {
        encoder: Some(files.encoder.to_string_lossy().into_owned()),
        decoder: Some(files.decoder.to_string_lossy().into_owned()),
        joiner: Some(files.joiner.to_string_lossy().into_owned()),
    };
    config.model_config.tokens = Some(files.tokens.to_string_lossy().into_owned());
    config.model_config.num_threads = cfg.num_threads;
    config.model_config.provider = Some(cfg.provider.clone());
    if !cfg.modeling_unit.is_empty() {
        config.model_config.modeling_unit = Some(cfg.modeling_unit.clone());
    }
    config.decoding_method = Some(cfg.decoding_method.clone());
    config.enable_endpoint = cfg.enable_endpoint;

    OnlineRecognizer::create(&config).ok_or_else(|| {
        AsrError::Config(
            "sherpa-onnx 识别器创建失败：检查模型文件是否完整、tokens.txt 是否匹配".into(),
        )
    })
}

/// 一次性建好两条路径的引擎，**共享同一个 recognizer**（模型只加载一次）。
pub struct SherpaEngines {
    pub offline: Arc<dyn AsrEngine>,
    pub streaming: Option<Arc<dyn StreamingAsr>>,
}

pub fn build_engines(
    cfg: &SherpaConfig,
    base_dir: Option<&Path>,
    with_streaming: bool,
) -> Result<SherpaEngines, AsrError> {
    let recognizer = Arc::new(build_recognizer(cfg, base_dir)?);
    Ok(SherpaEngines {
        offline: Arc::new(SherpaEngine {
            recognizer: recognizer.clone(),
        }),
        streaming: if with_streaming {
            Some(Arc::new(SherpaStreamingAsr { recognizer }))
        } else {
            None
        },
    })
}

// ---------------------------------------------------------------------------
// 整段（离线）路径
// ---------------------------------------------------------------------------

struct SherpaEngine {
    recognizer: Arc<OnlineRecognizer>,
}

impl AsrEngine for SherpaEngine {
    fn name(&self) -> &str {
        "sherpa"
    }

    fn transcribe(&self, samples: &[i16], sample_rate: u32) -> Result<String, AsrError> {
        if samples.is_empty() {
            return Err(AsrError::Empty);
        }
        let stream = self.recognizer.create_stream();
        let floats = to_f32(samples);
        // 流式 encoder 也能整段处理：一次喂入后 input_finished，再排空解码队列。
        stream.accept_waveform(sample_rate as i32, &floats);
        stream.input_finished();
        drain(&self.recognizer, &stream);
        let text = result_text(&self.recognizer, &stream);
        if text.trim().is_empty() {
            Err(AsrError::Empty)
        } else {
            Ok(text)
        }
    }
}

// ---------------------------------------------------------------------------
// 流式路径
// ---------------------------------------------------------------------------

struct SherpaStreamingAsr {
    recognizer: Arc<OnlineRecognizer>,
}

impl StreamingAsr for SherpaStreamingAsr {
    fn name(&self) -> &str {
        "sherpa-streaming"
    }

    fn start(&self) -> Result<Box<dyn AsrStream>, AsrError> {
        Ok(Box::new(SherpaStream {
            recognizer: self.recognizer.clone(),
            stream: self.recognizer.create_stream(),
        }))
    }
}

struct SherpaStream {
    recognizer: Arc<OnlineRecognizer>,
    stream: OnlineStream,
}

impl AsrStream for SherpaStream {
    fn push(&mut self, samples: &[i16], sample_rate: u32) -> Result<StreamOutcome, AsrError> {
        if samples.is_empty() {
            return Ok(StreamOutcome::default());
        }
        let floats = to_f32(samples);
        self.stream.accept_waveform(sample_rate as i32, &floats);
        drain(&self.recognizer, &self.stream);
        Ok(StreamOutcome {
            text: result_text(&self.recognizer, &self.stream),
            is_final: false,
        })
    }

    fn finish(&mut self) -> Result<StreamOutcome, AsrError> {
        self.stream.input_finished();
        drain(&self.recognizer, &self.stream);
        Ok(StreamOutcome {
            text: result_text(&self.recognizer, &self.stream),
            is_final: true,
        })
    }

    fn cancel(&mut self) {
        // OnlineStream 随 drop 释放，无需额外清理。
    }
}

/// 排空解码队列：`is_ready` 为真就继续 `decode`，直到没有可解码的帧。
fn drain(recognizer: &OnlineRecognizer, stream: &OnlineStream) {
    // 防御性上限：每 decode 必消耗一帧，正常不会接近这个数。
    const MAX_STEPS: u32 = 100_000;
    let mut steps = 0;
    while recognizer.is_ready(stream) {
        recognizer.decode(stream);
        steps += 1;
        if steps >= MAX_STEPS {
            tracing::warn!(steps, "sherpa-onnx 解码排空达到上限，可能模型异常");
            break;
        }
    }
}

fn result_text(recognizer: &OnlineRecognizer, stream: &OnlineStream) -> String {
    recognizer
        .get_result(stream)
        .map(|r| r.text)
        .unwrap_or_default()
}

/// i16 PCM -> 归一化 f32。与 sherpa-onnx 内部 `Wave::read` 的约定一致
/// （实测 `samples == i16 / 32768.0`）。
fn to_f32(samples: &[i16]) -> Vec<f32> {
    samples.iter().map(|s| *s as f32 / 32768.0).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::OnceLock;

    /// 每个测试用独立的临时目录，避免互相污染。
    fn temp_dir(tag: &str) -> PathBuf {
        static COUNTER: OnceLock<std::sync::atomic::AtomicUsize> = OnceLock::new();
        let n = COUNTER
            .get_or_init(|| std::sync::atomic::AtomicUsize::new(0))
            .fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        let dir = std::env::temp_dir().join(format!(
            "airtype_sherpa_{}_{}_{}",
            std::process::id(),
            tag,
            n
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn write(dir: &Path, name: &str) {
        std::fs::write(dir.join(name), b"stub").unwrap();
    }

    #[test]
    fn fp32_filter_rejects_quantized_names() {
        assert!(is_fp32("encoder-epoch-99-avg-1.onnx"));
        assert!(is_fp32("decoder-epoch-99-avg-1.onnx"));
        assert!(is_fp32("joiner-epoch-99-avg-1.onnx"));
        assert!(!is_fp32("encoder-epoch-99-avg-1.int8.onnx"));
        assert!(!is_fp32("model.int4.onnx"));
        assert!(!is_fp32("model-uint8.onnx"));
        assert!(!is_fp32("model.fp16.onnx"));
        assert!(!is_fp32("model-fp16.onnx"));
        assert!(!is_fp32("model.quant.onnx"));
        assert!(!is_fp32("model-q4.onnx"));
    }

    #[test]
    fn resolves_fp32_and_skips_quantized() {
        let dir = temp_dir("resolve");
        write(&dir, "encoder-epoch-99-avg-1.onnx");
        write(&dir, "encoder-epoch-99-avg-1.int8.onnx");
        write(&dir, "decoder-epoch-99-avg-1.onnx");
        write(&dir, "decoder-epoch-99-avg-1.int8.onnx");
        write(&dir, "joiner-epoch-99-avg-1.onnx");
        write(&dir, "joiner-epoch-99-avg-1.int8.onnx");
        write(&dir, "tokens.txt");

        let cfg = SherpaConfig {
            model_dir: dir.to_string_lossy().into_owned(),
            ..SherpaConfig::default()
        };
        let files = resolve_model_files(&cfg, None).unwrap();
        for (label, p) in [
            ("encoder", &files.encoder),
            ("decoder", &files.decoder),
            ("joiner", &files.joiner),
        ] {
            let name = p.file_name().unwrap().to_string_lossy().to_string();
            assert!(is_fp32(&name), "{label} 选中了量化文件: {name}");
            assert!(!name.contains("int8"), "{label} 选中了 int8: {name}");
        }
        assert_eq!(
            files.tokens.file_name().unwrap().to_string_lossy(),
            "tokens.txt"
        );

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn missing_model_dir_is_a_clear_error() {
        let cfg = SherpaConfig {
            model_dir: "/nonexistent/airtype/models".into(),
            ..SherpaConfig::default()
        };
        let err = resolve_model_files(&cfg, None).unwrap_err();
        assert!(err.to_string().contains("不是目录"));
    }

    #[test]
    fn relative_model_dir_falls_back_to_config_dir() {
        // 安装布局：配置在 <root>/config/airtype.toml，模型在 <root>/models/...。
        // 进程 CWD 不可控时，相对 model_dir 应能以配置文件目录为基准解析。
        let root = temp_dir("basedir");
        let cfg_dir = root.join("config");
        let dir = root.join("models").join("sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20");
        std::fs::create_dir_all(&cfg_dir).unwrap();
        std::fs::create_dir_all(&dir).unwrap();
        write(&dir, "encoder.onnx");
        write(&dir, "decoder.onnx");
        write(&dir, "joiner.onnx");
        write(&dir, "tokens.txt");

        let cfg = SherpaConfig {
            model_dir: "../models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20".into(),
            ..SherpaConfig::default()
        };
        let files = resolve_model_files(&cfg, Some(&cfg_dir)).unwrap();
        assert!(files.encoder.is_file());
        assert!(files.decoder.is_file());
        assert!(files.joiner.is_file());
        assert!(files.tokens.is_file());

        // 对照：不给 base_dir 且 CWD 下没有该目录 -> 明确报「不是目录」。
        let err = resolve_model_files(&cfg, None).unwrap_err();
        assert!(err.to_string().contains("不是目录"));

        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn explicit_quantized_path_is_rejected() {
        let dir = temp_dir("explicit");
        write(&dir, "encoder.int8.onnx");
        write(&dir, "decoder.onnx");
        write(&dir, "joiner.onnx");
        write(&dir, "tokens.txt");

        let cfg = SherpaConfig {
            model_dir: dir.to_string_lossy().into_owned(),
            encoder: "encoder.int8.onnx".into(),
            ..SherpaConfig::default()
        };
        let err = resolve_model_files(&cfg, None).unwrap_err();
        assert!(err.to_string().contains("只接受 fp32"));

        let _ = std::fs::remove_dir_all(&dir);
    }
}
