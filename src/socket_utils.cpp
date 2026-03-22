#include "socket_utils.h"
// socket_utils.cpp
// 跨平台socket工具函数实现，封装常用socket操作与选项
#include "logger.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace TcFrame
{
#ifdef _WIN32

	bool SocketUtils::InitWinsock()
	{
		static bool inited = false;
		if (inited) return true;

		WSADATA wsa_data;
		WORD version = MAKEWORD(2, 2);
		int ret = WSAStartup(version, &wsa_data);
		if (ret != 0)
		{
			LOG_ERROR("WSAStartup failed: " + GetLastErrorStr(ret));
			return false;
		}
		inited = true;
		LOG_INFO("Winsock initialized success");
		return true;
	}
#else

	bool SocketUtils::InitWinsock()
	{
		// Linux不需要初始化，直接返回成功
		return true;
	}
#endif

	uint16_t SocketUtils::HostToNetShort(uint16_t host16)
	{
		return htons(host16);
	}

	uint16_t SocketUtils::NetToHostShort(uint16_t net16)
	{
		return ntohs(net16);
	}

	uint32_t SocketUtils::HostToNetLong(uint32_t host32)
	{
		return htonl(host32);
	}

	uint32_t SocketUtils::NetToHostLong(uint32_t net32)
	{
		return ntohl(net32);
	}

	bool SocketUtils::IpV4StrToBin(const std::string& ip_str, void* out_bin)
	{
		int ret = inet_pton(AF_INET, ip_str.c_str(), out_bin);
		return ret > 0;
	}

	std::string SocketUtils::IpV4BinToStr(const void* ip_bin)
	{
		char str_buf[INET_ADDRSTRLEN];
		const char* ret = inet_ntop(AF_INET, ip_bin, str_buf, sizeof(str_buf));
		if (ret == nullptr)
		{
			LOG_DEBUG("inet_ntop failed: " + GetLastErrorStr(GetLastError()));
			return "";
		}
		return std::string(str_buf);
	}

	bool SocketUtils::SetReuseAddr(SocketType fd, bool enable)
	{
		int opt = enable ? 1 : 0;
#ifdef _WIN32

		int ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
		if (ret == SOCKET_ERROR)
#else

		int ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void*)&opt, sizeof(opt));
		if (ret < 0)
#endif

		{
			LOG_ERROR("setsockopt SO_REUSEADDR failed: " + GetLastErrorStr(GetLastError()));
			return false;
		}
		return true;
	}

	bool SocketUtils::SetReusePort(SocketType fd, bool enable)
	{
#ifndef SO_REUSEPORT

		// 平台不支持SO_REUSEPORT，直接降级返回成功
		(void)fd; (void)enable;
		LOG_WARN("SO_REUSEPORT is not supported on this platform, skip");
		return true;
#else

		int opt = enable ? 1 : 0;
#ifdef _WIN32

		int ret = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&opt, sizeof(opt));
		if (ret == SOCKET_ERROR)
#else

		int ret = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const void*)&opt, sizeof(opt));
		if (ret < 0)
#endif

		{
			LOG_ERROR("setsockopt SO_REUSEPORT failed: " + GetLastErrorStr(GetLastError()));
			return false;
		}
		return true;
#endif
	}

	bool SocketUtils::SetNonBlocking(SocketType fd, bool enable)
	{
#ifdef _WIN32

		u_long mode = enable ? 1 : 0;
		int ret = ioctlsocket(fd, FIONBIO, &mode);
		if (ret == SOCKET_ERROR)
		{
			LOG_ERROR("ioctlsocket FIONBIO failed: " + GetLastErrorStr(GetLastError()));
			return false;
		}
#else

		int flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0)
		{
			LOG_ERROR("fcntl F_GETFL failed: " + GetLastErrorStr(GetLastError()));
			return false;
		}
		if (enable)
			flags |= O_NONBLOCK;
		else
			flags &= ~O_NONBLOCK;
		if (fcntl(fd, F_SETFL, flags) < 0)
		{
			LOG_ERROR("fcntl F_SETFL failed: " + GetLastErrorStr(GetLastError()));
			return false;
		}
#endif

		return true;
	}

	SocketType SocketUtils::CreateTcpSocket()
	{
		SocketType fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd == INVALID_SOCKET_VALUE)
		{
			LOG_ERROR("create TCP socket failed: " + GetLastErrorStr(GetLastError()));
			return INVALID_SOCKET_VALUE;
		}
		return fd;
	}

	int SocketUtils::GetLastError()
	{
#ifdef _WIN32

		return WSAGetLastError();
#else

		return errno;
#endif
	}

	std::string SocketUtils::GetLastErrorStr(int err)
	{
#ifdef _WIN32

		char buf[256];
		FormatMessageA(
			FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			buf, sizeof(buf), nullptr
		);
		return std::string(buf);
#else

		return std::string(strerror(err));
#endif
	}

	void SocketUtils::CloseSocket(SocketType fd)
	{
#ifdef _WIN32

		closesocket(fd);
#else

		close(fd);
#endif
	}

	bool SocketUtils::IsPortAvailable(uint16_t port)
	{
		SocketType sockfd = CreateTcpSocket();
		if (sockfd == INVALID_SOCKET_VALUE)
		{
			return false;
		}

		sockaddr_in addr{};
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = HostToNetShort(port);
		addr.sin_addr.s_addr = INADDR_ANY;

		int ret = bind(sockfd, (sockaddr*)&addr, sizeof(addr));
		CloseSocket(sockfd);

		if (ret < 0
#ifdef _WIN32

			|| ret == SOCKET_ERROR
#endif

			)
		{
			int err = GetLastError();
			if (err ==
#ifdef _WIN32

				WSAEADDRINUSE
#else

				EADDRINUSE
#endif

				)
			{
				return false; // 端口被占用
			}
			LOG_ERROR("bind failed when check port available: " + GetLastErrorStr(err));
			return false;
		}
		return true; // 绑定成功，端口可用
	}

	bool SocketUtils::IsNormalWouldBlock(int err)
	{
#ifdef _WIN32

		return err == WSAEWOULDBLOCK;
#else

		return err == EAGAIN || err == EWOULDBLOCK;
#endif
	}

	SocketType SocketUtils::CreateNonBlockingSocket()
	{
		SocketType fd = CreateTcpSocket();
		if (fd == INVALID_SOCKET_VALUE)
		{
			return INVALID_SOCKET_VALUE;
		}
		if (!SetNonBlocking(fd, true))
		{
			CloseSocket(fd);
			return INVALID_SOCKET_VALUE;
		}
		return fd;
	}

	int SocketUtils::GetSocketError(SocketType fd)
	{
		int error = 0;
		socklen_t len = sizeof(error);
#ifdef _WIN32

		getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&error, &len);
#else

		getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
#endif

		return error;
	}
}