#include "tcpconnection.h"
// tcpconnection.cpp
// TCP连接封装实现，管理连接生命周期、事件回调、缓冲区，线程安全
#include "logger.h"
#include "socket_utils.h"
#include "global.h"

#ifdef _WIN32

#include <winsock2.h>

#include <ws2tcpip.h>

#undef ERROR

#else
#include <unistd.h>
#include <errno.h>

#endif

namespace TcFrame
{
	TcpConnection::TcpConnection(EventLoop* loop, std::string name, Socket socket, Address peer_addr)
		:m_loop(loop)
		, m_name(std::move(name))
		, m_socket(std::move(socket))
		, m_peer_addr(std::move(peer_addr))
		, m_state(ConnectionState::Connecting)
		, m_last_active_ms(0)
		, m_pending_write_bytes(0)
		, m_total_bytes_read(0)
		, m_total_bytes_written(0)
		, m_high_water_mark(1024 * 1024)
	{
		LOG_DEBUG("TcpConnection constructor: " + m_name + " fd: " + std::to_string(static_cast<int>(m_socket.GetFd())));
		// 直接传SocketType，不转int，避免64位Windows截断
		m_channel = std::make_unique<Channel>(loop, m_socket.GetFd());

		// 设置Channel的事件回调函数
		m_channel->SetReadCallback([this]() { this->HandleRead(); });
		m_channel->SetWriteCallback([this]() { this->HandleWrite(); });
		m_channel->SetCloseCallback([this]() { this->HandleClose(); });
		m_channel->SetErrorCallback([this]() { this->HandleError(); });
		RefreshActiveTime();
	}

	TcpConnection::~TcpConnection()
	{
		LOG_DEBUG("TcpConnection destructor: " + m_name + " fd: " + std::to_string(static_cast<int>(m_socket.GetFd())));
		// Socket的析构会自己close fd，Channel的析构会自己移除，RAII保证不泄漏
		assert(m_state == ConnectionState::Disconnected);
	}

	bool TcpConnection::IsNormalWouldBlock(int err) {
#ifdef _WIN32

		return err == WSAEWOULDBLOCK;
#else

		return err == EAGAIN || err == EWOULDBLOCK;
#endif
	}

	void TcpConnection::ConnectEstablished()
	{
		m_loop->AssertInLoopThread();
		assert(m_state == ConnectionState::Connecting);
		m_state = ConnectionState::Connected;
		RefreshActiveTime();
		m_channel->EnableReading(); // 连接建立后，默认关注读事件，等待对方发送数据
		if (m_connection_callback)
		{
			m_connection_callback(shared_from_this());
		}
		LOG_DEBUG("TcpConnection established: " + m_name + " fd: " + std::to_string(static_cast<int>(m_socket.GetFd())));
	}

	void TcpConnection::ConnectDestroyed()
	{
		m_loop->AssertInLoopThread();
		LOG_DEBUG("TcpConnection ConnectDestroyed enter: " + m_name + " state: " + std::to_string(static_cast<int>(m_state)));
		if (m_state == ConnectionState::Connected || m_state == ConnectionState::Disconnecting)
		{
			m_state = ConnectionState::Disconnected;
			m_pending_write_bytes.store(0);
			m_channel->DisableAll(); // 连接销毁，停止关注所有事件
			if (m_channel->IsAdded()) {
				m_loop->RemoveChannel(m_channel.get());
			}
			m_socket.Close();
		}

		LOG_DEBUG("TcpConnection destroyed done: " + m_name + " fd: " + std::to_string(static_cast<int>(m_socket.GetFd())));
	}

	void TcpConnection::HandleRead()
	{
		m_loop->AssertInLoopThread();
		int err = 0;
		// input_buffer.ReadFromFd已经处理了非阻塞读，返回读到的字节数
		ssize_t n = m_input_buffer.ReadFromFd(m_socket.GetFd(), &err);
		if (n > 0)
		{
			m_total_bytes_read.fetch_add(static_cast<uint64_t>(n));
			RefreshActiveTime();
			// 读到数据了，回调用户的MessageCallback，把Buffer给用户拆包处理
			if (m_message_callback)
			{
				m_message_callback(shared_from_this(), &m_input_buffer);
			}
		}
		else if (n == 0)
		{
			// 对端关闭连接（FIN读到了），触发关闭
			LOG_INFO("TcpConnection read EOF, close connection: " + m_name);
			HandleClose();
		}
		else
		{
			// 读出错，如果不是正常阻塞，就关闭连接
			if (!IsNormalWouldBlock(err))
			{
				LOG_ERROR("TcpConnection read error: " + std::to_string(err) + " fd: " + std::to_string(static_cast<int>(m_socket.GetFd())) + " msg: " + SocketUtils::GetLastErrorStr(err));
				HandleClose();
			}
		}
	}

	void TcpConnection::HandleWrite()
	{
		m_loop->AssertInLoopThread();
		if (!IsConnected())
		{
			LOG_ERROR("HandleWrite called but connection is not connected, fd: " + std::to_string(static_cast<int>(m_socket.GetFd())));
			return;
		}
		int err = 0;
		// output_buffer.WriteToFd处理非阻塞写，返回写到内核的字节数
		ssize_t n = m_output_buffer.WriteToFd(m_socket.GetFd(), &err);

		if (n > 0)
		{
			m_total_bytes_written.fetch_add(static_cast<uint64_t>(n));
			m_pending_write_bytes.store(m_output_buffer.ReadableBytes());
			RefreshActiveTime();
			// 写了n字节，如果所有可读数据都写完了
			if (m_output_buffer.ReadableBytes() == 0)
			{
				// 写完了，不再关注写事件，节省资源
				m_channel->DisableWriting();
				if (m_state == ConnectionState::Disconnecting)
				{
					HandleClose();
					return;
				}

				// 回调写完成，上层可以做统计或者后续处理
				if (m_write_complete_callback)
				{
					m_write_complete_callback(shared_from_this());
				}
			}
			// 如果还有没写完的，说明内核缓冲区满了，下次会继续触发写事件，不用处理
			// 如果出错，只有不是正常阻塞才关闭
			if (err != 0 && !IsNormalWouldBlock(err))
			{
				LOG_ERROR("TcpConnection HandleWrite partial error: " + std::to_string(err) + " fd: " + std::to_string(static_cast<int>(m_socket.GetFd())));
				HandleClose();
			}
		}
		else
		{
			if (!IsNormalWouldBlock(err))
			{
				LOG_ERROR("TcpConnection HandleWrite error: " + std::to_string(err) + " fd: " + std::to_string(static_cast<int>(m_socket.GetFd())));
				HandleClose();
			}
			// 正常阻塞：内核缓冲区满了，什么也不用做，还有数据在output_buffer，下次写事件会继续发
		}
	}

	void TcpConnection::HandleClose()
	{
		m_loop->AssertInLoopThread();
		LOG_INFO("TcpConnection HandleClose: " + m_name + " fd: " + std::to_string(static_cast<int>(m_socket.GetFd())));

		// 避免重复关闭
		if (m_state == ConnectionState::Disconnected)
		{
			LOG_DEBUG("TcpConnection already disconnected, ignore HandleClose");
			return;
		}

		// 更新状态
		m_state = ConnectionState::Disconnected;
		m_pending_write_bytes.store(0);
		// 禁用所有事件，从EventLoop移除
		m_channel->DisableAll();
		if (m_channel->IsAdded())
		{
			m_loop->RemoveChannel(m_channel.get());
		}

		// 调用关闭回调，通知TcpServer移除这个连接
		if (m_close_callback)
		{
			m_close_callback(shared_from_this());
		}

		m_socket.Close();
		LOG_DEBUG("TcpConnection HandleClose done: " + m_name);
	}

	void TcpConnection::HandleError()
	{
		// 先打详细错误日志，方便排查
		int err_code = SocketUtils::GetLastError();
		LOG_ERROR("TcpConnection HandleError: " + m_name + " fd: " + std::to_string(static_cast<int>(m_socket.GetFd())) + " error: " + SocketUtils::GetLastErrorStr(err_code));

		// 只要触发错误事件，连接已经不可用，直接关闭
		HandleClose();
	}

	void TcpConnection::Shutdown()
	{
		if (m_state == ConnectionState::Connected)
		{
			m_state = ConnectionState::Disconnecting;
			// 跨线程安全，扔给loop线程处理
			m_loop->RunInLoop([this]() { this->ShutdownInLoop(); });
			LOG_DEBUG("TcpConnection Shutdown requested: " + m_name);
		}
		else
		{
			LOG_WARN("TcpConnection Shutdown called but not connected, state: " + std::to_string(static_cast<int>(m_state)));
		}
	}

	void TcpConnection::ShutdownInLoop()
	{
		m_loop->AssertInLoopThread();
		if (m_output_buffer.ReadableBytes() > 0)
		{
			// 还有数据没发完，确保写事件被关注，等数据发完再关闭
			if (!m_channel->IsWriting())
			{
				m_channel->EnableWriting();
			}
			LOG_DEBUG("TcpConnection ShutdownInLoop: wait for output buffer send, remaining bytes: " + std::to_string(m_output_buffer.ReadableBytes()));
		}
		else
		{
			// 没有待发送数据，直接进入统一关闭路径，确保触发close回调与连接摘除
			HandleClose();
			LOG_DEBUG("TcpConnection ShutdownInLoop done via HandleClose: " + m_name);
		}
	}

	void TcpConnection::ForceClose()
	{
		if (m_state == ConnectionState::Connected || m_state == ConnectionState::Disconnecting)
		{
			m_state = ConnectionState::Disconnecting;
			m_loop->RunInLoop([this]() { this->ForceCloseInLoop(); });
			LOG_DEBUG("TcpConnection ForceClose requested: " + m_name);
		}
	}

	void TcpConnection::ForceCloseInLoop()
	{
		m_loop->AssertInLoopThread();
		if (m_state == ConnectionState::Connected || m_state == ConnectionState::Disconnecting)
		{
			m_state = ConnectionState::Disconnected;
			m_pending_write_bytes.store(0);
			m_channel->DisableAll();
			m_socket.Close();
			if (m_close_callback)
			{
				m_close_callback(shared_from_this());
			}
			LOG_DEBUG("TcpConnection ForceClose done: " + m_name);
		}
	}

	void TcpConnection::Send(const std::string& message)
	{
		if (!IsConnected())
		{
			LOG_ERROR("TcpConnection Send called but connection is not connected, fd: " + std::to_string(static_cast<int>(m_socket.GetFd())));
			return;
		}

		if (m_loop->IsInLoopThread())
		{
			SendInLoop(message.data(), message.size());
			return;
		}

		std::string data = message;
		m_loop->QueueInLoop([this, data = std::move(data)]() {
			this->SendInLoop(data.data(), data.size());
		});
	}

	void TcpConnection::Send(const void* data, size_t len)
	{
		if (!IsConnected())
		{
			LOG_ERROR("TcpConnection Send called but connection is not connected, fd: " + std::to_string(static_cast<int>(m_socket.GetFd())));
			return;
		}

		if (m_loop->IsInLoopThread())
		{
			SendInLoop(data, len);
			return;
		}

		std::string copied(static_cast<const char*>(data), len);
		m_loop->QueueInLoop([this, copied = std::move(copied)]() {
			this->SendInLoop(copied.data(), copied.size());
		});
	}

	void TcpConnection::SendInLoop(const std::string& message)
	{
		SendInLoop(message.data(), message.size());
	}

	void TcpConnection::SendInLoop(const void* data, size_t len)
	{
		m_loop->AssertInLoopThread();
		if (!IsConnected())
		{
			LOG_ERROR("TcpConnection SendInLoop called but connection is not connected, fd: " + std::to_string(static_cast<int>(m_socket.GetFd())));
			return;
		}

		// 把数据追加到输出缓冲区，等内核可写再发
		const size_t old_pending = m_output_buffer.ReadableBytes();
		m_output_buffer.Append(data, len);
		const size_t new_pending = m_output_buffer.ReadableBytes();
		m_pending_write_bytes.store(new_pending);

		if (m_high_water_mark_callback && old_pending < m_high_water_mark && new_pending >= m_high_water_mark)
		{
			m_high_water_mark_callback(shared_from_this(), new_pending);
		}

		if (!m_channel->IsWriting())
		{
			// 之前没在写，现在开启写事件监听
			m_channel->EnableWriting();
			LOG_DEBUG("TcpConnection enable writing, append " + std::to_string(len) + " bytes, fd: " + std::to_string(static_cast<int>(m_socket.GetFd())));
		}
	}

	bool TcpConnection::IsConnected() const
	{
		return m_state == ConnectionState::Connected;
	}

	EventLoop* TcpConnection::GetLoop() const
	{
		return m_loop;
	}

	SocketType TcpConnection::GetFd() const
	{
		return m_socket.GetFd();
	}

	const std::string& TcpConnection::GetName() const
	{
		return m_name;
	}

	Address TcpConnection::GetPeerAddr() const
	{
		return m_peer_addr;
	}

	int64_t TcpConnection::GetLastActiveMs() const
	{
		return m_last_active_ms.load();
	}

	size_t TcpConnection::GetPendingWriteBytes() const
	{
		return m_pending_write_bytes.load();
	}

	uint64_t TcpConnection::GetBytesReadTotal() const
	{
		return m_total_bytes_read.load();
	}

	uint64_t TcpConnection::GetBytesWrittenTotal() const
	{
		return m_total_bytes_written.load();
	}

	void TcpConnection::SetConnectionCallback(const ConnectionCallback& cb)
	{
		m_connection_callback = cb;
	}

	void TcpConnection::SetMessageCallback(const MessageCallback& cb)
	{
		m_message_callback = cb;
	}

	void TcpConnection::SetWriteCompleteCallback(const WriteCompleteCallback& cb)
	{
		m_write_complete_callback = cb;
	}

	void TcpConnection::SetCloseCallback(const CloseCallback& cb)
	{
		m_close_callback = cb;
	}

	void TcpConnection::SetHighWaterMarkCallback(size_t high_water_mark, const HighWaterMarkCallback& cb)
	{
		m_high_water_mark = high_water_mark;
		m_high_water_mark_callback = cb;
	}

	void TcpConnection::SetContext(std::any context)
	{
		m_context = std::move(context);
	}

	std::any& TcpConnection::GetContext()
	{
		return m_context;
	}

	void TcpConnection::RefreshActiveTime()
	{
		auto now = std::chrono::system_clock::now();
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
		m_last_active_ms.store(ms);
	}
}
