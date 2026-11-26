//! 端到端验收：用 Rust 的 StreamDenoiser 处理 noisy.wav，
//! 与 Python 版（dpdfnet_test/enhanced.wav）逐样本对比。
//!
//! 跑法：cargo run --release -p airtype-denoise

use std::path::PathBuf;
use std::sync::Arc;

use airtype_denoise::{denoise_offline, DenoiseModel};

fn read_wav(path: &PathBuf) -> (u32, Vec<i16>) {
    let b = std::fs::read(path).expect("读 wav");
    assert_eq!(&b[0..4], b"RIFF");
    assert_eq!(&b[8..12], b"WAVE");
    let mut pos = 12;
    let mut sr = 0u32;
    let mut data_off = 0usize;
    let mut data_len = 0usize;
    while pos + 8 <= b.len() {
        let id = &b[pos..pos + 4];
        let size = u32::from_le_bytes(b[pos + 4..pos + 8].try_into().unwrap()) as usize;
        if id == b"fmt " {
            sr = u32::from_le_bytes(b[pos + 12..pos + 16].try_into().unwrap());
        } else if id == b"data" {
            data_off = pos + 8;
            data_len = size;
            break;
        }
        pos += 8 + size + (size & 1);
    }
    let samples = b[data_off..data_off + data_len]
        .chunks_exact(2)
        .map(|p| i16::from_le_bytes([p[0], p[1]]))
        .collect();
    (sr, samples)
}

fn write_wav(path: &PathBuf, sr: u32, s: &[i16]) {
    let mut out = Vec::with_capacity(44 + s.len() * 2);
    let data = (s.len() * 2) as u32;
    out.extend_from_slice(b"RIFF");
    out.extend_from_slice(&(36 + data).to_le_bytes());
    out.extend_from_slice(b"WAVEfmt ");
    out.extend_from_slice(&16u32.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&sr.to_le_bytes());
    out.extend_from_slice(&(sr * 2).to_le_bytes());
    out.extend_from_slice(&2u16.to_le_bytes());
    out.extend_from_slice(&16u16.to_le_bytes());
    out.extend_from_slice(b"data");
    out.extend_from_slice(&data.to_le_bytes());
    for v in s {
        out.extend_from_slice(&v.to_le_bytes());
    }
    std::fs::write(path, out).expect("写 wav");
}

fn snr(ref_: &[i16], est: &[i16]) -> f64 {
    let n = ref_.len().min(est.len());
    let r: Vec<f64> = ref_[..n].iter().map(|v| *v as f64 / 32768.0).collect();
    let e: Vec<f64> = est[..n].iter().map(|v| *v as f64 / 32768.0).collect();
    let mean = r.iter().sum::<f64>() / n as f64;
    let sig = r.iter().map(|v| (v - mean).powi(2)).sum::<f64>() / n as f64;
    let err = r.iter().zip(&e).map(|(a, b)| (a - b).powi(2)).sum::<f64>() / n as f64;
    10.0 * (sig / err.max(1e-12)).log10()
}

fn main() {
    // CARGO_MANIFEST_DIR = .../AirTypeIME/crates/airtype-denoise
    // pop 两次 = .../AirTypeIME
    let mut root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    root.pop();
    root.pop();
    let test = root.parent().expect("项目根").join("dpdfnet_test");
    let model_path = root.join("models/dpdfnet/dpdfnet2.onnx");

    let runtime = root.join("libs/onnxruntime.dll");
    println!("模型: {}", model_path.display());
    println!("运行库: {}", runtime.display());
    let m = Arc::new(
        DenoiseModel::load(&model_path, Some(&runtime)).expect("加载模型"),
    );
    println!("n_fft={} hop={}", m.n_fft(), m.hop());

    let (sr, clean) = read_wav(&test.join("clean.wav"));
    let (_, noisy) = read_wav(&test.join("noisy.wav"));
    let (_, py) = read_wav(&test.join("enhanced.wav"));
    println!(
        "采样率={sr} 长度 clean={} noisy={} py={}",
        clean.len(),
        noisy.len(),
        py.len()
    );

    // 带参数时只做「降噪 → 存盘」，方便把结果直接喂给 ASR 做 A/B。
    if let Some(inp) = std::env::args().nth(1) {
        let (isr, data) = read_wav(&PathBuf::from(&inp));
        let mut d = airtype_denoise::StreamDenoiser::new(m.clone());
        let mut out = airtype_denoise::StreamDenoiser::process(&mut d, &data);
        out.extend(airtype_denoise::StreamDenoiser::flush(&mut d));
        let stem = PathBuf::from(&inp)
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_else(|| "out".into());
        let dst = test.join(format!("enh_{stem}.wav"));
        write_wav(&dst, isr, &out);
        println!("已写出 {}（{} 样本）", dst.display(), out.len());
        return;
    }

    let t0 = std::time::Instant::now();
    let rust = denoise_offline(&m, &noisy);
    let dt = t0.elapsed().as_secs_f64();
    let audio_s = noisy.len() as f64 / sr as f64;
    println!(
        "Rust 处理 {} 样本耗时 {:.2}s（音频 {:.2}s，RTF={:.3}）",
        noisy.len(),
        dt,
        audio_s,
        dt / audio_s
    );

    write_wav(&test.join("enhanced_rust.wav"), sr, &rust);

    // ---- 分片 vs 一次性：实时链路每帧只喂 320 样本（20ms），必须与整段等价 ----
    {
        let mut d2 = airtype_denoise::StreamDenoiser::new(m.clone());
        let mut chunked: Vec<i16> = Vec::with_capacity(noisy.len());
        for ch in noisy.chunks(320) {
            chunked.extend(airtype_denoise::StreamDenoiser::process(&mut d2, ch));
        }
        let n = chunked.len().min(rust.len());
        let mut mdiff = 0i32;
        for i in 0..n {
            mdiff = mdiff.max((chunked[i] as i32 - rust[i] as i32).abs());
        }
        println!(
            "\n== 分片(320/次) vs 一次性 ==\n  长度 chunked={} oneshot={}\n  最大偏差={}",
            chunked.len(),
            rust.len(),
            mdiff
        );
        if n > 0 {
            let rms = |v: &[i16]| {
                (v[..n].iter().map(|s| (*s as f64) * (*s as f64)).sum::<f64>() / n as f64).sqrt()
            };
            println!("  RMS chunked={:.0} oneshot={:.0}", rms(&chunked), rms(&rust));
        }
    }

    // 模型输出相对输入滞后 640 个样本（内部延迟）。流式版不丢（实时无感），
    // 离线对齐时把它前移 640 即可 —— 这正是官方离线版 `y[win_len*2:]` 做的事。
    let lag = 640usize;
    let rust_aligned: Vec<i16> = if rust.len() > lag {
        rust[lag..].to_vec()
    } else {
        Vec::new()
    };

    println!("\n== 质量对比（相对 clean，已补偿 640 样本）==");
    println!("  noisy         SNR = {:6.2} dB", snr(&clean, &noisy));
    println!("  Python 降噪   SNR = {:6.2} dB", snr(&clean, &py));
    println!("  Rust   降噪   SNR = {:6.2} dB  <-- 本次实现", snr(&clean, &rust_aligned));

    println!("\n== Rust vs Python 逐样本 ==");
    let rust = rust_aligned;
    let n = rust.len().min(py.len());
    let mut max_diff = 0i32;
    let mut sum_sq = 0f64;
    for i in 0..n {
        let d = (rust[i] as i32 - py[i] as i32).abs();
        max_diff = max_diff.max(d);
        sum_sq += (d as f64).powi(2);
    }
    println!("  长度 Rust={} Python={}", rust.len(), py.len());
    println!("  最大偏差 = {max_diff} (int16 满量程 32768)");
    println!("  均方根偏差 = {:.1}", (sum_sq / n as f64).sqrt());
}
