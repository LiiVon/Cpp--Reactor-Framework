#include "channel.h"
// Channel.cpp
// Reactor模式事件通道实现，绑定fd，描述关心的事件并分发回调
#include "eventloop.h"
#include "global.h"
#include "logger.h"

namespace TcFrame
{
	// 静态成员初始化，复用系统事件常量，和poll/epoll直接对应
	const int Channel::kNoneEvent = 0;
	const int Channel::kReadEvent = POLLIN;
	const int Channel::kWriteEvent = POLLOUT;

	Channel::Channel(EventLoop* loop, SocketType fd)
		: m_ownerLoop(loop)
		, m_fd(fd)
		, m_events(kNoneEvent)
		, m_revents(0)
		, m_added(false)
	{
		LOG_DEBUG("create channel, fd: " + std::to_string((int)m_fd));
	}

	Channel::~Channel()
	{
		LOG_DEBUG("destroy channel, fd: " + std::to_string((int)m_fd));
		// 析构时尽量关闭事件；若不在所属Loop线程，不能触发UpdateChannel避免跨线程断言
		if (!HasNoEvents())
		{
			if (m_ownerLoop && m_ownerLoop->IsInLoopThread())
			{
				DisableAll();
			}
			else
			{
				m_events = kNoneEvent;
			}
		}
	}

	void Channel::EnableReading()
	{
		m_events |= kReadEvent;
		m_ownerLoop->UpdateChannel(this);
	}

	void Channel::DisableReading()
	{
		m_events &= ~kReadEvent;
		m_ownerLoop->UpdateChannel(this);
	}

	void Channel::EnableWriting()
	{
		m_events |= kWriteEvent;
		m_ownerLoop->UpdateChannel(this);
	}

	void Channel::DisableWriting()
	{
		m_events &= ~kWriteEvent;
		m_ownerLoop->UpdateChannel(this);
	}

	void Channel::DisableAll()
	{
		m_events = kNoneEvent;
		m_ownerLoop->UpdateChannel(this);
	}

	void Channel::HandleEvent()
	{
		LOG_DEBUG("handle event for fd " + std::to_string((int)m_fd) + ", revents: " + std::to_string(m_revents));

		// --------------- 优先级：错误 > 关闭 > 读 > 写，标准Reactor处理顺序 ---------------
		// 1. 优先处理错误
		if (m_revents & POLLERR)
		{
			LOG_ERROR("channel error event, fd: " + std::to_string((int)m_fd));
			if (m_error_callback)
			{
				m_error_callback();
			}
			// 错误之后一般连接都废了，直接走关闭逻辑，就算没注册关闭回调也关
			if (m_close_callback)
			{
				m_close_callback();
			}
			return;
		}

		// 2. 处理连接关闭（对端关闭了发送窗口）
		if (m_revents & POLLHUP)
		{
			LOG_DEBUG("channel got POLLHUP, fd: " + std::to_string((int)m_fd));
			if (m_close_callback)
			{
				m_close_callback();
			}
			return;
		}

		// 3. 处理读事件
		if (m_revents & kReadEvent)
		{
			if (m_read_callback)
			{
				m_read_callback();
			}
		}

		// 4. 处理写事件
		if (m_revents & kWriteEvent)
		{
			if (m_write_callback)
			{
				m_write_callback();
			}
		}
	}

	void Channel::SetReadCallback(std::function<void()> cb)
	{
		m_read_callback = std::move(cb);
	}

	void Channel::SetWriteCallback(std::function<void()> cb)
	{
		m_write_callback = std::move(cb);
	}

	void Channel::SetErrorCallback(std::function<void()> cb)
	{
		m_error_callback = std::move(cb);
	}

	void Channel::SetCloseCallback(std::function<void()> cb)
	{
		m_close_callback = std::move(cb);
	}

	int Channel::GetEvents() const
	{
		return m_events;
	}

	void Channel::SetRevents(int revents)
	{
		m_revents = revents;
	}

	bool Channel::IsWriting() const
	{
		return (m_events & kWriteEvent) != 0;
	}

	bool Channel::IsReading() const
	{
		return (m_events & kReadEvent) != 0;
	}

	bool Channel::HasNoEvents() const
	{
		return m_events == kNoneEvent;
	}

	SocketType Channel::GetFd() const
	{
		return m_fd;
	}

	EventLoop* Channel::GetOwnerLoop() const
	{
		return m_ownerLoop;
	}

	void Channel::SetAdded(bool added)
	{
		m_added = added;
	}

	bool Channel::IsAdded() const
	{
		return m_added;
	}
}