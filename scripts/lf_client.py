#!/usr/bin/env python3
# lf_client.py
# 简单的长度头协议客户端，支持发送普通消息和心跳PING，校验服务端响应
import argparse
import socket


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
    return recv_exact(sock, length)


def main() -> int:
    parser = argparse.ArgumentParser(description="Length-field client for Light Reactor")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--message", default="hello")
    parser.add_argument("--ping", action="store_true", help="send PING and expect PONG")
    args = parser.parse_args()

    payload = b"PING" if args.ping else args.message.encode("utf-8")

    with socket.create_connection((args.host, args.port), timeout=5) as s:
        s.settimeout(5)
        s.sendall(pack_frame(payload))
        response = recv_frame(s)

    print(f"sent={payload!r}")
    print(f"recv={response!r}")

    if args.ping and response != b"PONG":
        raise RuntimeError("heartbeat check failed: expected PONG")
    if not args.ping and response != payload:
        raise RuntimeError("echo check failed: response mismatch")

    print("ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
