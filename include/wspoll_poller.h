
// wspoll_poller.h
// Windows平台下基于WSAPoll的Poller实现，供EventLoop使用
#pragma once

#include "poller.h"
#ifdef _WIN32
#include <winsock2.h>
#undef ERROR

namespace TcFrame
{
	class EventLoop;

	class WSPollPoller :public Poller
	{
	public:
		explicit WSPollPoller(EventLoop* loop);
		~WSPollPoller() override;

		// 实现基类Poller接口
		int Poll(int timeout_ms, std::vector<Channel*>& active_channels) override;
		void UpdateChannel(Channel* channel) override;
		void RemoveChannel(Channel* channel) override;

	private:
		// 将WSAPoll的事件位转换成Channel需要的revents，完成事件映射
		int TransRevents(short events);

	private:
		EventLoop* m_ownerLoop;	// 所属的EventLoop
		std::vector<WSAPOLLFD> m_pollfds;    // WSAPoll需要的pollfd列表，所有监听的socket都在这里
		std::vector<Channel*> m_channels;    // Channel列表，和m_pollfds一一对应，索引直接找到Channel
		std::unordered_map<SocketType, size_t> m_fd_to_idx;
	};
}
#endif // _WIN32