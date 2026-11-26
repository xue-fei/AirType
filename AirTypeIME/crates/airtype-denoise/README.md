# airtype-denoise

DPDFNet 16 kHz 流式语音增强。推理用 `ort`（onnxruntime，静态链接），STFT/ISTFT 用 `rustfft` 手写。

## 状态：已接入 pipeline，端到端验证通过

| 指标 | 值 |
|---|---|
| RTF | **0.198**（10.05 秒音频 1.99 秒，约 5 倍实时余量，单线程） |
| 降噪效果 | 10 dB → **20.57 dB** SNR（与官方 Python 实现完全一致） |
| 与 Python 逐样本偏差 | 最大 23 / 32768，均方根 2.4 |
| 固定延迟 | 首窗填充 160 样本（10ms）+ 模型内部 640 样本（实时无需补偿） |
| 模型 | `dpdfnet2.onnx` 16 kHz，9.8 MB，Apache 2.0 |

## 算法：对齐官方 StreamEnhancer 的因果路径

- STFT **不做 padding**，帧就是输入缓冲前 `n_fft` 个样本
- 每帧 ISTFT 后**直接累加**进输出缓冲，**不除窗平方和**——Vorbis 窗满足 50% 重叠 COLA（`w[n]²+w[n+hop]²=1`），前 hop 个样本累加完即完全重建
- 攒满一窗才推一帧，产出 hop 个样本，然后消费 hop 继续
- `flush()` 补零凑整窗推最后一帧，只取 hop 个输出

> 参考实现：`github.com/xue-fei/dpdfnet-unity` 的 `DpdfNetProcessor.ProcessCore`。
> 关键修正来自它——之前照搬离线版（`center=True` + reflect padding + 丢 640 延迟补偿）
> 会让整段音频错位 40ms，幅度正常但 SNR 崩到 -3。

## 为什么不用 tract

先试过纯 Rust 的 `tract-onnx`（零 DLL 依赖），算子能全部加载，但：

| 配置 | 数值 | 速度 |
|---|---|---|
| `into_optimized()` | ❌ 语音帧 `spec_e` 被压小 **20~200 倍**（静音帧正常） | RTF 0.24 |
| `into_typed()`（不优化） | ✅ 与 onnxruntime 逐帧完全一致 | **RTF 91**（10 秒音频 >15 分钟）|

tract 0.21 没有 `into_decluttered()`，"只清理不替换"的中间档位缺失；自定义优化 pipeline 要 `tract-pulse` 的 `Optimizer`，tract-onnx 未暴露入口。最终回退 ort。

## 体积

onnxruntime 静态链接，`airtype-denoise.exe` 22.8 MB，**无需额外 dll**。
接进 `airtype-ffi` 后该 DLL 预计从 20.9 MB 增至约 40 MB（模型另算 9.8 MB）。

## 用法

```rust
let model = Arc::new(DenoiseModel::load("models/dpdfnet/dpdfnet2.onnx")?);
let mut d = StreamDenoiser::new(model);
let out = d.process(&pcm_in);   // 输入 S16LE，输出长度略少
let tail = d.flush();           // 补齐尾部
```

## 接入位置

`Session` 的 Audio 分支先降噪再入库（`FrameKind::Audio` → `PcmBuffer`），
END 时调 `flush()` 补齐尾部。这样 ASR 和音量表看到的都是增强后的信号。

- core 只定义 `trait FrameDenoiser`（不依赖 native），daemon 按 `[denoise] enabled` 注入
- 每条 WebSocket 连接 new 一个 `StreamDenoiser`（RNN/OLA 状态必须独立）
- 模型加载失败只降级为"不降噪"，不影响 ASR

## 打包

- 模型 `models\dpdfnet\dpdfnet2.onnx`（9.7MB）
- 运行库 `lib\onnxruntime.dll`（18.4MB，**必须动态加载**）
- 安装包从 275.7MB 增至 288.8MB

> **为什么必须动态加载**：`sherpa-onnx-sys` 用静态 CRT(MT) 且已静态打包了一份
> onnxruntime，`ort` 若也静态链接会与它符号重定义 + CRT 不匹配（LNK2005 / LNK2038）。

## 验证

```
INFO airtype_daemon::server: 降噪已启用（DPDFNet）
推 10dB 噪声音频 → 识别：昨天是 Monday After Tomorrow是星期三。
PARTIAL 逐字：昨 → 昨天 → 昨天是 → 昨天是 Monday → ...
```

