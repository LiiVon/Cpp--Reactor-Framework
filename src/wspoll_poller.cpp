#ifdef _WIN32
// wspoll_poller.cpp
// Windows平台下基于WSAPoll的Poller实现，供EventLoop使用

#include "wspoll_poller.h"
#include "socket_utils.h"
#include "eventloop.h"
#include "channel.h"
#include "logger.h"
#include "global.h"

#undef ERROR

namespace TcFrame
{
	WSPollPoller::WSPollPoller(EventLoop* loop)
		: m_ownerLoop(loop)
	{
		LOG_INFO("WSPollPoller (large connection optimized) created for Windows platform");
	}

	WSPollPoller::~WSPollPoller()
	{
		m_pollfds.clear();
		m_channels.clear();
		m_fd_to_idx.clear();
		LOG_DEBUG("WSPollPoller destroyed");
	}

	int WSPollPoller::TransRevents(short events)
	{
		int revents = 0;

		// POLLIN 有可读数据，映射到Channel的kReadEvent
		if (events & POLLIN)
		{
			revents |= Channel::kReadEvent;
		}

		// POLLOUT 可写，映射到Channel的kWriteEvent
		if (events & POLLOUT)
		{
			revents |= Channel::kWriteEvent;
		}

		// POLLERR 错误，直接保留
		if (events & POLLERR)
		{
			revents |= POLLERR;
		}

		// POLLHUP 对端挂起/关闭，直接保留
		if (events & POLLHUP)
		{
			revents |= POLLHUP;
		}

		// POLLRDHUP 对端关闭写，兼容处理，合并到POLLHUP
#ifdef POLLRDHUP

		if (events & POLLRDHUP)
		{
			revents |= POLLHUP;
		}
#endif

		return revents;
	}

	int WSPollPoller::Poll(int timeout_ms, std::vector<Channel*>& active_channels)
	{
		ULONG num_fds = static_cast<ULONG>(m_pollfds.size());
		int num_events = WSAPoll(m_pollfds.data(), num_fds, timeout_ms);

		if (num_events < 0)
		{
			int err = SocketUtils::GetLastError();
			// WSAEWOULDBLOCK和WSAEINTR是正常非阻塞/中断，不算错误，直接返回0
			if (err == WSAEWOULDBLOCK || err == WSAEINTR)
			{
				return 0;
			}
			LOG_ERROR("WSAPoll failed, error code: " + std::to_string(err) + " " + SocketUtils::GetLastErrorStr(err));
			return -1;
		}

		// 超时，没有事件，直接返回0
		if (num_events == 0)
		{
			return 0;
		}

		// 遍历所有pollfd，找到revents不为0的，转成Channel加入active_channels
		// WSAPoll内核已经标记好revents，用户态只需要遍历一遍，即使十万连接也不会太慢
		for (size_t i = 0; i < m_pollfds.size(); ++i)
		{
			if (m_pollfds[i].revents != 0)
			{
				Channel* channel = m_channels[i];
				assert(channel != nullptr);
				assert(channel->GetFd() == m_pollfds[i].fd); // 检查fd对应，开发阶段找bug

				channel->SetRevents(TransRevents(m_pollfds[i].revents));
				active_channels.push_back(channel);
			}
		}

		return static_cast<int>(active_channels.size());
	}

	void WSPollPoller::UpdateChannel(Channel* channel)
	{
		SocketType fd = channel->GetFd();
		size_t idx = static_cast<size_t>(-1);

		auto it = m_fd_to_idx.find(fd);
		if (it != m_fd_to_idx.end())
		{
			idx = it->second;
		}

		// 如果Channel不关心任何事件，直接删除，不用处理
		if (channel->HasNoEvents())
		{
			if (idx != static_cast<size_t>(-1))
			{
				RemoveChannel(channel);
			}
			return;
		}

		// 构造WSAPOLLFD，设置fd和事件
		WSAPOLLFD pfd;
		memset(&pfd, 0, sizeof(pfd));
		pfd.fd = fd;

		if (channel->IsReading())
		{
			pfd.events |= POLLIN;
		}
		if (channel->IsWriting())
		{
			pfd.events |= POLLOUT;
		}

		if (idx == static_cast<size_t>(-1))
		{
			// 加到列表末尾，同时更新map映射
			m_pollfds.push_back(pfd);
			m_channels.push_back(channel);
			m_fd_to_idx[fd] = m_pollfds.size() - 1;
			channel->SetAdded(true);
			LOG_DEBUG("add new channel to WSPollPoller, fd: " + std::to_string((int)fd));
		}
		else
		{
			// 直接替换原来的pfd，映射不用变
			m_pollfds[idx] = pfd;
			LOG_DEBUG("update channel in WSPollPoller, fd: " + std::to_string((int)fd));
		}
	}

	void WSPollPoller::RemoveChannel(Channel* channel)
	{
		SocketType fd = channel->GetFd();
		auto it = m_fd_to_idx.find(fd);
		if (it == m_fd_to_idx.end())
		{
			LOG_ERROR("RemoveChannel: channel not found, fd: " + std::to_string((int)fd));
			return;
		}

		size_t idx = it->second;
		assert(idx < m_pollfds.size());
		assert(idx < m_channels.size());

		size_t last_idx = m_pollfds.size() - 1;
		if (idx != last_idx) // 如果不是最后一个，才需要交换
		{
			// 交换pollfd和channel
			std::swap(m_pollfds[idx], m_pollfds[last_idx]);
			std::swap(m_channels[idx], m_channels[last_idx]);
			// 更新交换过来的最后一个元素的map下标
			SocketType last_fd = m_pollfds[idx].fd;
			m_fd_to_idx[last_fd] = idx;
		}

		// 删除最后一个元素
		m_pollfds.pop_back();
		m_channels.pop_back();
		// 从map删除被删除的fd的映射
		m_fd_to_idx.erase(it);

		channel->SetAdded(false);
		LOG_DEBUG("remove channel from WSPollPoller, fd: " + std::to_string((int)fd));
	}
}
#endif // _WIN32