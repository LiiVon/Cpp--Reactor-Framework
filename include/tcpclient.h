
// tcpclient.h
// TCP客户端，封装outgoing连接，复用TcpConnection，支持自动重连
#pragma once

#include "global.h"
#include "socket.h"
#include "buffer.h"
#include "eventloop.h"
#include "channel.h"
#include "tcpconnection.h"
#include "address.h"

namespace TcFrame
{
	/*
	@brief: TCP客户端，对应一个 outgoing TCP连接
	- 绑定到一个EventLoop，所有操作都在Loop线程执行，符合One Loop Per Thread
	- 完全复用TcpConnection，和项目现有接口完全对齐
	*/
	class TcpClient
	{
	public:

		using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
		using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
		using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
		using CloseCallback = std::function<void(const TcpConnectionPtr&)>;

		// 构造：
		TcpClient(EventLoop* loop, const Address& server_addr, std::string name = "TcpClient");
		~TcpClient();

		TcpClient(const TcpClient&) = delete;
		TcpClient& operator=(const TcpClient&) = delete;

		// 对外接口：
		// Connect/Disconnect 必须在绑定的 EventLoop 线程调用，保证线程安全
		void Connect(); // 启动连接
		void Disconnect(); // 断开连接

		void Send(const std::string& message);
		void Send(const void* data, size_t len);

		void SetConnectionCallback(const ConnectionCallback& cb);
		void SetMessageCallback(const MessageCallback& cb);
		void SetWriteCompleteCallback(const WriteCompleteCallback& cb);
		void SetCloseCallback(const CloseCallback& cb);

		// 工具接口
		TcpConnectionPtr GetConnection() const;
		bool IsConnected() const;
		EventLoop* GetLoop() const;
		const std::string& GetName() const;

		// 唯一可选功能：自动重连，Worker需要，不影响其他接口，默认关闭
		void SetAutoReconnect(bool enable);

	private:
		// 内部方法，完全复用你现有的逻辑
		void HandleConnect(Socket&& client_socket);
		void HandleRemoveConnection(const TcpConnectionPtr& conn);
		void DoReconnect();

	private:

		EventLoop* m_loop;               // 绑定的EventLoop，所有操作都在这里
		Address m_server_addr;           // 要连接的服务器地址
		std::string m_name;              // 客户端名字，日志识别，和你TcpConnection一致
		std::unique_ptr<Socket> m_socket; // 连接完成前持有socket

		TcpConnectionPtr m_connection;   // 连接建立后持有TcpConnection，完全复用

		ConnectionCallback m_connection_callback;
		MessageCallback m_message_callback;
		WriteCompleteCallback m_write_complete_callback;
		CloseCallback m_close_callback;

		std::atomic<bool> m_connecting; // 是否正在连接
		std::atomic<bool> m_started;    // 是否已经启动
		bool m_auto_reconnect{ false };   // 是否自动重连，默认关闭
		int m_reconnect_delay_ms{ 3000 }; // 重连延迟，默认3秒，不用就不改
	};

	using TcpClientPtr = std::shared_ptr<TcpClient>;
}
