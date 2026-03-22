#include "tcpclient.h"
// tcpclient.cpp
// TCP客户端实现，封装outgoing连接，复用TcpConnection，支持自动重连
#include "logger.h"
#include "socket_utils.h"

namespace TcFrame
{
	TcpClient::TcpClient(EventLoop* loop, const Address& server_addr, std::string name)
		: m_loop(loop)
		, m_server_addr(server_addr)
		, m_name(std::move(name))
		, m_connecting(false)
		, m_started(false)
	{
		LOG_DEBUG("TcpClient create: " + m_name + ", server: " + m_server_addr.ToString()
			+ ", loop address: " + std::to_string(reinterpret_cast<uintptr_t>(loop)));
	}

	TcpClient::~TcpClient()
	{
		LOG_DEBUG("TcpClient destructor: " + m_name);
		if (m_connection)
		{
			m_connection->ForceClose();
		}
	}

	void TcpClient::Connect()
	{
		m_loop->AssertInLoopThread();

		if (m_started.exchange(true))
		{
			LOG_WARN("TcpClient " + m_name + " already started, ignored Connect()");
			return;
		}

		m_connecting = true;

		m_socket = std::make_unique<Socket>(SocketUtils::CreateNonBlockingSocket());
		m_socket->SetNonBlocking(true);

		int ret = m_socket->Connect(m_server_addr);
		if (ret == 0)
		{
			// 连接立即成功，直接处理
			HandleConnect(std::move(*m_socket));
			m_socket.reset();
		}
		else if (ret == SocketUtils::kConnectInProgress)
		{
			// 连接正在进行，等待连接完成，把socket加入EventLoop监听写事件
			LOG_DEBUG("TcpClient " + m_name + " connect in progress, wait for completion");

			// 用shared_ptr管理temp_channel，满足std::function可拷贝要求，同时延长生命周期到回调执行完
			std::shared_ptr<Channel> temp_channel = std::make_shared<Channel>(m_loop, m_socket->GetFd());
			Channel* temp_ptr = temp_channel.get();
			temp_channel->SetWriteCallback(
				[this, temp_ptr, temp_channel]()
				{
					// 写事件触发，连接完成，检查错误
					SocketType fd = temp_ptr->GetFd();
					int err = SocketUtils::GetSocketError(fd);
					if (err != 0)
					{
						LOG_ERROR("TcpClient " + m_name + " connect failed, error: " + SocketUtils::GetLastErrorStr(err));
						m_connecting = false;
						if (m_auto_reconnect && m_started)
						{
							LOG_INFO("TcpClient " + m_name + " will retry connect after " + std::to_string(m_reconnect_delay_ms) + "ms");
							m_loop->RunAfter(double(m_reconnect_delay_ms) / 1000.0, [this]() { DoReconnect(); });
						}
					}
					else
					{
						// 连接成功，处理
						HandleConnect(std::move(*m_socket));
						m_socket.reset();
					}
					// 临时Channel用完就移除
					if (temp_ptr->IsAdded())
					{
						m_loop->RemoveChannel(temp_ptr);
					}
					// 回调执行完，lambda销毁，temp_channel引用计数减为0，自动释放Channel
				});

			temp_channel->EnableWriting();
			m_loop->UpdateChannel(temp_channel.get());
		}
		else
		{
			// 连接直接失败，打日志
			LOG_ERROR("TcpClient " + m_name + " connect failed immediately, error: " + SocketUtils::GetLastErrorStr(SocketUtils::GetLastError()));
			m_connecting = false;
			m_started = false;
			m_socket.reset();
		}
	}

	void TcpClient::HandleConnect(Socket&& client_socket)
	{
		m_loop->AssertInLoopThread();
		m_connecting = false;
		LOG_INFO("TcpClient " + m_name + " connected to " + m_server_addr.ToString() + " fd: " + std::to_string(static_cast<int>(client_socket.GetFd())));

		TcpConnectionPtr conn = std::make_shared<TcpConnection>(m_loop, m_name, std::move(client_socket), m_server_addr);

		conn->SetConnectionCallback(m_connection_callback);
		conn->SetMessageCallback(m_message_callback);
		conn->SetWriteCompleteCallback(m_write_complete_callback);
		conn->SetCloseCallback([this](const TcpConnectionPtr& conn) { HandleRemoveConnection(conn); });

		m_connection = conn;
		conn->ConnectEstablished();
	}

	void TcpClient::HandleRemoveConnection(const TcpConnectionPtr& conn)
	{
		m_loop->AssertInLoopThread();
		LOG_INFO("TcpClient " + m_name + " connection disconnected from " + m_server_addr.ToString());

		m_connection.reset();

		if (m_close_callback)
		{
			m_close_callback(conn);
		}

		if (m_auto_reconnect && m_started)
		{
			LOG_INFO("TcpClient " + m_name + " auto reconnect after " + std::to_string(m_reconnect_delay_ms) + "ms");
			m_connecting = false;
			m_loop->RunAfter(double(m_reconnect_delay_ms) / 1000.0, [this]() { DoReconnect(); });
		}
		else
		{
			m_started = false;
		}
	}

	void TcpClient::DoReconnect()
	{
		if (m_started && !m_connecting && !IsConnected())
		{
			LOG_INFO("TcpClient " + m_name + " do reconnect to " + m_server_addr.ToString());
			Connect();
		}
	}

	void TcpClient::Disconnect()
	{
		m_auto_reconnect = false;
		m_started = false;
		if (m_connection)
		{
			m_connection->Shutdown();
		}
		LOG_INFO("TcpClient " + m_name + " disconnected from " + m_server_addr.ToString());
	}

	void TcpClient::Send(const std::string& message)
	{
		Send(message.data(), message.size());
	}

	void TcpClient::Send(const void* data, size_t len)
	{
		if (IsConnected())
		{
			m_connection->Send(data, len);
		}
		else
		{
			LOG_ERROR("TcpClient " + m_name + " Send failed, not connected");
		}
	}

	void TcpClient::SetConnectionCallback(const ConnectionCallback& cb)
	{
		m_connection_callback = cb;
	}
	void TcpClient::SetMessageCallback(const MessageCallback& cb)
	{
		m_message_callback = cb;
	}
	void TcpClient::SetWriteCompleteCallback(const WriteCompleteCallback& cb)
	{
		m_write_complete_callback = cb;
	}
	void TcpClient::SetCloseCallback(const CloseCallback& cb)
	{
		m_close_callback = cb;
	}

	TcpConnectionPtr TcpClient::GetConnection() const
	{
		return m_connection;
	}

	bool TcpClient::IsConnected() const
	{
		return m_connection && m_connection->IsConnected();
	}

	EventLoop* TcpClient::GetLoop() const
	{
		return m_loop;
	}

	const std::string& TcpClient::GetName() const
	{
		return m_name;
	}

	void TcpClient::SetAutoReconnect(bool enable)
	{
		m_auto_reconnect = enable;
	}
}
