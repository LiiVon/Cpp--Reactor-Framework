#!/usr/bin/env python3
# fault_injection.py
# 针对Light Reactor服务端的健壮性/异常场景注入测试，包括超长帧、断连、半包、心跳等
import argparse
import os
import signal
import socket
import struct
import subprocess
import sys
import time


def pack_frame(payload: bytes) -> bytes:
    return struct.pack("!I", len(payload)) + payload


def wait_for_port(host: str, port: int, timeout_s: float = 5.0) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.3):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def recv_frame(sock: socket.socket) -> bytes:
    header = sock.recv(4)
    if len(header) != 4:
        raise RuntimeError("short header")
    (n,) = struct.unpack("!I", header)
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise RuntimeError("connection closed while reading payload")
        data += chunk
    return data


def scenario_oversized_frame(host: str, port: int) -> None:
    with socket.create_connection((host, port), timeout=2) as s:
        s.settimeout(2)
        # 8MB payload length > codec default max (4MB), should be closed by server safely.
        s.sendall(struct.pack("!I", 8 * 1024 * 1024))


def scenario_abrupt_disconnect(host: str, port: int) -> None:
    s = socket.create_connection((host, port), timeout=2)
    s.sendall(pack_frame(b"hello"))
    s.close()


def scenario_half_frame_timeout(host: str, port: int) -> None:
    with socket.create_connection((host, port), timeout=2) as s:
        s.sendall(struct.pack("!I", 32) + b"abc")
        time.sleep(0.2)


def scenario_heartbeat(host: str, port: int) -> None:
    with socket.create_connection((host, port), timeout=2) as s:
        s.sendall(pack_frame(b"PING"))
        out = recv_frame(s)
        if out != b"PONG":
            raise RuntimeError(f"expected PONG, got {out!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Fault injection smoke for Light Reactor")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9100)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--bin", default="./main")
    args = parser.parse_args()

    server = subprocess.Popen(
        [args.bin, str(args.port), str(args.threads)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    try:
        if not wait_for_port(args.host, args.port, 6.0):
            raise RuntimeError("server did not start in time")

        checks = [
            ("heartbeat", scenario_heartbeat),
            ("oversized_frame", scenario_oversized_frame),
            ("abrupt_disconnect", scenario_abrupt_disconnect),
            ("half_frame_timeout", scenario_half_frame_timeout),
        ]

        for name, fn in checks:
            fn(args.host, args.port)
            if server.poll() is not None:
                raise RuntimeError(f"server exited unexpectedly during {name}")
            print(f"[pass] {name}")

        server.send_signal(signal.SIGINT)
        try:
            server.wait(timeout=12)
        except subprocess.TimeoutExpired:
            server.kill()
            raise RuntimeError("server did not exit after SIGINT")

        print("[ok] fault injection passed")
        return 0
    except Exception as ex:
        if server.poll() is None:
            server.kill()
        print(f"[fail] {ex}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
