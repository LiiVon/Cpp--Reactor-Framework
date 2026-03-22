
// tcpserver.h
// TCP服务器，主从Reactor架构，支持高并发与优雅停机
#pragma once

#include "global.h"
#include "socket.h"
#include "buffer.h"
#include "eventloop.h"
#include "channel.h"
#include "tcpconnection.h"
#include "acceptor.h"

namespace TcFrame
{
	/*
	@brief: TCP服务器，主从Reactor架构
	- main Reactor(主线程)负责accept新连接
	- 轮询分配给sub Reactor(多个线程)，每个sub Reactor负责连接读写
	- 多核水平扩展，高并发下性能好
	*/
	// 主从Reactor：main Reactor负责accept，sub Reactor负责连接读写
	class TcpServer
	{
	public:
		// 回调函数类型定义
		using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
		using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
		using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
		using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, size_t)>;
		using ShutdownCompleteCallback = std::function<void()>;

		// 构造：main_loop是主Reactor的EventLoop，listen_addr是监听地址，thread_num是sub Reactor线程数，0表示单线程模式
		TcpServer(EventLoop* main_loop, const Address& listen_addr, size_t thread_num = 4);
		~TcpServer();

		TcpServer(const TcpServer&) = delete;
		TcpServer& operator=(const TcpServer&) = delete;

		// 对外接口
		void Start(); // 启动服务器，开始监听和接受连接，必须在main_loop线程调用
		void StopGracefully(int timeout_ms = 5000, ShutdownCompleteCallback cb = {}); // 优雅停机
		bool IsStopping() const;

		// 设置回调函数，由用户调用，一次设置，所有连接复用
		void SetConnectionCallback(const ConnectionCallback& cb);
		void SetMessageCallback(const MessageCallback& cb);
		void SetWriteCompleteCallback(const WriteCompleteCallback& cb);
		void SetHighWaterMarkBytes(size_t bytes);
		void SetHighWaterMarkCallback(const HighWaterMarkCallback& cb);
		void SetIdleTimeoutSeconds(int seconds);
		void EnablePrometheusMetricsExport(const std::string& file_path, int interval_seconds = 5);
		std::string GetStats() const;
		std::string GetPrometheusMetrics() const;

		EventLoop* GetMainLoop() const;
		size_t GetThreadNum() const;

	private:
		// 内部回调：处理新连接、移除连接
		void HandleNewConnection(Socket&& client_socket, const Address& client_addr);
		void HandleRemoveConnection(const TcpConnectionPtr& conn);
		void ScanIdleConnections();
		void FinalizeGracefulStop(bool force_close);
		void ExportPrometheusMetrics();

		EventLoop* NextLoop(); // 轮询选择一个sub Reactor的EventLoop，负载均衡

	private:
		EventLoop* m_main_loop;               // 主Reactor，只负责accept
		Address m_listen_addr;               // 监听地址
		size_t m_thread_num;                  // sub Reactor线程数，0表示单线程
		std::unique_ptr<Acceptor> m_acceptor; // 接收器，负责accept新连接

		std::vector<std::unique_ptr<EventLoop>> m_sub_loops; // sub Reactor列表，每个一个线程

		// 活跃连接列表：key是socket fd(SocketType，跨平台不截断)，value是连接智能指针
		std::unordered_map<SocketType, TcpConnectionPtr> m_connections;

		// 用户回调，所有连接复用
		ConnectionCallback m_connection_callback;
		MessageCallback m_message_callback;
		WriteCompleteCallback m_write_complete_callback;
		HighWaterMarkCallback m_high_water_mark_callback;

		// 轮询索引，原子操作，线程安全
		std::atomic<size_t> m_next_loop_idx;
		std::atomic<bool> m_started; // 服务器是否已经启动，原子保证只启动一次
		std::atomic<bool> m_stopping; // 服务器是否进入优雅停机流程
		std::vector<std::shared_ptr<TcpConnection>> m_all_connections; // 保存所有活跃连接，用来广播下线
		int m_idle_timeout_seconds; // 空闲连接超时秒数，<=0 表示关闭
		EventLoop::TimerId m_idle_scan_timer_id;
		EventLoop::TimerId m_shutdown_check_timer_id;
		EventLoop::TimerId m_shutdown_deadline_timer_id;
		std::atomic<uint64_t> m_total_connections;
		std::atomic<uint64_t> m_closed_connections;
		std::atomic<uint64_t> m_high_water_mark_triggered;
		size_t m_high_water_mark_bytes;
		ShutdownCompleteCallback m_shutdown_complete_callback;

		bool m_metrics_export_enabled;
		std::string m_metrics_export_path;
		int m_metrics_export_interval_seconds;
		EventLoop::TimerId m_metrics_export_timer_id;
		std::chrono::steady_clock::time_point m_start_time;
	};
}
