# Light Reactor

一个面向高并发 TCP 服务的 C++17 Reactor 网络框架，当前支持 Windows / Linux 双平台。

## 项目亮点

- 基于 Reactor 架构，模块职责清晰。
- 主从 Reactor 模型：
  - 主循环负责 `accept` 新连接。
  - 子循环负责 IO 事件与回调处理。
- 提供 Windows/Linux 跨平台 Socket 抽象层。
- 线程安全的异步日志系统，支持多输出器。
- 基于 `yaml-cpp` 的配置模块。
- 线程池支持业务任务异步派发。
- 完整定时器能力：`RunAfter` / `RunAt` / `RunEvery` / `Cancel`。
- 连接空闲超时回收（Idle Timeout）与周期运行状态统计。
- 内置 4 字节长度头协议编解码器，处理 TCP 半包/粘包。
- 支持简单心跳：`PING` -> `PONG`。
- 支持优雅停机：停止接入、等待在途发送完成、超时强制关闭。
- 支持背压高水位回调（High Water Mark）用于慢连接保护。
- 支持 Prometheus 文本格式指标导出（默认 `./tcframe_metrics.prom`）。

## 当前架构

- `EventLoop`：One Loop Per Thread，包含任务队列与定时任务队列。
- `Poller`：平台相关的 IO 多路复用工厂。
  - Linux：`EpollPoller`
  - Windows：`WSPollPoller`
- `Channel`：fd 事件注册与回调分发。
- `Socket` / `SocketUtils`：Socket 生命周期与平台封装。
- `Acceptor`：监听并接收新连接。
- `TcpConnection`：单连接状态机与收发缓冲区管理。
- `TcpServer`：连接接入、分发与连接管理。
- `Logger`：异步日志（控制台/普通文件/错误文件）。
- `Config`：YAML 加载、重载与变更回调。
- `ThreadPool`：通用后台任务执行。
- `scripts/echo_bench.py`：Echo 压测脚本（吞吐与延迟统计）。

## 项目创建顺序（实习面试叙事）

建议按以下 7 个阶段讲述项目，从“能跑”到“可交付”：

1. 网络与事件基础：先完成 `Socket/Address`、`Channel`、`Poller`、`EventLoop` 的最小闭环。
2. 连接与服务抽象：实现 `Acceptor`、`TcpConnection`、`TcpServer`，跑通 Echo 基线。
3. 跨平台适配：统一 Windows/Linux 分支与接口行为（非阻塞连接、poll/epoll 分支）。
4. 核心能力增强：补齐定时器系统、空闲连接回收、长度帧编解码与心跳。
5. 稳定性治理：加入优雅停机、背压高水位回调、异常连接清理与故障路径修复。
6. 可观测与压测：输出运行统计与 Prometheus 文本指标，补充压测脚本与报告。
7. 工程化交付：补单测/集测、故障注入脚本、Linux+Windows CI，形成持续回归链路。

实习答辩建议：优先讲你亲自解决过的问题（如跨线程析构、停机卡住、半包粘包），再讲性能和工程化结果，面试官更容易认可你的真实贡献。

## 已完成的稳定性优化

- 强化 `EventLoop` 退出/析构流程，降低跨线程误用风险。
- 加固 `Epoll` 活跃 fd 查询逻辑，避免未知 fd 导致隐式插入问题。
- 优化 `ThreadPool` 关闭与任务提交并发同步。
- 对齐 Windows/Linux 的非阻塞 `connect` 语义。
- 增加可取消的定时器系统与动态 Poll 超时计算。
- 增加 `TcpServer` 空闲连接回收与运行时统计输出。
- 平台范围明确收敛为 Windows + Linux。

## 构建方式

### Linux

```bash
./scripts/build_linux.sh
```

脚本会自动编译全部 `*.cpp` 并完成链接，依赖：

- `-lyaml-cpp`
- `-lpthread`

## 构建产物

- 可执行文件：`./main`

## 运行示例（EchoServer）

默认启动参数：

- 端口：`9000`
- 子线程数：`4`

启动方式：

```bash
./main
```

自定义端口和线程数：

```bash
./main 9001 2
```

默认示例内置：

- 空闲连接超时：120 秒
- 运行状态日志：每 10 秒输出一次（总连接/关闭连接/活跃连接）
- 发送缓冲高水位：256KB（触发背压日志）
- 指标导出：每 5 秒写入 `./tcframe_metrics.prom`

可用 `nc` 进行本地验证（Linux）：

```bash
nc 127.0.0.1 9000
```

`nc` 可用于快速验证端口连通性；若要验证业务回显，请按下方协议格式发送长度帧。按 `Ctrl+C` 可优雅退出服务。

优雅退出流程：

- 收到 `Ctrl+C` 后，服务先停止接收新连接。
- 等待已有连接把待发送数据刷完并关闭。
- 超过 5 秒仍未结束则强制关闭剩余连接。

协议格式说明：

- 帧头：4 字节网络字节序整数 `N`（payload 长度）
- 帧体：`N` 字节业务数据
- 服务端对完整帧做 echo 回写；收到 `PING` 会回 `PONG`

## 压测脚本

启动服务后，可使用 Python 压测脚本：

```bash
python3 ./scripts/echo_bench.py --host 127.0.0.1 --port 9000 --clients 50 --requests 200 --message-size 128 --check-heartbeat
```

脚本会输出：

- 总请求数与成功请求数
- 总耗时与 QPS
- 延迟统计（Avg / P50 / P95 / P99）

## 测试体系

已补充单元 + 集成测试：

- `tests/test_timer.cpp`：定时器 `RunAfter/RunEvery/Cancel` 行为校验。
- `tests/test_codec.cpp`：长度帧拆包/粘包解码行为校验。
- `tests/test_connection_lifecycle.cpp`：连接建立、收发、关闭生命周期集成校验。

运行方式：

```bash
./scripts/run_tests.sh
```

## 故障注入

新增故障注入脚本，覆盖典型异常流量：

- 超大长度帧（非法长度）
- 客户端异常断开
- 半包后中断
- 心跳链路检查

运行方式：

```bash
python3 ./scripts/fault_injection.py --port 9100 --threads 1
```

## CI（Linux + Windows）

已提供 GitHub Actions 工作流：

- 配置文件：`.github/workflows/ci.yml`
- Linux：构建 + 测试 + 故障注入 + 性能报告产物上传
- Windows：MSYS2 环境构建 + 测试

## 性能对比报告

新增一页自动化性能报告（QPS / Avg / P50 / P95 / P99 对比）：

- 生成脚本：`scripts/perf_compare.py`
- 默认输出：`docs/perf_report.md`

运行方式：

```bash
python3 ./scripts/perf_compare.py --report docs/perf_report.md
```

## 最小协议客户端

可用以下脚本快速验证长度帧协议与心跳：

```bash
python3 ./scripts/lf_client.py --host 127.0.0.1 --port 9000 --message hello
python3 ./scripts/lf_client.py --host 127.0.0.1 --port 9000 --ping
```

## 后续可扩展方向（简历加分项）

- 增加更细粒度的压力场景（慢读客户端、突发洪峰、长连接空闲混合流量）。
- 增加测试覆盖率统计与门禁（例如覆盖率阈值）。
- 增加 Windows 端运行态压测数据并补全双平台性能对比。
- 增加可视化指标上报（例如接入 Prometheus + Grafana）。

## 说明

- 当前分支不包含 macOS 适配。
- 推荐编译器：`g++`，标准：`-std=c++17`。
