#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CommandAsr 引擎的桥接脚本示例。

AirTypeIME 在 `asr.engine = "command"` 时会把一段语音写成临时 WAV，
执行配置里的命令模板（{wav} 被替换成带引号的 WAV 路径），
并把 **stdout 第一行** 当作识别结果。

这里只是 `command` 引擎的桥接示例：每段语音都会冷启动一个进程，
因此**生产环境请用 `asr.engine = "sherpa"`**（内嵌 sherpa-onnx，模型在
daemon 进程内加载，见 README 第 6 节）。

要在 command 模式下接模型，替换 recognize() 即可，例如 sherpa-onnx：

    import sherpa_onnx, soundfile as sf
    rec = sherpa_onnx.OfflineRecognizer.from_transducer(
        encoder="models/.../encoder.onnx", decoder="models/.../decoder.onnx",
        joiner="models/.../joiner.onnx", tokens="models/.../tokens.txt")
    audio, sr = sf.read(wav_path, dtype="float32")
    s = rec.create_stream(); s.accept_waveform(sr, audio); rec.decode_stream(s)
    return s.result.text.strip()

也可以在这里调云端 ASR SDK / HTTP API，Rust 侧完全不用改。
"""

import sys
import wave


def recognize(wav_path: str) -> str:
    with wave.open(wav_path, "rb") as w:
        rate = w.getframerate()
        frames = w.getnframes()
        channels = w.getnchannels()
        width = w.getsampwidth()
    duration = frames / rate if rate else 0.0
    return (
        f"识别到一段 {duration:.2f} 秒的语音"
        f"（{rate}Hz / {channels}ch / {width * 8}bit），请接入真实 ASR 模型。"
    )


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: asr_bridge.py <wav>", file=sys.stderr)
        return 2
    print(recognize(sys.argv[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
