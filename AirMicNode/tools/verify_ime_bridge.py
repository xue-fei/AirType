#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""无硬件联调：用「与固件完全一致」的帧序列打真实 AirTypeIME。

复用 AirTypeIME/tools/mock_airmic.py 里零依赖的 WebSocket 客户端，
但帧的构造严格对齐固件：
  - START 元数据与 src/protocol.cpp 的 startMeta() 逐字节一致
  - seq 全局连续递增（不按 PTT 段重置），含回绕前的多段会话
  - 20ms / 640B PCM 帧、END 前排空、CANCEL、PING/PONG

用法：
    python tools/verify_ime_bridge.py --host 127.0.0.1
    python tools/verify_ime_bridge.py --host 192.168.2.177 --port 8765
"""

import argparse
import json
import socket
import sys
import time
from pathlib import Path

_AIRTYPE_TOOLS = Path(__file__).resolve().parents[2] / "AirTypeIME" / "tools"
sys.path.insert(0, str(_AIRTYPE_TOOLS))

from mock_airmic import (  # noqa: E402
    KIND_AUDIO, KIND_CANCEL, KIND_END, KIND_PING, KIND_PONG,
    KIND_START, KIND_STATUS, KIND_TEXT, WebSocketClient,
    make_frame, parse_frame,
)

SAMPLES_PER_FRAME = 320
PCM_BYTES_PER_FRAME = SAMPLES_PER_FRAME * 2  # 640
DEVICE_ID = "airmic-a1b2c3"


def firmware_start_meta(device=DEVICE_ID):
    """必须与 src/protocol.cpp 的 airmic::startMeta() 输出顺序一致。"""
    return (
        '{"device":"%s","codec":"pcm_s16le",'
        '"sample_rate":16000,"channels":1,"bits_per_sample":16}' % device
    ).encode("utf-8")


class FirmwareFrameStream:
    """模拟固件的 nextSeq()：uint16 连续递增，跨 PTT 段不重置。"""

    def __init__(self, start_seq=0):
        self.seq = start_seq & 0xFFFF

    def emit(self, kind, payload=b""):
        frame = make_frame(kind, self.seq, payload)
        self.seq = (self.seq + 1) & 0xFFFF
        return frame


def ptt_sequence(stream, audio_frames, tail=KIND_END):
    out = [stream.emit(KIND_START, firmware_start_meta())]
    for _ in range(audio_frames):
        out.append(stream.emit(KIND_AUDIO, bytes(PCM_BYTES_PER_FRAME)))
    out.append(stream.emit(tail))
    return out


def collect(ws, timeout_s=8.0):
    """收 STATUS / TEXT / PONG；出现终态后再留 0.5s 收尾帧即退出。"""
    states, texts, pongs = [], [], 0
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            opcode, payload = ws.recv_frame()
        except socket.timeout:
            break  # 收帧窗口结束，正常退出
        except (OSError, RuntimeError) as e:
            print("  接收中断: %s" % e, file=sys.stderr)
            break
        if opcode != 0x2:
            continue
        kind, body = parse_frame(payload)
        if kind == KIND_STATUS:
            status = json.loads(body.decode("utf-8"))
            state = status.get("state", "?")
            states.append(state)
            print("  [STATUS] %s" % json.dumps(status, ensure_ascii=False))
            if state in ("done", "cancelled", "error"):
                deadline = min(deadline, time.time() + 0.5)
        elif kind == KIND_TEXT:
            texts.append(body.decode("utf-8"))
            print("  [TEXT]   %s" % texts[-1])
        elif kind == KIND_PONG:
            pongs += 1
    return states, texts, pongs


def wait_until_ready(host, port, path, timeout_s=6.0):
    """用一次完整 WS 握手等服务就绪，避免裸 TCP 探测让服务端报握手失败。"""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            ws = WebSocketClient(host, port, path, timeout=1.0)
            ws.send_close()
            ws.close()
            return True
        except (OSError, RuntimeError):
            time.sleep(0.2)
    return False


def run_case(name, frames, host, port, path, expect_state):
    print("\n=== %s ===" % name)
    ws = WebSocketClient(host, port, path, timeout=5.0)
    try:
        for frame in frames:
            ws.send_binary(frame)
        print("  发出 %d 帧 binary / seq 连续递增" % len(frames))
        states, texts, pongs = collect(ws)
        ws.send_close()
    finally:
        ws.close()

    ok = expect_state in states
    if expect_state == "done":
        ok = ok and bool(texts)
    if expect_state == "cancelled":
        ok = ok and not texts

    print("  结果: %s (states=%s, texts=%d, pongs=%d)"
          % ("PASS" if ok else "FAIL", states, len(texts), pongs))
    return ok, states, texts, pongs


def main():
    ap = argparse.ArgumentParser(description="验证固件帧序列与 AirTypeIME 的兼容性")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--path", default="/ws")
    args = ap.parse_args()

    print("目标 ws://%s:%d%s" % (args.host, args.port, args.path))
    if not wait_until_ready(args.host, args.port, args.path):
        print("AirTypeIME 未就绪，请先在 PC 端启动 airtype-daemon", file=sys.stderr)
        return 1
    print("服务已就绪")
    print("START 元数据: %s" % firmware_start_meta().decode())

    stream = FirmwareFrameStream(start_seq=0xFFFE)  # 顺带验证 u16 回绕
    results = []

    # 1) 正常 PTT：20 帧音频（400ms）+ END + 心跳
    normal = ptt_sequence(stream, 20, KIND_END)
    normal.append(stream.emit(KIND_PING))
    ok, states, _, pongs = run_case(
        "正常 PTT（含 seq 回绕与 PING）", normal,
        args.host, args.port, args.path, "done")
    results.append(("正常 PTT", ok and "recognizing" in states))

    # 2) 新连接上的第二段 PTT：seq 继续递增不重置，验证服务端不报 error
    second = ptt_sequence(stream, 5, KIND_END)
    ok, states, texts, _ = run_case(
        "第二段 PTT（seq 连续，不重置）", second,
        args.host, args.port, args.path, "done")
    results.append(("连续 seq 多段会话", ok and bool(texts) and "error" not in states))

    # 3) 短按取消：START + 2 帧 AUDIO + CANCEL，不应出现 TEXT
    cancel = ptt_sequence(stream, 2, KIND_CANCEL)
    ok, states, texts, _ = run_case(
        "短按取消", cancel,
        args.host, args.port, args.path, "cancelled")
    results.append(("短按取消", ok and not texts))

    print("\n================ 汇总 ================")
    for name, ok in results:
        print("  %-22s %s" % (name, "PASS" if ok else "FAIL"))
    all_ok = all(ok for _, ok in results)
    print("固件帧序列与 AirTypeIME %s" % ("兼容" if all_ok else "不兼容"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
