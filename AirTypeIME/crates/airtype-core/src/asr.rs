//! ASR 适配层：`AsrEngine` trait 与两种内置实现。
//!
//! - `MockAsr`：默认，离线即可跑通整条链路。
//! - `CommandAsr`：把一段 PCM 写成临时 WAV，交给外部命令（whisper.cpp /
//!   faster-whisper CLI / 云端 SDK 脚本），取 stdout 首行作为识别文本。
//!   **每段语音都冷启动一个进程**，真实模型下加载开销大，只适合调试。
//!
//! 生产用的本地离线引擎是 `airtype-asr-sherpa`（内嵌 sherpa-onnx），
//! 由 `airtype-daemon` 负责装配，core 只定义 trait 与纯数据配置。
//!
//! 两种引擎都只依赖标准库，core 保持零异步运行时、零平台 API。

use std::path::Path;
use std::process::{Command, Stdio};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use thiserror::Error;

use crate::audio::encode_wav;
use crate::config::AsrConfig;

#[derive(Debug, Error)]
pub enum AsrError {
    #[error("asr i/o: {0}")]
    Io(#[from] std::io::Error),
    #[error("asr engine returned no text")]
    Empty,
    #[error("asr command exited with {0}")]
    Command(String),
    #[error("asr command timed out")]
    Timeout,
    #[error("asr config: {0}")]
    Config(String),
}

pub trait AsrEngine: Send + Sync {
    fn name(&self) -> &str;
    fn transcribe(&self, samples: &[i16], sample_rate: u32) -> Result<String, AsrError>;
}

/// 按配置构造引擎。内嵌 sherpa-onnx 引擎不在这里——core 不能依赖它的
/// native 库，由 `airtype-daemon` 按 `asr.engine` 分发。
pub fn build_engine(cfg: &AsrConfig) -> Result<Arc<dyn AsrEngine>, AsrError> {
    match cfg.engine.as_str() {
        "mock" | "" => Ok(Arc::new(MockAsr {
            text: cfg.mock_text.clone(),
        })),
        "command" => {
            if cfg.command.trim().is_empty() {
                return Err(AsrError::Config("asr.command is empty".into()));
            }
            Ok(Arc::new(CommandAsr {
                template: cfg.command.clone(),
                timeout_ms: cfg.timeout_ms,
            }))
        }
        other => Err(AsrError::Config(format!(
            "unknown asr engine `{other}` (supported: mock, command; sherpa 由 daemon 装配)"
        ))),
    }
}

pub struct MockAsr {
    pub text: String,
}

impl AsrEngine for MockAsr {
    fn name(&self) -> &str {
        "mock"
    }
    fn transcribe(&self, _samples: &[i16], _sample_rate: u32) -> Result<String, AsrError> {
        if self.text.trim().is_empty() {
            return Err(AsrError::Empty);
        }
        Ok(self.text.clone())
    }
}

pub struct CommandAsr {
    pub template: String,
    pub timeout_ms: u64,
}

/// 支持双引号的命令模板分词。刻意不做 shell 展开（管道/重定向/变量都不支持）：
/// ASR 命令只应是「程序 + 参数」，由配置完全决定。
fn split_command_line(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut in_quotes = false;
    let mut started = false;
    for c in s.chars() {
        match c {
            '"' => {
                in_quotes = !in_quotes;
                started = true;
            }
            c if c.is_whitespace() && !in_quotes => {
                if started {
                    out.push(std::mem::take(&mut cur));
                    started = false;
                }
            }
            c => {
                cur.push(c);
                started = true;
            }
        }
    }
    if started {
        out.push(cur);
    }
    out
}

impl CommandAsr {
    fn run(&self, wav_path: &Path) -> Result<String, AsrError> {
        let out_path = wav_path.with_extension("txt");
        let err_path = wav_path.with_extension("err");
        let _ = std::fs::remove_file(&out_path);
        let _ = std::fs::remove_file(&err_path);

        // 先分词、再把整个 {wav} token 换成路径：路径含空格也不会被拆开，
        // 同时绕开 Windows `cmd /C` 对嵌套引号的特殊处理（会让子进程拿到错误参数）。
        let wav = wav_path.to_string_lossy().into_owned();
        let parts: Vec<String> = split_command_line(&self.template)
            .into_iter()
            .map(|p| {
                if p.contains("{wav}") {
                    p.replace("{wav}", &wav)
                } else {
                    p
                }
            })
            .collect();
        if parts.is_empty() || parts[0].is_empty() {
            return Err(AsrError::Config("asr.command has no program".into()));
        }

        let mut command = Command::new(&parts[0]);
        command.args(&parts[1..]);

        // stdout/stderr 重定向到文件而非管道，避免子进程输出较多时管道写满死锁。
        command.stdout(Stdio::from(std::fs::File::create(&out_path)?));
        command.stderr(Stdio::from(std::fs::File::create(&err_path)?));
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            const CREATE_NO_WINDOW: u32 = 0x0800_0000;
            command.creation_flags(CREATE_NO_WINDOW);
        }

        let mut child = command.spawn()?;
        let started = Instant::now();
        let status = loop {
            match child.try_wait()? {
                Some(st) => break st,
                None => {
                    if started.elapsed().as_millis() as u64 >= self.timeout_ms {
                        let _ = child.kill();
                        let _ = child.wait();
                        let _ = std::fs::remove_file(&out_path);
                        let _ = std::fs::remove_file(&err_path);
                        return Err(AsrError::Timeout);
                    }
                    std::thread::sleep(Duration::from_millis(10));
                }
            }
        };

        let text = std::fs::read_to_string(&out_path)?;
        let stderr_text = std::fs::read_to_string(&err_path).unwrap_or_default();
        let _ = std::fs::remove_file(&out_path);
        let _ = std::fs::remove_file(&err_path);
        if !status.success() {
            return Err(AsrError::Command(format!(
                "{status:?}: {}",
                stderr_text.trim()
            )));
        }
        let line = text.lines().next().unwrap_or("").trim().to_string();
        if line.is_empty() {
            Err(AsrError::Empty)
        } else {
            Ok(line)
        }
    }
}

impl AsrEngine for CommandAsr {
    fn name(&self) -> &str {
        "command"
    }
    fn transcribe(&self, samples: &[i16], sample_rate: u32) -> Result<String, AsrError> {
        if samples.is_empty() {
            return Err(AsrError::Empty);
        }
        let stamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis();
        let wav_path = std::env::temp_dir().join(format!(
            "airtype_{}_{}.wav",
            std::process::id(),
            stamp
        ));
        std::fs::write(&wav_path, encode_wav(samples, sample_rate))?;
        let result = self.run(&wav_path);
        let _ = std::fs::remove_file(&wav_path);
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mock_engine_returns_configured_text() {
        let cfg = AsrConfig::default();
        let e = build_engine(&cfg).unwrap();
        assert_eq!(e.name(), "mock");
        assert!(e.transcribe(&[0i16; 320], 16000).unwrap().contains("AirType"));
    }

    #[test]
    fn unknown_engine_is_rejected() {
        let mut cfg = AsrConfig::default();
        cfg.engine = "whisper-http".into();
        assert!(build_engine(&cfg).is_err());
    }

    #[test]
    fn sherpa_is_assembled_by_daemon_not_core() {
        // core 不能依赖 sherpa-onnx 的 native 库，sherpa 引擎由 daemon 装配。
        let mut cfg = AsrConfig::default();
        cfg.engine = "sherpa".into();
        let err = match build_engine(&cfg) {
            Ok(_) => panic!("core 不应能装配 sherpa 引擎"),
            Err(e) => e,
        };
        assert!(
            err.to_string().contains("sherpa"),
            "错误信息应指明由 daemon 装配: {err}"
        );
    }

    #[test]
    fn empty_command_is_rejected() {
        let mut cfg = AsrConfig::default();
        cfg.engine = "command".into();
        cfg.command = "".into();
        assert!(build_engine(&cfg).is_err());
    }

    #[test]
    fn command_line_splitting_keeps_quoted_paths_whole() {
        assert_eq!(
            split_command_line("python \"C:/my tools/asr.py\" {wav} --lang zh"),
            vec!["python", "C:/my tools/asr.py", "{wav}", "--lang", "zh"]
        );
    }

    #[test]
    fn command_engine_runs_a_real_external_process() {
        // 用 Python 自身做外部命令，验证 WAV 落盘 + stdout 首行取值这条链路。
        let py = if cfg!(windows) { "python" } else { "python3" };
        let engine = CommandAsr {
            template: format!(
                "{py} -c \"import sys,wave;w=wave.open(sys.argv[1]);print('frames',w.getnframes())\" {{wav}}"
            ),
            timeout_ms: 10_000,
        };
        let text = engine.transcribe(&vec![0i16; 320], 16000).unwrap();
        assert_eq!(text, "frames 320");
    }

}
