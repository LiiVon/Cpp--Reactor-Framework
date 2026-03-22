#!/usr/bin/env bash
# build_linux.sh
# 用于在Linux环境下编译Light Reactor主程序，自动清理、编译、链接所有源文件
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "[build] cleaning old objects"
rm -f ./*.o

echo "[build] compiling sources"
for f in ./*.cpp; do
  echo "  - $(basename "$f")"
  g++ -std=c++17 -c "$f" -I.
done

echo "[build] linking main"
g++ -std=c++17 -o main ./*.o -lyaml-cpp -lpthread

echo "[ok] build complete: $ROOT_DIR/main"
