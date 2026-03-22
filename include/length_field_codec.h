
// LengthFieldCodec.h
// 基于4字节长度头的TCP编解码器，解决半包/粘包，支持心跳帧
#pragma once

#include "global.h"
#include "buffer.h"
#include "tcpconnection.h"
#include "socket_utils.h"

namespace TcFrame
{
	/*
	@brief 基于4字节长度头的编解码器，解决TCP半包/粘包问题
	帧格式：
	- 4字节网络字节序长度N（不含头）
	- N字节payload
	*/
	class LengthFieldCodec
	{
	public:
		using MessageCallback = std::function<void(const TcpConnectionPtr&, const std::string&)>;

		explicit LengthFieldCodec(MessageCallback cb, size_t max_frame_len = 4 * 1024 * 1024);

		// 供TcpServer/TcpClient的消息回调直接调用，自动解帧并回调完整消息
		void OnMessage(const TcpConnectionPtr& conn, Buffer* buffer);

		// 将业务消息封成长度帧并发送
		void Send(const TcpConnectionPtr& conn, const std::string& message);

		// 简单心跳约定：收到PING回PONG
		static bool HandleHeartbeat(const TcpConnectionPtr& conn, const std::string& message);

	private:
		MessageCallback m_message_callback;
		size_t m_max_frame_len;
	};
}
