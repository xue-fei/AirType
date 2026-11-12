#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""模拟 AirMic Node：通过 Wi-Fi WebSocket 向 AirTypeIME 推裸 PCM 帧。

零第三方依赖，只用标准库实现 WebSocket 客户端握手与帧收发，
便于在没有硬件时验证 START -> AUDIO... -> END 的完整链路。

用法：
    python tools/mock_airmic.py
    python tools/mock_airmic.py --wav sample.wav --fast
    python tools/mock_airmic.py --cancel
    python tools/mock_airmic.py --host 192.168.2.177 --duration-ms 2000
"""

import argparse
import base64
import json
import math
import os
import socket
import struct
import sys
import time
import wave

MAGIC = 0xA1
VERSION = 0x01

KIND_START = 0x01
KIND_AUDIO = 0x02
KIND_END = 0x03
KIND_CANCEL = 0x04
KIND_PING = 0x05
KIND_PONG = 0x06
KIND_STATUS = 0x81
KIND_TEXT = 0x82

SAMPLE_RATE = 16000
FRAME_MS = 20
SAMPLES_PER_FRAME = SAMPLE_RATE * FRAME_MS // 1000  # 320 samples = 640 bytes


def make_frame(kind: int, seq: int, payload: bytes = b"") -> bytes:
    return struct.pack("<BBBBHH", MAGIC, VERSION, kind, 0, seq & 0xFFFF, len(payload)) + payload


def parse_frame(payload: bytes):
    if len(payload) < 8:
        return None, b""
    magic, version, kind, flags, seq, length = struct.unpack("<BBBBHH", payload[:8])
    if magic != MAGIC or version != VERSION or length != len(payload) - 8:
        return None, b""
    return kind, payload[8:]


class WebSocketClient:
    """极简 RFC6455 客户端：仅支持 binary 帧、客户端掩码、同步收发。"""

    def __init__(self, host: str, port: int, path: str, timeout: float = 10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(request.encode("ascii"))
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("握手期间连接关闭")
            buf += chunk
        head, _, remainder = buf.partition(b"\r\n\r\n")
        if b"101" not in head.split(b"\r\n")[0]:
            raise RuntimeError("WebSocket 握手失败: " + head.decode("utf-8", "replace"))
        self.buf = remainder
        self.sock.settimeout(timeout)

    def _read_exact(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("连接已关闭")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send_binary(self, data: bytes) -> None:
        mask = os.urandom(4)
        n = len(data)
        if n < 126:
            header = struct.pack("!BB", 0x82, 0x80 | n)
        elif n < 65536:
            header = struct.pack("!BBH", 0x82, 0x80 | 126, n)
        else:
            header = struct.pack("!BBQ", 0x82, 0x80 | 127, n)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.sock.sendall(header + mask + masked)

    def recv_frame(self):
        b1, b2 = self._read_exact(2)
        opcode = b1 & 0x0F
        n = b2 & 0x7F
        if n == 126:
            n = struct.unpack("!H", self._read_exact(2))[0]
        elif n == 127:
            n = struct.unpack("!Q", self._read_exact(8))[0]
        mask = self._read_exact(4) if b2 & 0x80 else None
        payload = self._read_exact(n)
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return opcode, payload

    def send_close(self) -> None:
        """发 RFC6455 Close 帧，让服务端正常收尾，避免日志里出现读错误。"""
        try:
            self.sock.sendall(struct.pack("!BB", 0x88, 0x80) + os.urandom(4))
        except OSError:
            pass

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def tone_chunks(duration_ms: int, frame_ms: int, freq: int):
    """生成 16kHz / 16bit / mono 的正弦波 PCM 帧。"""
    samples_per_frame = SAMPLE_RATE * frame_ms // 1000
    total = duration_ms // frame_ms
    phase = 0
    for _ in range(max(1, total)):
        buf = bytearray()
        for i in range(samples_per_frame):
            value = int(3000 * math.sin(2 * math.pi * freq * (phase + i) / SAMPLE_RATE))
            buf += struct.pack("<h", value)
        phase += samples_per_frame
        yield bytes(buf)


def wav_chunks(path: str, frame_ms: int):
    """按帧切分真实 WAV；v1 不做重采样，只校验格式。"""
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2 or w.getframerate() != SAMPLE_RATE:
            raise SystemExit("WAV 必须是 16kHz / 16bit / 单声道（v1 不做重采样）")
        samples_per_frame = SAMPLE_RATE * frame_ms // 1000
        while True:
            data = w.readframes(samples_per_frame)
            if not data:
                break
            if len(data) % 2:
                data = data[:-1]
            yield data


def main() -> int:
    ap = argparse.ArgumentParser(description="模拟 AirMic Node 推流")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--path", default="/ws")
    ap.add_argument("--device", default="airmic-mock-01")
    ap.add_argument("--duration-ms", type=int, default=1000, help="模拟按住 PTT 的时长")
    ap.add_argument("--frame-ms", type=int, default=FRAME_MS)
    ap.add_argument("--freq", type=int, default=440, help="模拟音频的正弦波频率")
    ap.add_argument("--wav", help="改用真实 WAV 文件（16kHz/16bit/mono）")
    ap.add_argument("--fast", action="store_true", help="不按实时节奏 sleep，尽快推完")
    ap.add_argument("--cancel", action="store_true", help="发 CANCEL 而不是 END")
    ap.add_argument("--timeout", type=float, default=15.0)
    args = ap.parse_args()

    meta = {
        "device": args.device,
        "codec": "pcm_s16le",
        "sample_rate": SAMPLE_RATE,
        "channels": 1,
        "bits_per_sample": 16,
    }

    chunks = list(
        wav_chunks(args.wav, args.frame_ms)
        if args.wav
        else tone_chunks(args.duration_ms, args.frame_ms, args.freq)
    )

    print(f"连接 ws://{args.host}:{args.port}{args.path} ...")
    try:
        ws = WebSocketClient(args.host, args.port, args.path, args.timeout)
    except (OSError, RuntimeError) as e:
        print(f"连接失败: {e}", file=sys.stderr)
        return 1

    try:
        seq = 0
        ws.send_binary(make_frame(KIND_START, seq, json.dumps(meta).encode("utf-8")))
        seq += 1
        print(f"START  device={args.device}  codec=pcm_s16le  {SAMPLE_RATE}Hz/16bit/mono")

        for chunk in chunks:
            ws.send_binary(make_frame(KIND_AUDIO, seq, chunk))
            seq = (seq + 1) & 0xFFFF
            if not args.fast:
                time.sleep(args.frame_ms / 1000.0)
        print(f"AUDIO  {len(chunks)} 帧 / {sum(len(c) // 2 for c in chunks)} samples")

        tail_kind = KIND_CANCEL if args.cancel else KIND_END
        ws.send_binary(make_frame(tail_kind, seq))
        print("CANCEL" if args.cancel else "END")

        deadline = time.time() + args.timeout
        done_seen = False
        while time.time() < deadline:
            try:
                opcode, payload = ws.recv_frame()
            except (socket.timeout, OSError, RuntimeError) as e:
                print(f"等待响应结束: {e}", file=sys.stderr)
                break
            if opcode != 0x2:
                continue
            kind, body = parse_frame(payload)
            if kind == KIND_STATUS:
                status = json.loads(body.decode("utf-8"))
                print("[STATUS]", json.dumps(status, ensure_ascii=False))
                state = status.get("state")
                if state in ("cancelled", "error"):
                    break
                if state == "done":
                    # 服务端随后还会发一帧 TEXT；空语音段则没有，最多再等 1 秒。
                    done_seen = True
                    deadline = time.time() + 1.0
            elif kind == KIND_TEXT:
                print("[TEXT]  ", body.decode("utf-8"))
                if done_seen:
                    break
            elif kind == KIND_PONG:
                print("[PONG]")
        return 0
    finally:
        ws.send_close()
        ws.close()


if __name__ == "__main__":
    sys.exit(main())
