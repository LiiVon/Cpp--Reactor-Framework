
// tcpconnection.h
// TCP连接封装，管理连接生命周期、事件回调、缓冲区，线程安全
#pragma once

#include "global.h"
#include "socket.h"
#include "buffer.h"
#include "eventloop.h"
#include "channel.h"

namespace TcFrame
{
	class TcpConnection;

	using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

	enum class ConnectionState
	{
		Connecting,
		Connected,
		Disconnecting,
		Disconnected
	};

	/*
	@brief: TCP连接封装，管理一个连接的生命周期，处理读写事件，回调上层
	每个连接对应一个Socket，绑定到一个EventLoop，所有操作都在Loop线程执行
	*/
	class TcpConnection
		:public std::enable_shared_from_this<TcpConnection>
	{
	public:
		// 回调函数类型定义
		using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
		using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
		using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
		using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
		using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, size_t)>;

	public:
		// 构造：loop是所属EventLoop，name是连接名，socket是已accept的Socket，peer_addr是对端地址
		TcpConnection(EventLoop* loop, std::string name, Socket socket, Address peer_addr);
		~TcpConnection();

		TcpConnection(const TcpConnection&) = delete;
		TcpConnection& operator=(const TcpConnection&) = delete;

		//  对外用户接口：连接建立/销毁，由TcpServer调用
		void ConnectEstablished(); // 连接建立，注册事件，回调连接建立函数
		void ConnectDestroyed(); // 连接销毁，注销事件，回调连接销毁函数

		// 发送数据，线程安全，任意线程都可以调用
		void Send(const std::string& message);
		void Send(const void* data, size_t len);
		void Shutdown(); // 优雅关闭：等数据发完再关闭
		void ForceClose(); // 强制关闭，立即关闭

		bool IsConnected() const;
		EventLoop* GetLoop() const;
		SocketType GetFd() const; // 改成SocketType，避免64位截断，日志也能正确打印
		const std::string& GetName() const;
		Address GetPeerAddr() const;
		int64_t GetLastActiveMs() const;
		size_t GetPendingWriteBytes() const;
		uint64_t GetBytesReadTotal() const;
		uint64_t GetBytesWrittenTotal() const;

		//  设置回调函数，由TcpServer调用
		void SetConnectionCallback(const ConnectionCallback& cb);
		void SetMessageCallback(const MessageCallback& cb);
		void SetWriteCompleteCallback(const WriteCompleteCallback& cb);
		void SetCloseCallback(const CloseCallback& cb);
		void SetHighWaterMarkCallback(size_t high_water_mark, const HighWaterMarkCallback& cb);

		// 设置连接上下文，用来存用户自定义数据（比如每个连接自己的Parser）
		void SetContext(std::any context);
		// 获取连接上下文
		std::any& GetContext();

	private:
		// 工具函数：判断是不是正常的阻塞错误，不关闭连接
		static bool IsNormalWouldBlock(int err);

		// 事件回调函数，由Channel调用，都在Loop线程执行
		void HandleRead();
		void HandleWrite();
		void HandleClose();
		void HandleError();

		// 实际发送数据的函数，必须在Loop线程调用
		void SendInLoop(const std::string& message);
		void SendInLoop(const void* data, size_t len);
		void ShutdownInLoop();
		void ForceCloseInLoop();
		void RefreshActiveTime();

	private:
		EventLoop* m_loop;               // 所属的EventLoop，所有操作都在这个线程执行
		std::string m_name;              // 连接名，一般是"ip:port"，方便日志识别
		Socket m_socket;                 // 持有的Socket，RAII管理，析构自动关闭
		Address m_peer_addr;             // 对端地址
		std::unique_ptr <Channel> m_channel; // Socket对应的Channel，交给EventLoop监听事件
		std::any m_context;				// 用户自定义上下文，存每个连接自己的数据
		std::atomic<int64_t> m_last_active_ms; // 最近活跃时间戳（毫秒）

		ConnectionState m_state;         // 连接状态，状态机流转
		Buffer m_input_buffer;           // 读缓冲区：对端发来的数据先读到这里，再给上层处理
		Buffer m_output_buffer;          // 写缓冲区：要发送的数据先写到这里，内核可写再发出去
		std::atomic<size_t> m_pending_write_bytes; // 当前待发送字节数（output buffer近似值）
		std::atomic<uint64_t> m_total_bytes_read; // 连接累计读字节
		std::atomic<uint64_t> m_total_bytes_written; // 连接累计写字节

		// 各个事件回调，由上层设置
		ConnectionCallback m_connection_callback;   // 连接建立/关闭回调
		MessageCallback m_message_callback;          // 读到消息回调
		WriteCompleteCallback m_write_complete_callback; // 写完成回调（所有数据发完）
		CloseCallback m_close_callback;              // 连接关闭回调
		size_t m_high_water_mark;                    // 背压高水位阈值（字节）
		HighWaterMarkCallback m_high_water_mark_callback; // 达到高水位回调
	};
}
