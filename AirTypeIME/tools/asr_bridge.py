#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CommandAsr 引擎的桥接脚本示例。

AirTypeIME 在 `asr.engine = "command"` 时会把一段语音写成临时 WAV，
执行配置里的命令模板（{wav} 被替换成带引号的 WAV 路径），
并把 **stdout 第一行** 当作识别结果。

接真实模型时，只需替换 recognize()：
    from faster_whisper import WhisperModel
    model = WhisperModel("small", device="cpu", compute_type="int8")
    segments, _ = model.transcribe(wav_path, language="zh", vad_filter=True)
    return "".join(s.text for s in segments).strip()

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
