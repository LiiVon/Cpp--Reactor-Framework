#include "address.h"
// Address.cpp
// IPv4地址包装类实现，负责IP+Port的存储与sockaddr转换
#include "logger.h"
#include "socket_utils.h"
#include <cstring>

namespace TcFrame
{
	Address::Address()
		: m_len(sizeof(m_addr))
	{
		memset(&m_addr, 0, sizeof(m_addr));
		m_addr.sin_family = AF_INET;  // 指定IPv4地址族
		m_addr.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0，绑定所有网卡
	}

	Address::Address(const std::string& ip, uint16_t port)
		: m_len(sizeof(m_addr))
	{
		memset(&m_addr, 0, sizeof(m_addr));
		m_addr.sin_family = AF_INET;
		m_addr.sin_port = SocketUtils::HostToNetShort(port); // 端口转网络字节序

		// 空IP默认绑定所有网卡
		if (ip.empty())
		{
			m_addr.sin_addr.s_addr = INADDR_ANY;
		}
		else
		{
			// IP解析失败，默认 fallback 到0.0.0.0，保证服务可用
			if (!SocketUtils::IpV4StrToBin(ip, &m_addr.sin_addr))
			{
				LOG_ERROR("invalid ip address: " + ip + ", fallback to 0.0.0.0");
				m_addr.sin_addr.s_addr = INADDR_ANY;
			}
		}
	}

	struct sockaddr* Address::GetSockAddr()
	{
		return reinterpret_cast<struct sockaddr*>(&m_addr);
	}

	const struct sockaddr* Address::GetSockAddr() const
	{
		return reinterpret_cast<const struct sockaddr*>(&m_addr);
	}

	socklen_t Address::GetSockLen() const
	{
		return m_len;
	}

	std::string Address::GetIp() const
	{
		return SocketUtils::IpV4BinToStr(&m_addr.sin_addr);
	}

	uint16_t Address::GetPort() const
	{
		return SocketUtils::NetToHostShort(m_addr.sin_port);
	}

	std::string Address::ToString() const
	{
		return GetIp() + ":" + std::to_string(GetPort());
	}
}