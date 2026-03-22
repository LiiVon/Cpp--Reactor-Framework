
// socket.h
// TCP socket封装，RAII管理生命周期，跨平台基础操作
#pragma once

#include "global.h"
#include "address.h"

namespace TcFrame
{
	// TCP socket封装类，RAII管理生命周期，跨平台封装所有基础socket操作
	class Socket
	{
	public:
		// 工厂方法：创建非阻塞TCP socket
		static Socket CreateNonBlocking();

		// 绑定已有socket句柄（比如accept返回的新连接）
		explicit Socket(SocketType sockfd);
		Socket();
		~Socket();

		Socket(const Socket&) = delete;
		Socket& operator=(const Socket&) = delete;
		Socket(Socket&& other) noexcept;
		Socket& operator=(Socket&& other) noexcept;

		// 核心业务接口
		bool Bind(const Address& addr);    // 绑定地址（服务端监听用）
		bool Listen(int backlog = 1024);  // 开始监听
		std::unique_ptr<Socket> Accept(Address& peer_addr);  // 接受新连接，返回新socket
		// 主动连接服务器（客户端用）：0成功，SocketUtils::kConnectInProgress表示非阻塞连接进行中，其他值表示失败
		int Connect(const Address& server_addr);

		// 选项设置
		void SetReuseAddr(bool enable = true);
		void SetReusePort(bool enable = true);
		void SetNonBlocking(bool enable = true);

		// 工具接口
		SocketType GetFd() const;		// 获取原生socket句柄
		void Close();			// 主动关闭socket
		bool IsValid() const;   // 检查socket是否有效

		Address GetLocalAddress() const;
		Address GetPeerAddress() const;

	private:
		SocketType m_sockfd = INVALID_SOCKET_VALUE; // 原生socket句柄
	};
}