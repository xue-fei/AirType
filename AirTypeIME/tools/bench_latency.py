#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""AirTypeIME 端到端延迟基准。

模拟 AirMic 的 PTT 时序，测量「松键发 END -> 收到 TEXT」的客户端体感延迟，
并打印服务端 STATUS 里各阶段耗时（ASR / 后处理 / 注入），定位瓶颈。

流式模式下额外统计**首字延迟**（START -> 第一个非空 partial）与每次 PTT 的
partial 次数——实时上屏能力的核心指标。

需要先启动 daemon（sherpa 引擎把模型加载在进程内，不需要额外服务）：

    # 终端 1：daemon（流式 zipformer fp32）
    cargo run -p airtype-daemon -- config/airtype-sherpa.toml

    # 终端 2：基准
    python tools/bench_latency.py --count 10 --duration-ms 1500
    python tools/bench_latency.py --wav sample.wav --count 5   # 真实语音

目标：松键后 0.3~1 秒内文字出现在输入框。默认 --target-ms 1000。
"""

import argparse
import json
import select
import socket
import statistics
import sys
import threading
import time

from mock_airmic import (
    FRAME_MS,
    KIND_AUDIO,
    KIND_END,
    KIND_START,
    KIND_STATUS,
    KIND_TEXT,
    SAMPLE_RATE,
    WebSocketClient,
    make_frame,
    parse_frame,
    tone_chunks,
    wav_chunks,
)


def percentile(values, p):
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = min(len(ordered) - 1, int(round((len(ordered) - 1) * p / 100.0)))
    return ordered[idx]


def ptt_once(ws, seq, chunks, meta_bytes, timeout, realtime=False, frame_ms=FRAME_MS):
    """发一段 PTT，边推边读。

    返回 (客户端体感 ms, STATUS dict, 文本, 下一个 seq, 首字 ms, partial 数)。
    首字延迟从 START 算起——流式模式下第一段文字在松键**之前**就到了，
    这正是「实时上屏」的核心指标。

    默认尽快推完（测吞吐与极限延迟）；`realtime=True` 时按真实节奏发帧，
    此时「松键→文字」才对应实际 PTT 的体感延迟。
    """
    t_start = time.perf_counter()
    ws.send_binary(make_frame(KIND_START, seq, meta_bytes))
    seq = (seq + 1) & 0xFFFF

    box = {"status": None, "text": None, "first_partial": None, "partials": 0}
    done = threading.Event()

    def reader():
        # select 轮询而非调小套接字超时：主线程还要用同一个 socket 发送。
        while not done.is_set():
            try:
                ready, _, _ = select.select([ws.sock], [], [], 0.2)
            except (OSError, ValueError):
                break
            if not ready:
                continue
            try:
                opcode, payload = ws.recv_frame()
            except socket.timeout:
                continue
            except (OSError, RuntimeError):
                break
            if opcode != 0x2:
                continue
            kind, body = parse_frame(payload)
            if kind == KIND_STATUS:
                s = json.loads(body.decode("utf-8"))
                state = s.get("state")
                if state == "partial":
                    box["partials"] += 1
                    if box["first_partial"] is None and s.get("text", "").strip():
                        box["first_partial"] = time.perf_counter()
                elif state in ("done", "cancelled", "error"):
                    box["status"] = s
                    done.set()
            elif kind == KIND_TEXT:
                box["text"] = body.decode("utf-8", "replace")
                if box["status"] is not None:
                    done.set()

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    for chunk in chunks:
        ws.send_binary(make_frame(KIND_AUDIO, seq, chunk))
        seq = (seq + 1) & 0xFFFF
        if realtime:
            time.sleep(frame_ms / 1000.0)

    t_end = time.perf_counter()
    ws.send_binary(make_frame(KIND_END, seq))
    seq = (seq + 1) & 0xFFFF

    if not done.wait(timeout):
        raise RuntimeError("等待响应超时")
    done.set()
    t.join(timeout=2.0)

    client_ms = (time.perf_counter() - t_end) * 1000.0
    first_partial_ms = (
        (box["first_partial"] - t_start) * 1000.0
        if box["first_partial"] is not None
        else None
    )
    return (
        client_ms,
        box["status"],
        box["text"],
        seq,
        first_partial_ms,
        box["partials"],
    )


def main():
    ap = argparse.ArgumentParser(description="AirTypeIME 端到端延迟基准")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--path", default="/ws")
    ap.add_argument("--device", default="airmic-bench-01")
    ap.add_argument("--count", type=int, default=10, help="计入统计的 PTT 次数")
    ap.add_argument("--warmup", type=int, default=1, help="预热次数，不计入统计")
    ap.add_argument("--duration-ms", type=int, default=1500, help="模拟语音时长")
    ap.add_argument("--frame-ms", type=int, default=FRAME_MS)
    ap.add_argument("--freq", type=int, default=440)
    ap.add_argument("--wav", help="改用真实 WAV（16kHz/16bit/mono）作为语音内容")
    ap.add_argument("--timeout", type=float, default=60.0, help="单次等待识别的超时")
    ap.add_argument(
        "--realtime",
        action="store_true",
        help="按真实节奏发音频帧（默认尽快推完，用来测吞吐极限）",
    )
    ap.add_argument("--target-ms", type=float, default=1000.0, help="松键延迟目标")
    ap.add_argument("--json-out", help="把原始结果写成 JSON 文件")
    ap.add_argument("--strict", action="store_true", help="P95 超目标时以退出码 1 结束")
    args = ap.parse_args()

    meta = json.dumps(
        {
            "device": args.device,
            "codec": "pcm_s16le",
            "sample_rate": SAMPLE_RATE,
            "channels": 1,
            "bits_per_sample": 16,
        }
    ).encode("utf-8")

    chunks = list(
        wav_chunks(args.wav, args.frame_ms)
        if args.wav
        else tone_chunks(args.duration_ms, args.frame_ms, args.freq)
    )
    if not chunks:
        print("没有可发送的音频帧", file=sys.stderr)
        return 2
    audio_ms = sum(len(c) // 2 for c in chunks) * 1000 / SAMPLE_RATE

    print("目标 ws://%s:%d%s" % (args.host, args.port, args.path))
    print(
        "每段 %d 帧 / %.0f ms 音频，预热 %d 次，统计 %d 次"
        % (len(chunks), audio_ms, args.warmup, args.count)
    )
    try:
        ws = WebSocketClient(args.host, args.port, args.path, args.timeout)
    except (OSError, RuntimeError) as e:
        print("连接失败: %s" % e, file=sys.stderr)
        return 1

    results = []
    seq = 0
    try:
        for i in range(args.warmup + args.count):
            warm = i < args.warmup
            try:
                client_ms, status, text, seq, first_partial_ms, partials = ptt_once(
                    ws, seq, chunks, meta, args.timeout, args.realtime, args.frame_ms
                )
            except RuntimeError as e:
                print("第 %d 次失败: %s" % (i + 1, e), file=sys.stderr)
                return 1
            t = (status or {}).get("timings") or {}
            state = (status or {}).get("state", "?")
            if state != "done":
                print(
                    "第 %d 次状态异常: %s %s"
                    % (i + 1, state, (status or {}).get("message", "")),
                    file=sys.stderr,
                )
                return 1
            record = {
                "warmup": warm,
                "client_ms": round(client_ms, 1),
                "first_partial_ms": (
                    round(first_partial_ms, 1) if first_partial_ms is not None else None
                ),
                "partials": partials,
                "asr_ms": t.get("asr_ms", 0),
                "post_ms": t.get("post_ms", 0),
                "inject_ms": t.get("inject_ms", 0),
                "server_total_ms": t.get("total_ms", 0),
                "server_first_partial_ms": t.get("first_partial_ms", 0),
                "audio_ms": t.get("audio_ms", round(audio_ms)),
                "text": text or "",
            }
            if not warm:
                results.append(record)
            tag = "warmup" if warm else "#%-2d" % (i + 1 - args.warmup)
            print(
                "  %-7s 客户端 %7.1f ms | 首字 %7s | partial %2d | ASR %7.1f | 注入 %6.1f | %s"
                % (
                    tag,
                    record["client_ms"],
                    ("%.1f" % first_partial_ms) if first_partial_ms is not None else "n/a",
                    partials,
                    record["asr_ms"],
                    record["inject_ms"],
                    (text or "")[:28],
                )
            )
    finally:
        ws.send_close()
        ws.close()

    if not results:
        print("没有有效样本", file=sys.stderr)
        return 1

    print("")
    print("=== 汇总（%d 次，音频 %.0f ms）===" % (len(results), audio_ms))
    for key, label in [
        ("client_ms", "松键->文字（客户端体感）"),
        ("first_partial_ms", "START->首字（实时上屏）"),
        ("asr_ms", "ASR 推理"),
        ("post_ms", "文本后处理"),
        ("inject_ms", "输入注入"),
        ("server_total_ms", "服务端总计"),
    ]:
        v = [r[key] for r in results if r[key] is not None]
        if not v:
            print("  %-26s 无数据" % label)
            continue
        print(
            "  %-26s 均值 %7.1f  P50 %7.1f  P95 %7.1f  max %7.1f ms"
            % (label, statistics.mean(v), percentile(v, 50), percentile(v, 95), max(v))
        )
    partial_counts = [r["partials"] for r in results]
    print(
        "  %-26s 均值 %7.1f  P50 %7.1f  max %7.1f 次"
        % (
            "每次 PTT 的 partial 数",
            statistics.mean(partial_counts),
            percentile(partial_counts, 50),
            max(partial_counts),
        )
    )
    p95 = percentile([r["client_ms"] for r in results], 95)
    ok = p95 <= args.target_ms
    print("")
    print(
        "目标 P95 <= %.0f ms : %s  (实测 P95 %.1f ms)"
        % (args.target_ms, "达标" if ok else "未达标", p95)
    )

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(
                {
                    "audio_ms": audio_ms,
                    "target_ms": args.target_ms,
                    "p95_client_ms": p95,
                    "passed": ok,
                    "samples": results,
                },
                f,
                ensure_ascii=False,
                indent=2,
            )
        print("原始结果已写入 %s" % args.json_out)

    return 0 if ok or not args.strict else 1


if __name__ == "__main__":
    sys.exit(main())
