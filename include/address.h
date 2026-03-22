
// Address.h
// IPv4地址包装类，负责IP+Port的存储与sockaddr转换，适配系统调用
#pragma once

#include "global.h"

#ifdef _WIN32

#include <WinSock2.h>
#include <WS2tcpip.h>
#undef ERROR
typedef int socklen_t;
#endif

namespace TcFrame
{
	// IPv4地址包装类，存储IP+Port，提供系统调用需要的sockaddr转换
	class Address
	{
	public:
		Address();
		Address(const std::string& ip, uint16_t port);
		~Address() = default;

		// 获取系统调用需要的sockaddr指针
		struct sockaddr* GetSockAddr();
		// const版本，支持const Address调用
		const struct sockaddr* GetSockAddr() const;

		// 获取sockaddr长度，给bind/accept等系统调用用
		socklen_t GetSockLen() const;

		void SetSockLen(socklen_t len) { m_len = len; }

		// 获取IP、Port、格式化字符串
		std::string GetIp() const;
		uint16_t GetPort() const;
		std::string ToString() const;

	private:
		struct sockaddr_in m_addr;  // IPv4地址结构
		socklen_t m_len;
	};
}