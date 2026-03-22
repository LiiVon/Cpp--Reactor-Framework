#include "poller.h"
// poller.cpp
// IO多路复用抽象基类实现，屏蔽平台差异，供EventLoop统一调用
#include "eventloop.h"
#include "logger.h"
#include "channel.h"

// 根据平台引入对应具体实现头文件
#ifdef _WIN32

#include "wspoll_poller.h"

#elif defined(__linux__)

#include "epoll_poller.h"

#endif

namespace TcFrame
{
	Poller* Poller::CreatePoller(EventLoop* loop)
	{
#ifdef _WIN32

		LOG_INFO("create WSPollPoller for Windows platform");
		return new WSPollPoller(loop);
#elif defined(__linux__)

		LOG_INFO("create EpollPoller for Linux platform");
		return new EpollPoller(loop);
#else

		LOG_FATAL("unsupported platform: only Windows and Linux are supported");
		return nullptr;
#endif
	}
}