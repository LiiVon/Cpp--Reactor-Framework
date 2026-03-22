#!/usr/bin/env bash
# run_tests.sh
# 自动编译并运行所有C++单元测试，输出测试结果
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

CXX=${CXX:-g++}
CXXFLAGS=(-std=c++17 -O2 -I.)
LDFLAGS=(-lyaml-cpp -lpthread)

COMMON_SRCS=(
  acceptor.cpp
  address.cpp
  buffer.cpp
  channel.cpp
  epoll_poller.cpp
  eventloop.cpp
  length_field_codec.cpp
  logger.cpp
  log_utils.cpp
  poller.cpp
  socket.cpp
  socket_utils.cpp
  tcpclient.cpp
  tcpconnection.cpp
  tcpserver.cpp
  thread_pool.cpp
  wspoll_poller.cpp
)

mkdir -p tests/bin

declare -a TESTS=(
  test_timer
  test_codec
  test_connection_lifecycle
)

for t in "${TESTS[@]}"; do
  echo "[test-build] $t"
  "$CXX" "${CXXFLAGS[@]}" -o "tests/bin/$t" "tests/$t.cpp" "${COMMON_SRCS[@]}" "${LDFLAGS[@]}"
done

for t in "${TESTS[@]}"; do
  echo "[test-run] $t"
  "./tests/bin/$t"
done

echo "[ok] all tests passed"
