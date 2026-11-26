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

`Pipeline::on_frame` 的**入口**（不是 Session）：构造增强帧后同时喂给
Session（离线缓冲）与流式 ASR。

> 这点踩过坑：最初挂在 `Session` 的 Audio 分支，只影响离线路径 ——
> `on_frame_streaming` 直接用原始 `frame.payload` 喂流式 ASR，于是
> 「六档 SNR 开/关降噪识别结果逐字相同」，差点得出"降噪对 ASR 无帮助"的错误结论。

- core 只定义 `trait FrameDenoiser`（不依赖 native），daemon 按 `[denoise] enabled` 注入
- 每条 WebSocket 连接 new 一个 `StreamDenoiser`（RNN/OLA 状态必须独立）
- 模型加载失败只降级为"不降噪"，不影响 ASR

## 抑制上限 attn_limit_db（关键）

模型本身没有可调参数，但官方 `audio.py::apply_attn_limit` 在**频谱域**做干湿混合：

```python
alpha = 10 ** (-attn_limit_db / 20)
out = alpha * noisy_spec + (1 - alpha) * enhanced_spec
```

`0` = 完全不降噪，数值越大越接近纯模型输出。实现时有两个坑：

1. **alpha == 1 不能跳过混合**。跳过会变成"用纯模型输出"，语义正好反了
   （第一版就是这么错的：`attn=0` 反而最狠）。
2. **必须用 4 帧前的输入谱**。官方 `ATTN_LIMIT_NOISY_FRAME_OFFSET = 4`：
   模型输出比输入谱晚 4 帧（640 样本），拿当前帧去混等于把两个错位信号叠加，
   会产生梳状失真（实测 SNR 从 26dB 掉到 0dB）。所以 `StreamDenoiser` 缓存
   最近 4 帧输入谱。

### 实测：最优值是 3

同一条 10 秒中英混说，CER 相对干净音频（越低越好）：

| 输入 | 不降噪 | attn=0 | **attn=3** | attn=6 | attn=12 | 全降噪 |
|---|---|---|---|---|---|---|
| clean | 0% | 0% | **0%** | **0%** | 2.4% | 26.2% |
| 15 dB | 31.0% | 31.0% | **19.0%** | 11.9% | 19.0% | 16.7% |
| 5 dB | 69.0% | 66.7% | **21.4%** | 42.9% | 35.7% | 31.0% |
| 平均 | 33.3% | 32.6% | **13.5%** | 18.3% | 19.0% | 24.6% |

结论：
- **attn=3 是甜点**：干净语音零损失，5dB 噪声下 CER 从 69% 降到 21%
- **全降噪（None/几百 dB）对干净语音是灾难**（0% → 26%），不能作默认
- 3~6 都可用，取 3 更稳（5dB 档明显更好）

默认值即 `3.0`（`config.rs::default_attn_limit_db`），老配置不写这行也自动生效。

## 打包

- 模型 `models\dpdfnet\dpdfnet2.onnx`（9.7MB）
- 运行库 `lib\onnxruntime.dll`（18.4MB，**必须动态加载**）
- 安装包从 275.7MB 增至 288.9MB

> **为什么必须动态加载**：`sherpa-onnx-sys` 用静态 CRT(MT) 且已静态打包了一份
> onnxruntime，`ort` 若也静态链接会与它符号重定义 + CRT 不匹配（LNK2005 / LNK2038）。

## 验证

```
# 默认 attn=3，daemon 内降噪
clean.wav → 昨天是 Monday Today Is 礼拜二 The Day After Tomorrow 是星期三。   (与不降噪一致)
snr5.wav  → 昨天是 Monday 礼拜二 Today After Tomorrow 是星期三。              (不降噪只有"Monday 吧是星期三。")
```

`attn=0` 的 SNR 会精确还原出名义值（15.03 / 5.02 dB），说明 STFT/ISTFT 重建无损。
