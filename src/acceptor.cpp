#include "acceptor.h"
// Acceptor.cpp
// 连接接收器实现，负责监听端口并accept新连接，供TcpServer使用
#include "logger.h"
#include "socket_utils.h"

namespace TcFrame
{
	Acceptor::Acceptor(EventLoop* loop, const Address& listen_addr)
		: m_loop(loop)
		, m_listen_socket(Socket::CreateNonBlocking())
		, m_is_listening(false)
	{
		// 绑定监听地址，不需要const_cast，直接传const引用
		m_listen_socket.Bind(listen_addr);
		// 直接传SocketType，不需要强转int，64位Windows不会截断
		m_listen_channel = std::make_unique<Channel>(m_loop, m_listen_socket.GetFd());
		// 设置读回调：有新连接到来触发HandleAccept
		m_listen_channel->SetReadCallback([this]() { this->HandleAccept(); });

		LOG_DEBUG("Acceptor created, listening on " + listen_addr.ToString());
	}

	Acceptor::~Acceptor()
	{
		// 停止监听，从EventLoop移除Channel
		m_listen_channel->DisableAll();
		if (m_listen_channel->IsAdded())
		{
			m_loop->RemoveChannel(m_listen_channel.get());
		}

		LOG_DEBUG("Acceptor destroyed, listen fd: " + std::to_string(static_cast<int>(m_listen_socket.GetFd())));
	}

	void Acceptor::Stop()
	{
		m_loop->AssertInLoopThread();
		if (!m_is_listening)
		{
			return;
		}

		m_listen_channel->DisableReading();
		m_is_listening = false;
		LOG_INFO("Acceptor stop listening, " + m_listen_socket.GetLocalAddress().ToString());
	}

	void Acceptor::Listen()
	{
		// 断言必须在loop线程调用，符合我们EventLoop的线程安全设计
		m_loop->AssertInLoopThread();
		// socket开始listen
		m_listen_socket.Listen();
		// 开启读事件监听，等待新连接
		m_listen_channel->EnableReading();
		m_is_listening = true;

		LOG_INFO("Acceptor start listening, " + m_listen_socket.GetLocalAddress().ToString());
	}

	void Acceptor::HandleAccept()
	{
		m_loop->AssertInLoopThread();

		Address client_addr;
		std::unique_ptr<Socket> client_socket = m_listen_socket.Accept(client_addr);

		if (client_socket && client_socket->GetFd() != INVALID_SOCKET_VALUE)
		{
			if (m_new_connection_callback)
			{
				// 移动语义转移Socket所有权，上层拿到后直接管理，没有拷贝，高效
				m_new_connection_callback(std::move(*client_socket), client_addr);
			}
			else
			{
				// 没有设置回调，直接关闭客户端，避免资源泄露
				client_socket->Close();
				LOG_WARN("Acceptor got new connection but no callback, closed directly");
			}

			LOG_DEBUG("Acceptor accept new connection, client: " + client_addr.ToString());
		}
		else
		{
			int err = SocketUtils::GetLastError();
			LOG_ERROR("Acceptor HandleAccept failed, error: " + SocketUtils::GetLastErrorStr(err));
		}
	}

	void Acceptor::SetNewConnectionCallback(const NewConnectionCallback& cb)
	{
		m_new_connection_callback = cb;
	}

	bool Acceptor::IsListening() const
	{
		return m_is_listening;
	}
}