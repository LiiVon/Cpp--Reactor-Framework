
// Acceptor.h
// 连接接收器，负责监听端口并accept新连接，属于Reactor模型的底层组件，供TcpServer使用
#pragma once

#include "global.h"
#include "socket.h"
#include "channel.h"
#include "eventloop.h"
#include "address.h"

namespace TcFrame
{
	/*
	@brief: 连接接收器，负责监听端口，accept新连接，回调给上层TcpServer
	属于Reactor的一部分，listen socket交给EventLoop监听，有新连接自动accept
	*/
	class Acceptor
	{
	public:
		// 新连接到来的回调：参数是已accept的新Socket（移动语义转移所有权）和客户端地址
		using NewConnectionCallback = std::function<void(Socket&&, const Address&)>;

		// 构造：绑定到指定EventLoop和监听地址
		Acceptor(EventLoop* loop, const Address& listen_addr);
		~Acceptor();

		// 禁止拷贝，每个Acceptor唯一对应一个监听端口
		Acceptor(const Acceptor&) = delete;
		Acceptor& operator=(const Acceptor&) = delete;

		void Listen(); // 开始监听，必须在loop线程调用
		void Stop(); // 停止监听新连接，保留listen socket供后续析构关闭
		void SetNewConnectionCallback(const NewConnectionCallback& cb); // 设置新连接回调
		bool IsListening() const; // 判断是否正在监听

	private:
		void HandleAccept(); // 处理新连接：accept -> 回调给上层，EventLoop触发读事件调用

	private:
		EventLoop* m_loop; // 所属的EventLoop，所有操作都在这个loop线程执行
		Socket m_listen_socket; // 监听socket，唯一对应监听端口
		std::unique_ptr<Channel> m_listen_channel; // listen socket的Channel，交给EventLoop监听
		NewConnectionCallback m_new_connection_callback; // 新连接到来的回调
		bool m_is_listening; // 是否正在监听
	};
}
