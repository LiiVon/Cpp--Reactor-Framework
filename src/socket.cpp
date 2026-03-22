#include "socket.h"
// socket.cpp
// TCP socket封装实现，RAII管理生命周期，跨平台基础操作
#include "socket_utils.h"
#include "logger.h"
#include "address.h"
#include <utility>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#undef ERROR

namespace TcFrame
{
	// Windows下自动初始化/清理Winsock，程序启动自动执行，退出自动清理
	// 静态全局变量，程序启动自动初始化，只初始化一次
	struct WinsockInitializer
	{
		WinsockInitializer()
		{
			WSADATA wsa_data;
			int ret = WSAStartup(MAKEWORD(2, 2), &wsa_data);
			if (ret != 0)
			{
				LOG_FATAL("WSAStartup failed, code: " + std::to_string(ret));
				std::exit(-1);
			}
			LOG_INFO("Winsock initialize success");
		}
		~WinsockInitializer()
		{
			WSACleanup();
			LOG_INFO("Winsock cleanup complete");
		}
	};
	static WinsockInitializer g_winsock_initializer;
}
#endif

namespace TcFrame
{
	Socket::Socket(SocketType sockfd)
		: m_sockfd(sockfd)
	{
		LOG_DEBUG("create socket from existing handle, fd: " + std::to_string((int)m_sockfd));
	}

	Socket::Socket()
	{
		m_sockfd = SocketUtils::CreateTcpSocket();
		if (m_sockfd == INVALID_SOCKET_VALUE)
		{
			int err_code = SocketUtils::GetLastError();
			LOG_FATAL("create new socket failed: " + SocketUtils::GetLastErrorStr(err_code));
			return;
		}
		LOG_DEBUG("create new socket, fd: " + std::to_string((int)m_sockfd));

		// 默认开启地址复用+端口复用，方便服务端重启绑定
		SetReuseAddr(true);
		SetReusePort(true);
	}

	Socket::Socket(Socket&& other) noexcept
		: m_sockfd(other.m_sockfd)
	{
		// 转移所有权，原对象置为无效，避免析构重复关闭
		other.m_sockfd = INVALID_SOCKET_VALUE;
		LOG_DEBUG("move construct socket, new fd: " + std::to_string((int)m_sockfd));
	}

	Socket& Socket::operator=(Socket&& other) noexcept
	{
		if (this != &other)
		{
			// 先关闭自己当前的socket
			if (IsValid())
			{
				Close();
			}
			// 交换所有权
			std::swap(m_sockfd, other.m_sockfd);
		}
		return *this;
	}

	Socket::~Socket()
	{
		Close();
	}

	bool Socket::Bind(const Address& addr)
	{
		if (!IsValid())
		{
			LOG_ERROR("bind failed: socket fd is invalid");
			return false;
		}

		int ret = bind(m_sockfd, addr.GetSockAddr(), addr.GetSockLen());
		if (
#ifdef _WIN32

			ret == SOCKET_ERROR
#else

			ret < 0
#endif

			)
		{
			int err_code = SocketUtils::GetLastError();
			LOG_ERROR("bind address " + addr.ToString() + " failed: " + SocketUtils::GetLastErrorStr(err_code));
			return false;
		}

		LOG_INFO("bind address success: " + addr.ToString());
		return true;
	}

	bool Socket::Listen(int backlog)
	{
		if (!IsValid())
		{
			LOG_ERROR("listen failed: socket fd is invalid");
			return false;
		}

		int ret = listen(m_sockfd, backlog);
		if (
#ifdef _WIN32

			ret == SOCKET_ERROR
#else

			ret < 0
#endif

			)
		{
			int err_code = SocketUtils::GetLastError();
			LOG_ERROR("listen failed: " + SocketUtils::GetLastErrorStr(err_code));
			return false;
		}

		LOG_INFO("socket listen success, backlog: " + std::to_string(backlog));
		return true;
	}

	std::unique_ptr<Socket> Socket::Accept(Address& peer_addr)
	{
		if (!IsValid())
		{
			LOG_ERROR("accept failed: listen socket fd is invalid");
			return nullptr;
		}

		socklen_t addr_len = peer_addr.GetSockLen();
		SocketType client_fd = accept(m_sockfd, peer_addr.GetSockAddr(), &addr_len);
		if (client_fd == INVALID_SOCKET_VALUE)
		{
			int err_code = SocketUtils::GetLastError();
			LOG_ERROR("accept failed: " + SocketUtils::GetLastErrorStr(err_code));
			return nullptr;
		}
		peer_addr.SetSockLen(addr_len);

		auto new_socket = std::make_unique<Socket>(client_fd);
		new_socket->SetNonBlocking(true); // 新连接默认非阻塞，适配Reactor事件驱动
		return new_socket;
	}

	int Socket::Connect(const Address& server_addr)
	{
		if (!IsValid())
		{
			LOG_ERROR("connect failed: socket fd is invalid");
			return -1;
		}

		int ret = connect(m_sockfd, server_addr.GetSockAddr(), server_addr.GetSockLen());
		if (
#ifdef _WIN32

			ret == SOCKET_ERROR
#else

			ret < 0
#endif

			)
		{
			int err_code = SocketUtils::GetLastError();

#ifdef _WIN32
			if (err_code == WSAEWOULDBLOCK || err_code == WSAEINPROGRESS)
			{
				LOG_DEBUG("connect in progress (non-blocking): " + server_addr.ToString());
				return SocketUtils::kConnectInProgress;
			}
#else
			if (err_code == EINPROGRESS || SocketUtils::IsNormalWouldBlock(err_code))
			{
				LOG_DEBUG("connect in progress (non-blocking): " + server_addr.ToString());
				return SocketUtils::kConnectInProgress;
			}
#endif

			LOG_ERROR("connect to " + server_addr.ToString() + " failed: " + SocketUtils::GetLastErrorStr(err_code));
			return -err_code;
		}

		LOG_INFO("connect to server success: " + server_addr.ToString());
		return 0;
	}

	void Socket::SetReuseAddr(bool enable)
	{
		if (!SocketUtils::SetReuseAddr(m_sockfd, enable))
		{
			LOG_ERROR("SetReuseAddr failed, fd: " + std::to_string((int)m_sockfd));
		}
		else
		{
			LOG_DEBUG("SetReuseAddr " + std::string(enable ? "enabled" : "disabled") + ", fd: " + std::to_string((int)m_sockfd));
		}
	}

	void Socket::SetReusePort(bool enable)
	{
		if (!SocketUtils::SetReusePort(m_sockfd, enable))
		{
			if (enable)
			{
				// 开启SO_REUSEPORT失败不影响核心功能，只打警告
				LOG_WARN("SetReusePort failed, ignored (not a critical error)");
			}
		}
		else
		{
			LOG_DEBUG("SetReusePort enabled, fd: " + std::to_string((int)m_sockfd));
		}
	}

	void Socket::SetNonBlocking(bool enable)
	{
		if (!SocketUtils::SetNonBlocking(m_sockfd, enable))
		{
			LOG_ERROR("SetNonBlocking failed, fd: " + std::to_string((int)m_sockfd));
		}
		else
		{
			LOG_DEBUG("SetNonBlocking " + std::string(enable ? "enabled" : "disabled") + ", fd: " + std::to_string((int)m_sockfd));
		}
	}

	SocketType Socket::GetFd() const
	{
		return m_sockfd;
	}

	void Socket::Close()
	{
		if (IsValid())
		{
			SocketUtils::CloseSocket(m_sockfd);
			LOG_DEBUG("socket closed, fd: " + std::to_string((int)m_sockfd));
			m_sockfd = INVALID_SOCKET_VALUE;
		}
	}

	bool Socket::IsValid() const
	{
		return m_sockfd != INVALID_SOCKET_VALUE;
	}

	Socket Socket::CreateNonBlocking()
	{
		SocketType fd = SocketUtils::CreateTcpSocket();
		if (fd == INVALID_SOCKET_VALUE)
		{
			LOG_FATAL("CreateNonBlocking failed: create socket error: " + SocketUtils::GetLastErrorStr(SocketUtils::GetLastError()));
			return Socket(INVALID_SOCKET_VALUE);
		}
		SocketUtils::SetNonBlocking(fd);
		SocketUtils::SetReuseAddr(fd, true);
		return Socket(fd);
	}

	Address Socket::GetLocalAddress() const
	{
		Address addr{};
		socklen_t len = addr.GetSockLen();
		int ret = getsockname(m_sockfd, addr.GetSockAddr(), &len);
		if (ret != 0)
		{
			LOG_ERROR("Socket::GetLocalAddress failed, fd: " + std::to_string(static_cast<int>(m_sockfd)) + ", error: " + SocketUtils::GetLastErrorStr(SocketUtils::GetLastError()));
		}
		addr.SetSockLen(len);
		return addr;
	}

	Address Socket::GetPeerAddress() const
	{
		Address addr{};
		socklen_t len = addr.GetSockLen();
		int ret = getpeername(m_sockfd, addr.GetSockAddr(), &len);
		if (ret != 0)
		{
			LOG_ERROR("Socket::GetPeerAddress failed, fd: " + std::to_string(static_cast<int>(m_sockfd)) + ", error: " + SocketUtils::GetLastErrorStr(SocketUtils::GetLastError()));
		}
		addr.SetSockLen(len);
		return addr;
	}
}