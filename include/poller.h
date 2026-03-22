
// poller.h
// IO多路复用抽象基类，屏蔽平台差异，供EventLoop统一调用
#pragma once
#include "global.h"

namespace TcFrame
{
	class EventLoop;
	class Channel;

	/*
	@brief: IO多路复用抽象基类
	核心职责：检测一堆Channel中哪些已经就绪，把就绪Channel返回给EventLoop
	不同平台有不同实现：
	- Linux: EpollPoller (epoll)
	- Windows: WSPollPoller (WSPoll/IOCP)
	*/
	class Poller
	{
	public:
		Poller() = default;
		virtual ~Poller() = default;

		Poller(const Poller&) = delete;
		Poller& operator=(const Poller&) = delete;

		virtual int Poll(int timeout_ms, std::vector<Channel*>& active_channels) = 0;

		// 更新Channel的监听事件，EventLoop调用，Poller自动处理添加/修改
		virtual void UpdateChannel(Channel* channel) = 0;

		// 移除Channel，停止监听该Channel，从Poller中删除
		virtual void RemoveChannel(Channel* channel) = 0;

		// 工厂方法：根据当前编译平台，创建对应平台的Poller实现
		static Poller* CreatePoller(EventLoop* loop);
	};
}