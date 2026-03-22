#!/usr/bin/env python3
# perf_compare.py
# 自动化性能对比脚本，分别以单线程/多线程运行服务端并基准测试，生成对比报告
import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass


@dataclass
class BenchResult:
    name: str
    qps: float
    avg: float
    p50: float
    p95: float
    p99: float


def wait_for_port(host: str, port: int, timeout_s: float = 5.0) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.3):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def run_cmd(cmd: list[str]) -> str:
    out = subprocess.check_output(cmd, text=True)
    return out


def parse_bench_output(name: str, output: str) -> BenchResult:
    def find_float(key: str) -> float:
        m = re.search(rf"{re.escape(key)}=([0-9]+(?:\.[0-9]+)?)", output)
        if not m:
            raise RuntimeError(f"missing {key} in benchmark output")
        return float(m.group(1))

    return BenchResult(
        name=name,
        qps=find_float("qps"),
        avg=find_float("latency_ms_avg"),
        p50=find_float("latency_ms_p50"),
        p95=find_float("latency_ms_p95"),
        p99=find_float("latency_ms_p99"),
    )


def run_profile(binary: str, host: str, port: int, threads: int, clients: int, requests: int, msg_size: int, name: str) -> BenchResult:
    proc = subprocess.Popen([binary, str(port), str(threads)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_for_port(host, port, timeout_s=6.0):
            raise RuntimeError("server start timeout")

        out = run_cmd([
            "python3",
            "./scripts/echo_bench.py",
            "--host",
            host,
            "--port",
            str(port),
            "--clients",
            str(clients),
            "--requests",
            str(requests),
            "--message-size",
            str(msg_size),
            "--check-heartbeat",
        ])
        return parse_bench_output(name, out)
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=6)
            except subprocess.TimeoutExpired:
                proc.kill()


def ratio(before: float, after: float, better_high: bool) -> str:
    if before == 0:
        return "N/A"
    change = (after - before) / before * 100.0
    if better_high:
        return f"{change:+.2f}%"
    return f"{-change:+.2f}%"


def write_report(path: str, before: BenchResult, after: BenchResult) -> None:
    lines = []
    lines.append("# 性能对比报告")
    lines.append("")
    lines.append("说明：本报告自动生成，前后对比定义为单线程基线 vs 多线程优化配置。")
    lines.append("")
    lines.append("| 指标 | 优化前(1线程) | 优化后(4线程) | 变化 |")
    lines.append("|---|---:|---:|---:|")
    lines.append(f"| QPS | {before.qps:.2f} | {after.qps:.2f} | {ratio(before.qps, after.qps, True)} |")
    lines.append(f"| Avg 延迟(ms) | {before.avg:.3f} | {after.avg:.3f} | {ratio(before.avg, after.avg, False)} |")
    lines.append(f"| P50 延迟(ms) | {before.p50:.3f} | {after.p50:.3f} | {ratio(before.p50, after.p50, False)} |")
    lines.append(f"| P95 延迟(ms) | {before.p95:.3f} | {after.p95:.3f} | {ratio(before.p95, after.p95, False)} |")
    lines.append(f"| P99 延迟(ms) | {before.p99:.3f} | {after.p99:.3f} | {ratio(before.p99, after.p99, False)} |")
    lines.append("")
    lines.append("## 测试参数")
    lines.append("")
    lines.append("- 协议：4字节长度头 + Echo")
    lines.append("- 客户端工具：scripts/echo_bench.py")
    lines.append("")

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate performance comparison report")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9200)
    parser.add_argument("--clients", type=int, default=40)
    parser.add_argument("--requests", type=int, default=300)
    parser.add_argument("--message-size", type=int, default=128)
    parser.add_argument("--bin", default="./main")
    parser.add_argument("--report", default="docs/perf_report.md")
    args = parser.parse_args()

    before = run_profile(args.bin, args.host, args.port, 1, args.clients, args.requests, args.message_size, "before")
    after = run_profile(args.bin, args.host, args.port + 1, 4, args.clients, args.requests, args.message_size, "after")

    write_report(args.report, before, after)
    print(f"[ok] report generated: {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
