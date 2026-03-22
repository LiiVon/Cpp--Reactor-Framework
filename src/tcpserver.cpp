#include "tcpserver.h"
// tcpserver.cpp
// TCP服务器实现，主从Reactor架构，支持高并发与优雅停机
#include "logger.h"
#include <cassert>
#include <utility>

namespace TcFrame
{
	TcpServer::TcpServer(EventLoop* main_loop, const Address& listen_addr, size_t thread_num)
		: m_main_loop(main_loop)
		, m_listen_addr(listen_addr)
		, m_thread_num(thread_num)
		, m_next_loop_idx(0)
		, m_started(false)
		, m_stopping(false)
		, m_idle_timeout_seconds(0)
		, m_idle_scan_timer_id(0)
		, m_shutdown_check_timer_id(0)
		, m_shutdown_deadline_timer_id(0)
		, m_total_connections(0)
		, m_closed_connections(0)
		, m_high_water_mark_triggered(0)
		, m_high_water_mark_bytes(1024 * 1024)
		, m_metrics_export_enabled(false)
		, m_metrics_export_interval_seconds(5)
		, m_metrics_export_timer_id(0)
	{
		LOG_INFO("TcpServer create, main loop address: " +
			std::to_string(reinterpret_cast<uintptr_t>(main_loop)) +
			", work thread num: " + std::to_string(thread_num));

		m_acceptor = std::make_unique<Acceptor>(main_loop, listen_addr);
		m_acceptor->SetNewConnectionCallback([this](Socket&& client_socket, const Address& client_addr) {
			this->HandleNewConnection(std::move(client_socket), client_addr);
			});

		if (m_thread_num > 0)
		{
			m_sub_loops.reserve(m_thread_num);
			for (size_t i = 0; i < m_thread_num; ++i)
			{
				auto loop = std::make_unique<EventLoop>(true);
				LOG_INFO("TcpServer create sub loop " + std::to_string(i) +
					", address: " + std::to_string(reinterpret_cast<uintptr_t>(loop.get())));
				m_sub_loops.push_back(std::move(loop));
			}
		}
	}

	TcpServer::~TcpServer()
	{
		LOG_DEBUG("TcpServer destructor begin");
		if (m_idle_scan_timer_id != 0)
		{
			m_main_loop->Cancel(m_idle_scan_timer_id);
			m_idle_scan_timer_id = 0;
		}
		if (m_shutdown_check_timer_id != 0)
		{
			m_main_loop->Cancel(m_shutdown_check_timer_id);
			m_shutdown_check_timer_id = 0;
		}
		if (m_shutdown_deadline_timer_id != 0)
		{
			m_main_loop->Cancel(m_shutdown_deadline_timer_id);
			m_shutdown_deadline_timer_id = 0;
		}
		if (m_metrics_export_timer_id != 0)
		{
			m_main_loop->Cancel(m_metrics_export_timer_id);
			m_metrics_export_timer_id = 0;
		}

		m_stopping.store(true);
		if (m_acceptor && m_acceptor->IsListening())
		{
			m_main_loop->RunInLoop([this]() { m_acceptor->Stop(); });
		}

		for (auto& item : m_connections)
		{
			TcpConnectionPtr conn = item.second;
			conn->ForceClose();
		}

		for (auto& sub_loop : m_sub_loops)
		{
			sub_loop->Quit();
			if (sub_loop->GetThread().joinable())
			{
				sub_loop->GetThread().join();
			}
		}
		m_connections.clear();
		LOG_DEBUG("TcpServer destructor done");
	}

	void TcpServer::Start()
	{
		if (m_started.exchange(true))
		{
			LOG_WARN("TcpServer already started, ignored Start()");
			return;
		}
		m_main_loop->AssertInLoopThread();
		m_start_time = std::chrono::steady_clock::now();
		m_acceptor->Listen();
		if (m_idle_timeout_seconds > 0)
		{
			m_idle_scan_timer_id = m_main_loop->RunEvery(1.0, [this]() {
				this->ScanIdleConnections();
			});
		}
		if (m_metrics_export_enabled && !m_metrics_export_path.empty())
		{
			m_metrics_export_timer_id = m_main_loop->RunEvery(
				static_cast<double>(std::max(m_metrics_export_interval_seconds, 1)),
				[this]() { this->ExportPrometheusMetrics(); });
			ExportPrometheusMetrics();
		}
		LOG_INFO("TcpServer started successfully, listening on " + m_listen_addr.ToString());
	}

	void TcpServer::StopGracefully(int timeout_ms, ShutdownCompleteCallback cb)
	{
		if (!m_main_loop->IsInLoopThread())
		{
			m_main_loop->RunInLoop([this, timeout_ms, cb = std::move(cb)]() mutable {
				this->StopGracefully(timeout_ms, std::move(cb));
			});
			return;
		}

		m_main_loop->AssertInLoopThread();
		if (!m_started.load())
		{
			if (cb)
			{
				cb();
			}
			return;
		}

		if (m_stopping.exchange(true))
		{
			if (cb)
			{
				m_shutdown_complete_callback = std::move(cb);
			}
			return;
		}

		m_shutdown_complete_callback = std::move(cb);
		if (m_acceptor && m_acceptor->IsListening())
		{
			m_acceptor->Stop();
		}

		LOG_WARN("TcpServer entering graceful shutdown, active connections: " + std::to_string(m_connections.size()));
		for (auto& item : m_connections)
		{
			const TcpConnectionPtr& conn = item.second;
			if (conn)
			{
				conn->Shutdown();
			}
		}

		if (m_connections.empty())
		{
			FinalizeGracefulStop(false);
			return;
		}

		if (m_shutdown_check_timer_id == 0)
		{
			m_shutdown_check_timer_id = m_main_loop->RunEvery(0.1, [this]() {
				if (m_connections.empty())
				{
					FinalizeGracefulStop(false);
				}
			});
		}

		const int safe_timeout_ms = std::max(timeout_ms, 1);
		m_shutdown_deadline_timer_id = m_main_loop->RunAfter(static_cast<double>(safe_timeout_ms) / 1000.0, [this]() {
			FinalizeGracefulStop(true);
		});
	}

	EventLoop* TcpServer::NextLoop()
	{
		if (m_thread_num == 0)
		{
			return m_main_loop;
		}

		size_t idx = m_next_loop_idx.fetch_add(1) % m_thread_num;
		EventLoop* selected = m_sub_loops[idx].get();
		LOG_DEBUG("TcpServer NextLoop selected loop index: " + std::to_string(idx) +
			", address: " + std::to_string(reinterpret_cast<uintptr_t>(selected)));
		return selected;
	}

	void TcpServer::HandleNewConnection(Socket&& client_socket, const Address& client_addr)
	{
		m_main_loop->AssertInLoopThread();
		if (m_stopping.load())
		{
			LOG_WARN("TcpServer is stopping, reject new connection from " + client_addr.ToString());
			client_socket.Close();
			return;
		}

		EventLoop* io_loop = NextLoop();
		std::string conn_name = "Conn-" + client_addr.ToString() + "-" + std::to_string(static_cast<int>(client_socket.GetFd()));

		TcpConnectionPtr conn = std::make_shared<TcpConnection>(io_loop, conn_name, std::move(client_socket), client_addr);
		m_total_connections.fetch_add(1);

		conn->SetConnectionCallback(m_connection_callback);
		conn->SetMessageCallback(m_message_callback);
		conn->SetWriteCompleteCallback(m_write_complete_callback);
		conn->SetHighWaterMarkCallback(m_high_water_mark_bytes, [this](const TcpConnectionPtr& c, size_t bytes) {
			m_high_water_mark_triggered.fetch_add(1);
			if (m_high_water_mark_callback)
			{
				m_high_water_mark_callback(c, bytes);
			}
		});
		conn->SetCloseCallback([this](const TcpConnectionPtr& conn) { this->HandleRemoveConnection(conn); });

		m_connections[conn->GetFd()] = conn;

		io_loop->RunInLoop([conn]() {
			conn->ConnectEstablished();
			});

		LOG_INFO("TcpServer new connection [" + conn_name + "] from " + client_addr.ToString() +
			", assigned to loop " + std::to_string(reinterpret_cast<uintptr_t>(io_loop)));
	}

	void TcpServer::HandleRemoveConnection(const TcpConnectionPtr& conn)
	{
		m_main_loop->RunInLoop([this, conn]() {
			m_main_loop->AssertInLoopThread();
			SocketType fd = conn->GetFd();
			LOG_INFO("TcpServer remove connection [" + conn->GetName() + "] fd: " + std::to_string(static_cast<int>(fd)));

			size_t erased = m_connections.erase(fd);
			if (erased == 0)
			{
				LOG_WARN("TcpConnection fd %d already erased from m_connections, ignore duplicate remove", static_cast<int>(fd));
				return;
			}
			m_closed_connections.fetch_add(1);
			(void)erased;

			EventLoop* io_loop = conn->GetLoop();
			io_loop->RunInLoop([conn]() {
				conn->ConnectDestroyed();
				});

			if (m_stopping.load() && m_connections.empty())
			{
				FinalizeGracefulStop(false);
			}

			LOG_DEBUG("TcpServer remove connection done for fd " + std::to_string(static_cast<int>(fd)));
			});
	}

	EventLoop* TcpServer::GetMainLoop() const
	{
		return m_main_loop;
	}

	void TcpServer::SetConnectionCallback(const ConnectionCallback& cb)
	{
		m_connection_callback = cb;
	}
	void TcpServer::SetMessageCallback(const MessageCallback& cb)
	{
		m_message_callback = cb;
	}
	void TcpServer::SetWriteCompleteCallback(const WriteCompleteCallback& cb)
	{
		m_write_complete_callback = cb;
	}

	void TcpServer::SetHighWaterMarkBytes(size_t bytes)
	{
		m_high_water_mark_bytes = std::max<size_t>(bytes, 1);
	}

	void TcpServer::SetHighWaterMarkCallback(const HighWaterMarkCallback& cb)
	{
		m_high_water_mark_callback = cb;
	}

	void TcpServer::SetIdleTimeoutSeconds(int seconds)
	{
		m_idle_timeout_seconds = std::max(seconds, 0);
	}

	std::string TcpServer::GetStats() const
	{
		std::ostringstream oss;
		oss << "total_connections=" << m_total_connections.load()
			<< ", closed_connections=" << m_closed_connections.load()
			<< ", active_connections=" << m_connections.size()
			<< ", idle_timeout_seconds=" << m_idle_timeout_seconds
			<< ", stopping=" << (m_stopping.load() ? 1 : 0)
			<< ", high_water_mark_triggered=" << m_high_water_mark_triggered.load();
		return oss.str();
	}

	void TcpServer::EnablePrometheusMetricsExport(const std::string& file_path, int interval_seconds)
	{
		m_metrics_export_enabled = true;
		m_metrics_export_path = file_path;
		m_metrics_export_interval_seconds = std::max(interval_seconds, 1);
	}

	std::string TcpServer::GetPrometheusMetrics() const
	{
		uint64_t bytes_read_total = 0;
		uint64_t bytes_written_total = 0;
		size_t pending_write_bytes = 0;

		for (const auto& item : m_connections)
		{
			const TcpConnectionPtr& conn = item.second;
			if (!conn)
			{
				continue;
			}
			bytes_read_total += conn->GetBytesReadTotal();
			bytes_written_total += conn->GetBytesWrittenTotal();
			pending_write_bytes += conn->GetPendingWriteBytes();
		}

		double uptime_seconds = 0.0;
		if (m_started.load())
		{
			uptime_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(
				std::chrono::steady_clock::now() - m_start_time).count();
		}

		std::ostringstream oss;
		oss << "# HELP tcframe_connections_total Total accepted connections\n";
		oss << "# TYPE tcframe_connections_total counter\n";
		oss << "tcframe_connections_total " << m_total_connections.load() << "\n";
		oss << "# HELP tcframe_connections_closed_total Total closed connections\n";
		oss << "# TYPE tcframe_connections_closed_total counter\n";
		oss << "tcframe_connections_closed_total " << m_closed_connections.load() << "\n";
		oss << "# HELP tcframe_connections_active Current active connections\n";
		oss << "# TYPE tcframe_connections_active gauge\n";
		oss << "tcframe_connections_active " << m_connections.size() << "\n";
		oss << "# HELP tcframe_server_stopping Whether graceful shutdown is in progress\n";
		oss << "# TYPE tcframe_server_stopping gauge\n";
		oss << "tcframe_server_stopping " << (m_stopping.load() ? 1 : 0) << "\n";
		oss << "# HELP tcframe_output_high_watermark_triggered_total High watermark trigger count\n";
		oss << "# TYPE tcframe_output_high_watermark_triggered_total counter\n";
		oss << "tcframe_output_high_watermark_triggered_total " << m_high_water_mark_triggered.load() << "\n";
		oss << "# HELP tcframe_connection_pending_write_bytes Current pending write bytes on active connections\n";
		oss << "# TYPE tcframe_connection_pending_write_bytes gauge\n";
		oss << "tcframe_connection_pending_write_bytes " << pending_write_bytes << "\n";
		oss << "# HELP tcframe_connection_bytes_read_total Total bytes read by active connections\n";
		oss << "# TYPE tcframe_connection_bytes_read_total counter\n";
		oss << "tcframe_connection_bytes_read_total " << bytes_read_total << "\n";
		oss << "# HELP tcframe_connection_bytes_written_total Total bytes written by active connections\n";
		oss << "# TYPE tcframe_connection_bytes_written_total counter\n";
		oss << "tcframe_connection_bytes_written_total " << bytes_written_total << "\n";
		oss << "# HELP tcframe_uptime_seconds Server uptime seconds\n";
		oss << "# TYPE tcframe_uptime_seconds gauge\n";
		oss << "tcframe_uptime_seconds " << uptime_seconds << "\n";

		return oss.str();
	}

	void TcpServer::ScanIdleConnections()
	{
		m_main_loop->AssertInLoopThread();
		if (m_idle_timeout_seconds <= 0)
		{
			return;
		}

		auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		const int64_t timeout_ms = static_cast<int64_t>(m_idle_timeout_seconds) * 1000;

		std::vector<TcpConnectionPtr> to_close;
		to_close.reserve(m_connections.size());
		for (auto& item : m_connections)
		{
			const TcpConnectionPtr& conn = item.second;
			if (!conn || !conn->IsConnected())
			{
				continue;
			}
			if (now_ms - conn->GetLastActiveMs() >= timeout_ms)
			{
				to_close.push_back(conn);
			}
		}

		for (const auto& conn : to_close)
		{
			LOG_INFO("close idle connection: " + conn->GetName());
			conn->ForceClose();
		}
	}

	size_t TcpServer::GetThreadNum() const
	{
		return m_thread_num;
	}

	void TcpServer::FinalizeGracefulStop(bool force_close)
	{
		m_main_loop->AssertInLoopThread();
		if (!m_stopping.load())
		{
			return;
		}

		std::vector<TcpConnectionPtr> stale_connections;
		for (auto it = m_connections.begin(); it != m_connections.end();)
		{
			const TcpConnectionPtr& conn = it->second;
			if (!conn || !conn->IsConnected())
			{
				if (conn)
				{
					stale_connections.push_back(conn);
				}
				it = m_connections.erase(it);
			}
			else
			{
				++it;
			}
		}

		for (const auto& conn : stale_connections)
		{
			EventLoop* io_loop = conn->GetLoop();
			io_loop->RunInLoop([conn]() {
				conn->ConnectDestroyed();
			});
		}

		if (force_close)
		{
			LOG_WARN("TcpServer graceful shutdown timeout reached, force closing remaining connections: " + std::to_string(m_connections.size()));
			for (auto& item : m_connections)
			{
				const TcpConnectionPtr& conn = item.second;
				if (conn)
				{
					conn->ForceClose();
				}
			}
			if (!m_connections.empty())
			{
				return;
			}
		}

		if (m_shutdown_check_timer_id != 0)
		{
			m_main_loop->Cancel(m_shutdown_check_timer_id);
			m_shutdown_check_timer_id = 0;
		}
		if (m_shutdown_deadline_timer_id != 0)
		{
			m_main_loop->Cancel(m_shutdown_deadline_timer_id);
			m_shutdown_deadline_timer_id = 0;
		}
		if (m_idle_scan_timer_id != 0)
		{
			m_main_loop->Cancel(m_idle_scan_timer_id);
			m_idle_scan_timer_id = 0;
		}
		if (m_metrics_export_timer_id != 0)
		{
			m_main_loop->Cancel(m_metrics_export_timer_id);
			m_metrics_export_timer_id = 0;
		}

		m_started.store(false);
		m_stopping.store(false);
		LOG_INFO("TcpServer graceful shutdown finished");

		if (m_shutdown_complete_callback)
		{
			auto cb = std::move(m_shutdown_complete_callback);
			m_shutdown_complete_callback = {};
			cb();
		}
	}

	void TcpServer::ExportPrometheusMetrics()
	{
		m_main_loop->AssertInLoopThread();
		if (!m_metrics_export_enabled || m_metrics_export_path.empty())
		{
			return;
		}

		const std::string payload = GetPrometheusMetrics();
		const std::string tmp_path = m_metrics_export_path + ".tmp";

		std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc);
		if (!ofs.is_open())
		{
			LOG_ERROR("failed to open metrics temp file: " + tmp_path);
			return;
		}
		ofs << payload;
		ofs.close();

		if (std::rename(tmp_path.c_str(), m_metrics_export_path.c_str()) != 0)
		{
			LOG_ERROR("failed to rename metrics file from " + tmp_path + " to " + m_metrics_export_path);
		}
	}

	bool TcpServer::IsStopping() const
	{
		return m_stopping.load();
	}
}
