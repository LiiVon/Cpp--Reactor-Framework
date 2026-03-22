#!/usr/bin/env python3
# echo_bench.py
# TCP Echo基准测试脚本，支持多客户端并发、延迟统计、心跳校验，评估Reactor性能
import argparse
import socket
import threading
import time
from statistics import mean


def pack_frame(payload: bytes) -> bytes:
    return len(payload).to_bytes(4, byteorder="big", signed=False) + payload


def recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = []
    remain = size
    while remain > 0:
        chunk = sock.recv(remain)
        if not chunk:
            raise RuntimeError("server closed connection")
        chunks.append(chunk)
        remain -= len(chunk)
    return b"".join(chunks)


def recv_frame(sock: socket.socket) -> bytes:
    header = recv_exact(sock, 4)
    length = int.from_bytes(header, byteorder="big", signed=False)
    if length < 0:
        raise RuntimeError("invalid negative frame length")
    return recv_exact(sock, length)


def worker(host, port, payload, requests, latencies, lock):
    local = []
    try:
        with socket.create_connection((host, port), timeout=5) as s:
            s.settimeout(5)
            for _ in range(requests):
                start = time.perf_counter()
                s.sendall(pack_frame(payload))
                echoed = recv_frame(s)
                end = time.perf_counter()
                if echoed != payload:
                    raise RuntimeError("echo payload mismatch")
                local.append((end - start) * 1000.0)
    except Exception as e:
        print(f"[worker] error: {e}")

    with lock:
        latencies.extend(local)


def percentile(values, p):
    if not values:
        return 0.0
    idx = int((len(values) - 1) * p)
    return values[idx]


def main():
    parser = argparse.ArgumentParser(description="Echo benchmark for Light Reactor")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--clients", type=int, default=20)
    parser.add_argument("--requests", type=int, default=200)
    parser.add_argument("--message-size", type=int, default=128)
    parser.add_argument("--check-heartbeat", action="store_true")
    args = parser.parse_args()

    payload = ("x" * args.message_size).encode("utf-8")
    total_requests = args.clients * args.requests

    all_latencies = []
    lock = threading.Lock()
    threads = []

    begin = time.perf_counter()
    for _ in range(args.clients):
        t = threading.Thread(
            target=worker,
            args=(args.host, args.port, payload, args.requests, all_latencies, lock),
            daemon=True,
        )
        t.start()
        threads.append(t)

    if args.check_heartbeat:
        with socket.create_connection((args.host, args.port), timeout=5) as hb:
            hb.settimeout(5)
            hb.sendall(pack_frame(b"PING"))
            if recv_frame(hb) != b"PONG":
                raise RuntimeError("heartbeat check failed: expected PONG")

    for t in threads:
        t.join()
    end = time.perf_counter()

    duration = end - begin
    all_latencies.sort()

    ok_requests = len(all_latencies)
    qps = ok_requests / duration if duration > 0 else 0.0

    print("=== Echo Benchmark Result ===")
    print(f"target={args.host}:{args.port}")
    print(f"clients={args.clients}, requests_per_client={args.requests}, total={total_requests}")
    print(f"ok_requests={ok_requests}, duration={duration:.3f}s, qps={qps:.2f}")
    if all_latencies:
        print(f"latency_ms_avg={mean(all_latencies):.3f}")
        print(f"latency_ms_p50={percentile(all_latencies, 0.50):.3f}")
        print(f"latency_ms_p95={percentile(all_latencies, 0.95):.3f}")
        print(f"latency_ms_p99={percentile(all_latencies, 0.99):.3f}")


if __name__ == "__main__":
    main()
