#ifdef __linux__
// EpollPoller.cpp
// Linux平台下基于epoll的Poller实现，负责IO多路复用

#include "epoll_poller.h"
#include "eventloop.h"
#include "channel.h"
#include "logger.h"
#include "global.h"
#include <unistd.h>
#include <sys/epoll.h>

namespace TcFrame
{
	EpollPoller::EpollPoller(EventLoop* loop)
		: m_ownerLoop(loop)
	{
		// 创建epoll实例，尺寸传一个大于0的数就行，内核现在忽略这个尺寸
		m_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
		if (m_epoll_fd < 0)
		{
			LOG_FATAL("EpollPoller create epoll_fd failed: " + std::string(strerror(errno)));
		}
		// 预分配空间，避免频繁扩容，大连接下一次分配够
		m_events.resize(128);
		LOG_INFO("EpollPoller created for Linux platform, epoll_fd: " + std::to_string(m_epoll_fd));
	}

	EpollPoller::~EpollPoller()
	{
		close(m_epoll_fd);
		m_fd_to_channel.clear();
		LOG_DEBUG("EpollPoller destroyed, epoll_fd: " + std::to_string(m_epoll_fd));
	}

	int EpollPoller::TransRevents(uint32_t events)
	{
		int revents = 0;

		if (events & EPOLLIN)
		{
			revents |= Channel::kReadEvent;
		}
		if (events & EPOLLOUT)
		{
			revents |= Channel::kWriteEvent;
		}
		if (events & EPOLLERR)
		{
			revents |= POLLERR;
		}
		if (events & EPOLLHUP)
		{
			revents |= POLLHUP;
		}
#ifdef EPOLLRDHUP

		if (events & EPOLLRDHUP)
		{
			revents |= POLLHUP;
		}
#endif

		return revents;
	}

	int EpollPoller::Poll(int timeout_ms, std::vector<Channel*>& active_channels)
	{
		int num_events = epoll_wait(m_epoll_fd, m_events.data(), (int)m_events.size(), timeout_ms);
		if (num_events < 0)
		{
			if (errno == EINTR)
			{
				return 0; // 被中断，正常情况，直接返回0
			}
			LOG_ERROR("EpollPoller epoll_wait error: " + std::string(strerror(errno)));
			return -1;
		}

		if (num_events == 0)
		{
			return 0; // 超时，没有事件
		}

		// 如果返回的事件填满了缓冲区，扩容两倍，下次就能装下更多事件
		if ((size_t)num_events == m_events.size())
		{
			m_events.resize(m_events.size() * 2);
		}

		// 遍历就绪事件，O(1)找到Channel，填充revents，加入active_channels
		for (int i = 0; i < num_events; ++i)
		{
			SocketType fd = (SocketType)m_events[i].data.fd;
			auto it = m_fd_to_channel.find(fd);
			if (it == m_fd_to_channel.end() || it->second == nullptr)
			{
				LOG_WARN("epoll returned unknown fd, skip, fd: " + std::to_string((int)fd));
				continue;
			}

			Channel* channel = it->second;
			assert(channel->GetFd() == fd);

			channel->SetRevents(TransRevents(m_events[i].events));
			active_channels.push_back(channel);
		}

		return num_events;
	}

	void EpollPoller::UpdateChannel(Channel* channel)
	{
		SocketType fd = channel->GetFd();
		// O(1)判断是否已经存在
		if (channel->HasNoEvents())
		{
			// 不关心任何事件，直接删除
			if (m_fd_to_channel.count(fd))
			{
				RemoveChannel(channel);
			}
			return;
		}

		if (m_fd_to_channel.count(fd))
		{
			// 已经存在，修改事件
			UpdateEpoll(EPOLL_CTL_MOD, channel);
			LOG_DEBUG("update channel in EpollPoller, fd: " + std::to_string((int)fd));
		}
		else
		{
			// 不存在，新增
			m_fd_to_channel[fd] = channel;
			UpdateEpoll(EPOLL_CTL_ADD, channel);
			channel->SetAdded(true);
			LOG_DEBUG("add new channel to EpollPoller, fd: " + std::to_string((int)fd));
		}
	}

	void EpollPoller::RemoveChannel(Channel* channel)
	{
		SocketType fd = channel->GetFd();
		if (!m_fd_to_channel.count(fd))
		{
			LOG_ERROR("RemoveChannel: channel not found, fd: " + std::to_string((int)fd));
			return;
		}

		// 从epoll删除，再从map删除
		UpdateEpoll(EPOLL_CTL_DEL, channel);
		m_fd_to_channel.erase(fd);
		channel->SetAdded(false);
		LOG_DEBUG("remove channel from EpollPoller, fd: " + std::to_string((int)fd));
	}

	void EpollPoller::UpdateEpoll(int op, Channel* channel)
	{
		SocketType fd = channel->GetFd();
		struct epoll_event event;
		memset(&event, 0, sizeof(event));
		event.data.fd = (int)fd;
		event.events = channel->GetEvents();

		if (epoll_ctl(m_epoll_fd, op, (int)fd, &event) < 0)
		{
			LOG_ERROR("epoll_ctl failed, op: " + std::to_string(op) + ", fd: " + std::to_string((int)fd) + ", error: " + strerror(errno));
		}
	}
}
#endif // __linux__