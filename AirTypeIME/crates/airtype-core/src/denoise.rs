//! 音频降噪的抽象接口。
//!
//! core 只定义 trait，不依赖任何 native 推理引擎——和 `AsrEngine` /
//! `StreamingAsr` 的做法一致：真正的实现（DPDFNet + onnxruntime）在
//! `airtype-denoise` crate 里，由 daemon 按配置装配后注入。

/// 逐帧音频增强器（降噪）。
///
/// 语义要求和实现约定：
/// - `process` 可以一次吃任意长度，内部自行缓冲；返回**已就绪**的样本，
///   长度可能小于输入（首窗填充期），调用方按顺序拼接即可；
/// - `flush` 在 PTT 结束（End）时调用，把缓冲里的余量补齐并返回；
/// - 同一段语音内状态（RNN / OLA）必须连续，跨段要 `reset`。实现方通常
///   一个 PTT 段 new 一个实例，所以 trait 不强制 `reset`。
///
/// 为什么把降噪放在 Session 而不是 Pipeline：Session 是音频进入系统的
/// 第一站，处理完再过 ASR 和音量表，两处都基于增强后的信号，语义一致。
pub trait FrameDenoiser: Send {
    /// 喂一段 S16LE 样本，返回本次可用的增强样本。
    fn process(&mut self, pcm: &[i16]) -> Vec<i16>;

    /// 流结束时排空内部缓冲，返回尾部残留样本。
    fn flush(&mut self) -> Vec<i16>;
}
