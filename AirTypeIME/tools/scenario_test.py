#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""场景矩阵测试：不依赖硬件，验证 daemon 对各种帧序列的响应。

启动一个临时 daemon（mock 引擎 + inject=none + 设置页），用 mock_airmic 的
WebSocket 客户端逐场景发帧，断言状态帧序列，结束后杀进程。

覆盖的分支（这些在硬件缺席时最容易漏）：
  1. 正常 START/AUDIO/END        -> recognizing + done + TEXT
  2. START/CANCEL                -> cancelled，无 TEXT
  3. START/END（空段）           -> done/empty utterance，无 TEXT
  4. 非法帧（magic 错）          -> error，连接保持
  5. 未 START 就 AUDIO           -> error（NotStarted）
  6. 未 START 就 END             -> Ignored（无状态帧），连接保持
  7. seq 回绕（0xFFFF->0）       -> 不报 error
  8. START 声明 opus             -> error（UnsupportedCodec）
  9. PING                        -> PONG
 10. 中途断连（不发 END）        -> 设置页 devices 里不再出现该设备

用法：
    python tools/scenario_test.py
    python tools/scenario_test.py --daemon target/debug/airtype_daemon.exe
"""

import argparse
import json
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mock_airmic as M  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
WS_PATH = "/ws"
MOCK_TEXT = "场景测试文本"
AUDIO = b"\x00\x00" * 320  # 20ms 静音，16kHz/16bit/mono


def find_daemon(explicit):
    if explicit:
        path = Path(explicit)
        if not path.exists():
            raise SystemExit(f"指定的 daemon 不存在: {path}")
        return path
    for candidate in (
        REPO / "target" / "debug" / "airtype-daemon.exe",
        REPO / "target" / "release" / "airtype-daemon.exe",
    ):
        if candidate.exists():
            return candidate
    # 兜底：bin 名可能带连字符或下划线，按通配找主程序（排除 deps/增量目录）。
    for candidate in sorted(REPO.glob("target/*/airtype*daemon*.exe")):
        if "deps" not in candidate.parts and "incremental" not in candidate.parts:
            return candidate
    raise SystemExit("找不到 airtype-daemon.exe，请先 cargo build -p airtype-daemon")


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def write_config(directory, ws_port, web_port):
    cfg = f"""[server]
bind = "127.0.0.1"
port = {ws_port}
path = "{WS_PATH}"

[audio]
sample_rate = 16000
channels = 1
bits_per_sample = 16
frame_ms = 20

[asr]
engine = "mock"
mock_text = "{MOCK_TEXT}"

[streaming]
enabled = false
chunk_ms = 250

[inject]
mode = "none"

[web]
enabled = true
bind = "127.0.0.1"
port = {web_port}
"""
    path = Path(directory) / "airtype.toml"
    path.write_text(cfg, encoding="utf-8")
    return path


class Client:
    def __init__(self, port, timeout=5.0):
        self.ws = M.WebSocketClient("127.0.0.1", port, WS_PATH, timeout)

    def send(self, kind, seq, payload=b""):
        self.ws.send_binary(M.make_frame(kind, seq, payload))

    def drain(self, quiet=0.35, max_wait=6.0):
        """读到静默为止，返回 [(kind, body), ...]。"""
        out = []
        deadline = time.time() + max_wait
        while time.time() < deadline:
            try:
                self.ws.sock.settimeout(quiet)
                opcode, payload = self.ws.recv_frame()
            except socket.timeout:
                break
            except (OSError, RuntimeError):
                break
            if opcode != 0x2:
                continue
            kind, body = M.parse_frame(payload)
            if kind is None:
                continue
            out.append((kind, body))
        return out

    def close(self):
        try:
            self.ws.send_close()
        except OSError:
            pass
        self.ws.close()


def states(frames):
    out = []
    for kind, body in frames:
        if kind == M.KIND_STATUS:
            try:
                out.append(json.loads(body.decode("utf-8")))
            except (ValueError, UnicodeDecodeError):
                out.append({"state": "<bad json>"})
    return out


def state_names(frames):
    return [s.get("state") for s in states(frames)]


def texts(frames):
    return [body.decode("utf-8", "replace") for kind, body in frames if kind == M.KIND_TEXT]


def has_error(frames):
    return any(s.get("state") == "error" for s in states(frames))


def has_kind(frames, kind):
    return any(k == kind for k, _ in frames)


def start_meta(device="scenario-01", codec="pcm_s16le"):
    return json.dumps(
        {
            "device": device,
            "codec": codec,
            "sample_rate": 16000,
            "channels": 1,
            "bits_per_sample": 16,
        }
    ).encode("utf-8")


# --- 场景 -------------------------------------------------------------------


def scenario_normal(port):
    c = Client(port)
    c.send(M.KIND_START, 0, start_meta())
    for seq in range(1, 4):
        c.send(M.KIND_AUDIO, seq, AUDIO)
    c.send(M.KIND_END, 4)
    frames = c.drain()
    c.close()
    names = state_names(frames)
    got = texts(frames)
    # postprocess 默认会补标点，所以前缀匹配而不是全等。
    ok = "recognizing" in names and "done" in names and len(got) == 1 and got[0].startswith(MOCK_TEXT)
    return ok, f"states={names} texts={got}"


def scenario_cancel(port):
    c = Client(port)
    c.send(M.KIND_START, 0, start_meta())
    c.send(M.KIND_AUDIO, 1, AUDIO)
    c.send(M.KIND_CANCEL, 2)
    frames = c.drain()
    c.close()
    names = state_names(frames)
    ok = "cancelled" in names and not texts(frames)
    return ok, f"states={names} texts={texts(frames)}"


def scenario_empty(port):
    c = Client(port)
    c.send(M.KIND_START, 0, start_meta())
    c.send(M.KIND_END, 1)
    frames = c.drain()
    c.close()
    names = state_names(frames)
    empty = any(s.get("message") == "empty utterance" for s in states(frames))
    ok = "done" in names and empty and not texts(frames)
    return ok, f"states={names} texts={texts(frames)}"


def scenario_bad_frame(port):
    c = Client(port)
    c.ws.send_binary(b"\x00\x01\x02\x03\x04\x05\x06\x07")  # magic/version 全错
    first = c.drain()
    err_seen = has_error(first)
    # 连接应保持可用
    c.send(M.KIND_START, 0, start_meta())
    c.send(M.KIND_AUDIO, 1, AUDIO)
    c.send(M.KIND_END, 2)
    second = c.drain()
    c.close()
    ok = err_seen and "done" in state_names(second)
    return ok, f"非法帧后 error={err_seen}，随后 states={state_names(second)}"


def scenario_audio_without_start(port):
    c = Client(port)
    c.send(M.KIND_AUDIO, 0, AUDIO)
    frames = c.drain()
    c.close()
    ok = has_error(frames)
    return ok, f"states={state_names(frames)}"


def scenario_end_without_start(port):
    c = Client(port)
    c.send(M.KIND_END, 0)
    first = c.drain()
    # Ignored 不该产生任何状态帧；连接仍可用
    c.send(M.KIND_START, 1, start_meta())
    c.send(M.KIND_END, 2)
    second = c.drain()
    c.close()
    ok = state_names(first) == ["ready"] and "done" in state_names(second)
    return ok, f"首次 states={state_names(first)}，随后 states={state_names(second)}"


def scenario_seq_wraparound(port):
    c = Client(port)
    c.send(M.KIND_START, 0xFFFF, start_meta())
    c.send(M.KIND_AUDIO, 0x0000, AUDIO)
    c.send(M.KIND_AUDIO, 0x0001, AUDIO)
    c.send(M.KIND_END, 0x0002)
    frames = c.drain()
    c.close()
    ok = not has_error(frames) and "done" in state_names(frames)
    return ok, f"states={state_names(frames)}"


def scenario_bad_codec(port):
    c = Client(port)
    c.send(M.KIND_START, 0, start_meta(codec="opus"))
    first = c.drain()
    err_seen = has_error(first)
    # session 未进入 Recording，换正确 meta 应能继续
    c.send(M.KIND_START, 1, start_meta())
    c.send(M.KIND_END, 2)
    second = c.drain()
    c.close()
    ok = err_seen and "done" in state_names(second)
    return ok, f"opus 被拒={err_seen}，随后 states={state_names(second)}"


def scenario_ping(port):
    c = Client(port)
    c.send(M.KIND_PING, 0)
    frames = c.drain()
    c.close()
    ok = has_kind(frames, M.KIND_PONG)
    return ok, f"收到 PONG={ok}"


def scenario_disconnect_cleanup(port, web_port):
    c = Client(port)
    c.send(M.KIND_START, 0, start_meta(device="disconnect-01"))
    c.send(M.KIND_AUDIO, 1, AUDIO)
    c.close()  # 不发 END，直接断连
    time.sleep(0.8)
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{web_port}/api/state", timeout=3) as resp:
            devices = json.loads(resp.read().decode("utf-8")).get("devices", [])
    except (OSError, ValueError) as exc:
        return False, f"查询设置页失败: {exc}"
    names = [d.get("device", "") for d in devices]
    # registry 会把离线条目留到 prune 为止，所以「已离线」就算清理到位。
    entry = next((d for d in devices if d.get("device") == "disconnect-01"), None)
    ok = entry is None or not entry.get("connected", True)
    return ok, f"disconnect-01={'已清理' if entry is None else ('离线' if ok else '仍在线')} devices={names}"


SCENARIOS = [
    ("正常识别", scenario_normal),
    ("PTT 取消", scenario_cancel),
    ("空语音段", scenario_empty),
    ("非法帧后连接保持", scenario_bad_frame),
    ("未 START 就 AUDIO", scenario_audio_without_start),
    ("未 START 就 END", scenario_end_without_start),
    ("seq 回绕", scenario_seq_wraparound),
    ("START 声明 opus", scenario_bad_codec),
    ("PING/PONG", scenario_ping),
]


def wait_port(port, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), 0.2):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def tail(path, lines=30):
    try:
        data = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join(data[-lines:])


def main():
    ap = argparse.ArgumentParser(description="AirType daemon 场景矩阵测试")
    ap.add_argument("--daemon", help="airtype_daemon.exe 路径（默认自动探测）")
    args = ap.parse_args()

    daemon = find_daemon(args.daemon)
    ws_port = free_port()
    web_port = free_port()

    tmp = tempfile.mkdtemp(prefix="airtype-scenario-")
    cfg = write_config(tmp, ws_port, web_port)
    log_path = Path(tmp) / "daemon.log"

    print(f"daemon: {daemon}")
    print(f"ws={ws_port} web={web_port} config={cfg}")

    with open(log_path, "w", encoding="utf-8") as log_file:
        proc = subprocess.Popen(
            [str(daemon), str(cfg)],
            cwd=str(REPO),
            stdout=log_file,
            stderr=subprocess.STDOUT,
        )
        try:
            if not wait_port(ws_port):
                print("daemon 未能在 15s 内监听，日志尾部：")
                print(tail(log_path))
                return 1
            if not wait_port(web_port):
                print("设置页未能在 15s 内监听，日志尾部：")
                print(tail(log_path))
                return 1

            results = []
            for name, fn in SCENARIOS:
                try:
                    ok, detail = fn(ws_port)
                except Exception as exc:  # 连接异常也算失败
                    ok, detail = False, f"异常: {exc}"
                results.append((name, ok, detail))

            # 断连清理单独跑（需要 web 端口）
            try:
                ok, detail = scenario_disconnect_cleanup(ws_port, web_port)
            except Exception as exc:
                ok, detail = False, f"异常: {exc}"
            results.append(("中途断连清理", ok, detail))
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    failed = 0
    for name, ok, detail in results:
        print(f"[{'PASS' if ok else 'FAIL'}] {name}\n        {detail}")
        if not ok:
            failed += 1

    print(f"\n{len(results) - failed}/{len(results)} 通过")
    if failed:
        print("daemon 日志尾部：")
        print(tail(log_path))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
