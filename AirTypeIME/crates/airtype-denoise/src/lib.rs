//! DPDFNet 流式语音增强（16 kHz）。
//!
//! 推理后端：onnxruntime（`ort`）。STFT/ISTFT 用 `rustfft` 手写，参数全部来自
//! ONNX metadata，与官方 `stream.py::StreamEnhancer` 的因果路径一致。
//!
//! # 为什么不是 tract
//!
//! 先试过 `tract-onnx`：算子能加载、跳过优化时数值与 onnxruntime 逐样本一致，
//! 但它的 `into_optimized()` 会把这个模型改坏——语音帧的 `spec_e` 被压小
//! 20~200 倍（静音帧正常），降噪等于失效；而不优化则 RTF 高达 91。
//! tract 0.21 也没有 `into_decluttered()` 这个"只清理不替换"的中间档位。
//!
//! # 模型形态
//!
//! 逐帧流式，两输入两输出：
//!
//! ```text
//! spec[1,1,161,2] + state[45424]  ->  spec_e[1,1,161,2] + state_out[45424]
//! ```
//!
//! 每帧只吃 hop = 160 样本（10 ms），RNN 状态在帧之间传递。
//!
//! # 延迟
//!
//! 因果路径的延迟只有"凑满首窗"（n_fft - hop = 160 样本 = 10 ms）。
//! 输出相对输入还有模型自身的 640 样本内部延迟，实时场景不需要补偿
//! （官方 C# 移植同样不补偿）。要离线对齐 clean 时按 640 平移即可。

use std::f32::consts::PI;
use std::path::Path;
use std::sync::{Arc, Mutex};

use ort::environment::Environment;
use ort::session::{builder::GraphOptimizationLevel, Session};
use ort::value::Tensor;
use prost::Message;
use rustfft::num_complex::Complex;
use rustfft::{Fft, FftPlanner};
use thiserror::Error;

#[derive(Debug, Error)]
pub enum DenoiseError {
    #[error("模型加载失败: {0}")]
    Model(String),
    #[error("ONNX metadata 缺少字段 {0}")]
    MissingMeta(&'static str),
    #[error("ONNX metadata 解析失败: {0}")]
    BadMeta(String),
    #[error("推理失败: {0}")]
    Infer(String),
}

/// 加载好的模型（只读，可被多条流共享）。
pub struct DenoiseModel {
    /// 必须比 Session 活得久：ort 2.0 的 Session 绑定到 Environment。
    _env: Environment,
    /// run 需要 &mut Session（会话有状态），而模型被 Arc 共享，故用 Mutex 串行。
    session: Mutex<Session>,
    n_fft: usize,
    hop: usize,
    window: Vec<f32>,
    /// 归一化层初始统计量，长度 = state_size。
    state_init: Vec<f32>,
    fft: Arc<dyn Fft<f32>>,
    ifft: Arc<dyn Fft<f32>>,
}

/// Vorbis 窗。**不是** Hann：模型训练用的是 Vorbis，且它满足 50% 重叠 COLA
/// （`w[n]^2 + w[n+hop]^2 == 1`），正是"OLA 直接累加、不除窗平方和"的前提。
fn vorbis_window(n: usize) -> Vec<f32> {
    let h = n as f32 / 2.0;
    (0..n)
        .map(|i| {
            let s = ((0.5 * PI * (i as f32 + 0.5)) / h).sin();
            (0.5 * PI * s * s).sin()
        })
        .collect()
}

fn meta_get<'a>(
    map: &'a std::collections::HashMap<String, String>,
    key: &'static str,
) -> Result<&'a str, DenoiseError> {
    map.get(key)
        .map(|s| s.as_str())
        .ok_or(DenoiseError::MissingMeta(key))
}

fn meta_usize(
    map: &std::collections::HashMap<String, String>,
    key: &'static str,
) -> Result<usize, DenoiseError> {
    meta_get(map, key)?
        .parse()
        .map_err(|e| DenoiseError::BadMeta(format!("{key}: {e}")))
}

fn meta_f32_list(
    map: &std::collections::HashMap<String, String>,
    key: &'static str,
) -> Result<Vec<f32>, DenoiseError> {
    meta_get(map, key)?
        .split(',')
        .map(|s| {
            s.trim()
                .parse::<f32>()
                .map_err(|e| DenoiseError::BadMeta(format!("{key}: {e}")))
        })
        .collect()
}

/// 只取 metadata：ort 不暴露 ONNX 的自定义 metadata，所以自己用 prost 解一遍。
fn read_metadata(bytes: &[u8]) -> Result<std::collections::HashMap<String, String>, DenoiseError> {
    let proto = onnx_proto::ModelProto::decode(bytes)
        .map_err(|e| DenoiseError::BadMeta(e.to_string()))?;
    Ok(proto
        .metadata_props
        .iter()
        .map(|p| (p.key.clone(), p.value.clone()))
        .collect())
}

/// 极简 ONNX protobuf：只声明 metadata 需要的那两个字段，
/// 避免为了读 metadata 引入整套 onnx proto 定义。
mod onnx_proto {
    #[derive(Clone, PartialEq, prost::Message)]
    pub struct StringStringEntryProto {
        #[prost(string, tag = "1")]
        pub key: String,
        #[prost(string, tag = "2")]
        pub value: String,
    }

    #[derive(Clone, PartialEq, prost::Message)]
    pub struct ModelProto {
        #[prost(int64, tag = "1")]
        pub ir_version: i64,
        #[prost(string, tag = "2")]
        pub producer_name: String,
        #[prost(string, tag = "3")]
        pub producer_version: String,
        #[prost(string, tag = "4")]
        pub domain: String,
        #[prost(int64, tag = "5")]
        pub model_version: i64,
        #[prost(string, tag = "6")]
        pub doc_string: String,
        #[prost(message, repeated, tag = "14")]
        pub metadata_props: Vec<StringStringEntryProto>,
    }
}

impl DenoiseModel {
    /// 从 ONNX 文件加载。
    ///
    /// `runtime` 是 `onnxruntime.dll` 的路径。ort 走 load-dynamic（见 Cargo.toml
    /// 的说明），必须显式指定；传 `None` 时退回 `ort::init()`，让系统按 PATH 找。
    pub fn load(
        path: impl AsRef<Path>,
        runtime: Option<impl AsRef<Path>>,
    ) -> Result<Self, DenoiseError> {
        let path = path.as_ref();
        let bytes = std::fs::read(path)
            .map_err(|e| DenoiseError::Model(format!("读取 {} 失败: {e}", path.display())))?;
        let meta = read_metadata(&bytes)?;

        let n_fft = meta_usize(&meta, "n_fft")?;
        let hop = meta_usize(&meta, "hop_length")?;
        let state_size = meta_usize(&meta, "state_size")?;
        let erb_n = meta_usize(&meta, "erb_norm_state_size")?;
        let spec_n = meta_usize(&meta, "spec_norm_state_size")?;
        let window_type = meta_get(&meta, "window_type")?.to_string();
        if window_type != "vorbis" {
            return Err(DenoiseError::BadMeta(format!(
                "未支持的窗类型 {window_type}（只实现了 vorbis）"
            )));
        }

        let mut state_init = vec![0.0f32; state_size];
        let erb = meta_f32_list(&meta, "erb_norm_init")?;
        let spec = meta_f32_list(&meta, "spec_norm_init")?;
        if erb.len() != erb_n || spec.len() != spec_n || erb_n + spec_n > state_size {
            return Err(DenoiseError::BadMeta(
                "归一化初值长度与 state_size 不匹配".into(),
            ));
        }
        state_init[..erb_n].copy_from_slice(&erb);
        state_init[erb_n..erb_n + spec_n].copy_from_slice(&spec);

        let builder = match runtime {
            Some(p) => ort::init_from(p.as_ref())
                .map_err(|e| DenoiseError::Model(format!("加载 onnxruntime 失败: {e}")))?,
            None => ort::init(),
        };
        let env = builder
            .with_name("airtype-denoise")
            .build()
            .map_err(|e| DenoiseError::Model(e.to_string()))?;
        let session = Session::builder(&env)
            .map_err(|e| DenoiseError::Model(e.to_string()))?
            .with_optimization_level(GraphOptimizationLevel::Level3)
            .map_err(|e| DenoiseError::Model(e.to_string()))?
            .with_intra_threads(1)
            .map_err(|e| DenoiseError::Model(e.to_string()))?
            .commit_from_file(path)
            .map_err(|e| DenoiseError::Model(e.to_string()))?;

        let mut planner = FftPlanner::<f32>::new();
        let fft = planner.plan_fft_forward(n_fft);
        let ifft = planner.plan_fft_inverse(n_fft);

        Ok(Self {
            _env: env,
            session: Mutex::new(session),
            n_fft,
            hop,
            window: vorbis_window(n_fft),
            state_init,
            fft,
            ifft,
        })
    }

    pub fn n_fft(&self) -> usize {
        self.n_fft
    }

    pub fn hop(&self) -> usize {
        self.hop
    }

    pub fn state_len(&self) -> usize {
        self.state_init.len()
    }

    /// 一帧前向：时域帧 → 增强后的时域帧（长度均为 n_fft）。
    fn forward_frame(&self, frame: &[f32], state: &mut [f32]) -> Result<Vec<f32>, DenoiseError> {
        let bins = self.n_fft / 2 + 1;

        let mut buf: Vec<Complex<f32>> = (0..self.n_fft)
            .map(|i| Complex::new(frame[i] * self.window[i], 0.0))
            .collect();
        self.fft.process(&mut buf);

        // 交错成 [1,1,bins,2]（实、虚）
        let mut inter: Vec<f32> = Vec::with_capacity(bins * 2);
        for c in &buf[..bins] {
            inter.push(c.re);
            inter.push(c.im);
        }

        let spec_t = Tensor::from_array(([1usize, 1, bins, 2], inter))
            .map_err(|e| DenoiseError::Infer(e.to_string()))?;
        let state_t = Tensor::from_array(([state.len()], state.to_vec()))
            .map_err(|e| DenoiseError::Infer(e.to_string()))?;

        // outputs 借用 &mut Session，所以拷完数据立刻释放锁。
        let spec_vec: Vec<f32>;
        {
            let mut sess = self
                .session
                .lock()
                .map_err(|e| DenoiseError::Infer(format!("session 锁中毒: {e}")))?;
            let outputs = sess
                .run(ort::inputs![
                    "spec" => spec_t,
                    "state_in" => state_t,
                ])
                .map_err(|e| DenoiseError::Infer(e.to_string()))?;
            let (_, spec_data) = outputs["spec_e"]
                .try_extract_tensor::<f32>()
                .map_err(|e| DenoiseError::Infer(e.to_string()))?;
            let (_, state_data) = outputs["state_out"]
                .try_extract_tensor::<f32>()
                .map_err(|e| DenoiseError::Infer(e.to_string()))?;
            state.copy_from_slice(state_data);
            spec_vec = spec_data.to_vec();
        }

        // 补成共轭对称谱再逆变换
        let mut full: Vec<Complex<f32>> = vec![Complex::new(0.0, 0.0); self.n_fft];
        for k in 0..bins {
            full[k] = Complex::new(spec_vec[2 * k], spec_vec[2 * k + 1]);
        }
        for k in bins..self.n_fft {
            let m = self.n_fft - k;
            full[k] = Complex::new(spec_vec[2 * m], -spec_vec[2 * m + 1]);
        }
        self.ifft.process(&mut full);

        // rustfft 逆变换不归一化，补 1/n；再乘合成窗
        let scale = 1.0 / self.n_fft as f32;
        Ok(full
            .iter()
            .enumerate()
            .map(|(i, c)| c.re * scale * self.window[i])
            .collect())
    }
}

/// 一条音频流的降噪器。
///
/// 对齐官方 `StreamEnhancer` 的**因果**路径：
/// - STFT 不做 padding，帧就是输入缓冲前 n_fft 个样本；
/// - 每帧 ISTFT 后直接累加进输出缓冲，不除窗平方和（靠 Vorbis 窗 COLA）；
/// - 攒满一窗才推理一帧，产出 hop 个样本，然后消耗 hop 继续。
pub struct StreamDenoiser {
    model: Arc<DenoiseModel>,
    state: Vec<f32>,
    /// 输入缓冲：攒满 n_fft 才能推一帧，每帧只消费 hop。
    pending: Vec<f32>,
    /// OLA 输出缓冲，长度 n_fft。
    ola: Vec<f32>,
}

impl StreamDenoiser {
    pub fn new(model: Arc<DenoiseModel>) -> Self {
        let n = model.n_fft;
        let state = model.state_init.clone();
        Self {
            model,
            state,
            pending: Vec::with_capacity(n * 2),
            ola: vec![0.0; n],
        }
    }

    /// 喂一段 S16LE 样本，返回本次已就绪的增强样本（hop 的整数倍，可能为空）。
    ///
    /// 首窗填充期返回偏少，由 `flush()` 补齐；累计满足「输入 N → 输出 N」。
    pub fn process(&mut self, pcm: &[i16]) -> Vec<i16> {
        let mut out: Vec<i16> = Vec::with_capacity(pcm.len());
        for &s in pcm {
            self.pending.push(s as f32 / 32768.0);
        }
        let n = self.model.n_fft;
        let hop = self.model.hop;
        while self.pending.len() >= n {
            let block = self.step().unwrap_or_else(|e| {
                // 降噪失败不该让整条语音链路挂掉：本 hop 输出静音、继续走。
                tracing::warn!("降噪推理失败，本 hop 输出静音: {e}");
                vec![0.0f32; hop]
            });
            for v in block {
                out.push(float_to_i16(v));
            }
            self.pending.drain(..hop);
        }
        out
    }

    /// 流结束：把不足一窗的余量补零凑满，推最后一帧，返回至多 hop 个样本。
    pub fn flush(&mut self) -> Vec<i16> {
        if self.pending.is_empty() {
            return Vec::new();
        }
        let n = self.model.n_fft;
        let hop = self.model.hop;
        if self.pending.len() < n {
            self.pending.resize(n, 0.0);
        }
        let block = self.step().unwrap_or_else(|e| {
            tracing::warn!("降噪 flush 推理失败: {e}");
            vec![0.0f32; hop]
        });
        block.into_iter().take(hop).map(float_to_i16).collect()
    }

    /// 重置流状态。换独立音频段时调用，防止 RNN 状态跨段泄漏。
    pub fn reset(&mut self) {
        self.state = self.model.state_init.clone();
        self.pending.clear();
        self.ola.iter_mut().for_each(|v| *v = 0.0);
    }

    /// 推一帧，返回 hop 个样本。
    fn step(&mut self) -> Result<Vec<f32>, DenoiseError> {
        let n = self.model.n_fft;
        let hop = self.model.hop;
        let frame: Vec<f32> = self.pending[..n].to_vec();

        let time = self.model.forward_frame(&frame, &mut self.state)?;

        // OLA 直接累加：合成窗已在前向里乘过，COLA 保证前 hop 个样本即原信号。
        for i in 0..n {
            self.ola[i] += time[i];
        }
        let out: Vec<f32> = self.ola[..hop].to_vec();

        self.ola.copy_within(hop.., 0);
        for i in (n - hop)..n {
            self.ola[i] = 0.0;
        }
        Ok(out)
    }
}

impl airtype_core::denoise::FrameDenoiser for StreamDenoiser {
    fn process(&mut self, pcm: &[i16]) -> Vec<i16> {
        StreamDenoiser::process(self, pcm)
    }

    fn flush(&mut self) -> Vec<i16> {
        StreamDenoiser::flush(self)
    }
}

fn float_to_i16(v: f32) -> i16 {
    (v.clamp(-1.0, 1.0) * 32767.0).round() as i16
}

/// 一次性降噪整段音频（离线/测试用）。
pub fn denoise_offline(model: &Arc<DenoiseModel>, pcm: &[i16]) -> Vec<i16> {
    let mut d = StreamDenoiser::new(model.clone());
    let mut out = d.process(pcm);
    out.extend(d.flush());
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn model_path() -> std::path::PathBuf {
        let mut p = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        p.pop();
        p.pop();
        p.push("models/dpdfnet/dpdfnet2.onnx");
        p
    }

    #[test]
    fn vorbis_window_is_cola_compliant() {
        let w = vorbis_window(320);
        assert_eq!(w.len(), 320);
        assert!(w[0] < 1e-3, "首样本应接近 0，实际 {}", w[0]);
        assert!(w[319] < 1e-3, "末样本应接近 0，实际 {}", w[319]);
        assert!(w[160] > 0.99, "中点应接近 1，实际 {}", w[160]);
        // 50% 重叠下窗平方和恒为 1 —— 这是 OLA 不除窗平方和的依据
        for i in 0..160 {
            let s = w[i] * w[i] + w[i + 160] * w[i + 160];
            assert!((s - 1.0).abs() < 1e-5, "COLA 在 {i} 处 = {s}");
        }
    }

    #[test]
    fn loads_model_and_reports_params() {
        let p = model_path();
        if !p.exists() {
            eprintln!("跳过：模型不存在 {}", p.display());
            return;
        }
        let m = DenoiseModel::load(&p, None::<&std::path::Path>).expect("加载模型");
        assert_eq!(m.n_fft(), 320);
        assert_eq!(m.hop(), 160);
        assert_eq!(m.state_len(), 45424);
    }

    #[test]
    fn silence_in_silence_out() {
        let p = model_path();
        if !p.exists() {
            eprintln!("跳过：模型不存在");
            return;
        }
        let m = Arc::new(DenoiseModel::load(&p, None::<&std::path::Path>).unwrap());
        let silent = vec![0i16; 16000];
        let out = denoise_offline(&m, &silent);
        assert_eq!(out.len(), silent.len(), "输入输出长度必须一致");
        let peak = out.iter().map(|v| v.abs()).max().unwrap_or(0);
        assert!(peak < 300, "静音不该被放大，实际峰值 {peak}");
    }
}
