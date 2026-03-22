#include "length_field_codec.h"
// LengthFieldCodec.cpp
// 基于4字节长度头的TCP编解码器实现，解决半包/粘包，支持心跳帧
#include "logger.h"

namespace TcFrame
{
	LengthFieldCodec::LengthFieldCodec(MessageCallback cb, size_t max_frame_len)
		: m_message_callback(std::move(cb))
		, m_max_frame_len(max_frame_len)
	{
	}

	void LengthFieldCodec::OnMessage(const TcpConnectionPtr& conn, Buffer* buffer)
	{
		while (buffer->ReadableBytes() >= sizeof(uint32_t))
		{
			const int32_t payload_len = buffer->PeekInt32();
			if (payload_len < 0 || static_cast<size_t>(payload_len) > m_max_frame_len)
			{
				LOG_ERROR("invalid frame length: " + std::to_string(payload_len) + ", close connection: " + conn->GetName());
				conn->ForceClose();
				return;
			}

			const size_t frame_len = sizeof(uint32_t) + static_cast<size_t>(payload_len);
			if (buffer->ReadableBytes() < frame_len)
			{
				return;
			}

			buffer->Retrieve(sizeof(uint32_t));
			std::string message = buffer->RetrieveAsString(static_cast<size_t>(payload_len));
			if (m_message_callback)
			{
				m_message_callback(conn, message);
			}
		}
	}

	void LengthFieldCodec::Send(const TcpConnectionPtr& conn, const std::string& message)
	{
		if (!conn || !conn->IsConnected())
		{
			return;
		}

		const uint32_t net_len = SocketUtils::HostToNetLong(static_cast<uint32_t>(message.size()));
		std::string framed;
		framed.resize(sizeof(uint32_t) + message.size());
		std::memcpy(&framed[0], &net_len, sizeof(uint32_t));
		if (!message.empty())
		{
			std::memcpy(&framed[sizeof(uint32_t)], message.data(), message.size());
		}
		conn->Send(framed);
	}

	bool LengthFieldCodec::HandleHeartbeat(const TcpConnectionPtr& conn, const std::string& message)
	{
		if (message == "PING")
		{
			const uint32_t net_len = SocketUtils::HostToNetLong(4);
			std::string pong;
			pong.resize(sizeof(uint32_t) + 4);
			std::memcpy(&pong[0], &net_len, sizeof(uint32_t));
			std::memcpy(&pong[sizeof(uint32_t)], "PONG", 4);
			conn->Send(pong);
			return true;
		}
		return false;
	}
}
