#include "eventloop.h"
// EventLoop.cpp
// Reactor核心事件循环实现，负责IO事件分发、跨线程任务、定时任务
#include "channel.h"
#include "socket_utils.h"
#include "poller.h"
#include "logger.h"
#include "global.h"

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#undef ERROR

#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#else
#error "Unsupported platform: EventLoop currently supports only Windows and Linux"
#endif

#endif

namespace TcFrame
{
	// 线程本地存储定义，每个线程一个实例
	thread_local EventLoop* t_currentThreadLoop = nullptr;

#ifdef _WIN32

	void EventLoop::CreateWakeupPair(SocketType& readFd, SocketType& writeFd)
	{
		// Windows下用本地回环socket pair实现唤醒
		SocketType listener = socket(AF_INET, SOCK_STREAM, 0);
		if (listener == INVALID_SOCKET_VALUE)
		{
			int err = SocketUtils::GetLastError();
			LOG_FATAL("CreateWakeupPair create listener failed: " + SocketUtils::GetLastErrorStr(err));
		}

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0; // 系统自动分配空闲端口

		int ret = bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
		if (ret != 0)
		{
			int err = SocketUtils::GetLastError();
			LOG_FATAL("CreateWakeupPair bind listener failed: " + SocketUtils::GetLastErrorStr(err));
		}

		listen(listener, 1);

		// 获取系统分配的实际端口
		sockaddr_in local_addr{};
		int len = sizeof(local_addr);
		getsockname(listener, reinterpret_cast<sockaddr*>(&local_addr), &len);

		writeFd = socket(AF_INET, SOCK_STREAM, 0);
		if (writeFd == INVALID_SOCKET_VALUE)
		{
			int err = SocketUtils::GetLastError();
			LOG_FATAL("CreateWakeupPair create writeFd failed: " + SocketUtils::GetLastErrorStr(err));
		}

		ret = connect(writeFd, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr));
		if (ret != 0)
		{
			int err = SocketUtils::GetLastError();
			LOG_FATAL("CreateWakeupPair connect writeFd failed: " + SocketUtils::GetLastErrorStr(err));
		}

		readFd = accept(listener, nullptr, nullptr);
		if (readFd == INVALID_SOCKET_VALUE)
		{
			int err = SocketUtils::GetLastError();
			LOG_FATAL("CreateWakeupPair accept failed: " + SocketUtils::GetLastErrorStr(err));
		}

		SocketUtils::CloseSocket(listener);
		SocketUtils::SetNonBlocking(readFd);
		SocketUtils::SetNonBlocking(writeFd);

		LOG_DEBUG("CreateWakeupPair done, readFd: " + std::to_string(static_cast<int>(readFd)) + ", writeFd: " + std::to_string(static_cast<int>(writeFd)));
	}
#else

	void EventLoop::CreateWakeupPair(SocketType& readFd, SocketType& writeFd)
	{
		// Linux下用eventfd，更简单高效，读写都是同一个fd，大连接下开销更小
		int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (efd < 0)
		{
			LOG_FATAL("CreateWakeupPair eventfd failed: " + std::string(strerror(errno)));
		}
		readFd = efd;
		writeFd = efd;
		LOG_DEBUG("CreateWakeupPair (eventfd) done, fd: " + std::to_string(static_cast<int>(readFd)));
	}
#endif

	EventLoop::EventLoop(bool auto_start)
		: m_quit(false)
		, m_callingPendingFunctors(false)
		, m_wakeupFd(INVALID_SOCKET_VALUE)
		, m_wakeupWriteFd(INVALID_SOCKET_VALUE)
		, m_next_timer_id(1)
	{
		m_threadId = std::this_thread::get_id();

		// 创建平台对应的Poller
		m_poller = std::unique_ptr<Poller>(Poller::CreatePoller(this));

		// 创建跨线程唤醒pair
		CreateWakeupPair(m_wakeupFd, m_wakeupWriteFd);

		// 创建wakeup channel，注册读回调
		m_wakeupChannel = std::make_unique<Channel>(this, m_wakeupFd);
		m_wakeupChannel->SetReadCallback([this]() { HandleWakeupRead(); });
		m_wakeupChannel->EnableReading();

		// 设置线程本地存储
		t_currentThreadLoop = this;

		// 自动启动：开新线程运行Loop
		if (auto_start)
		{
			m_thread = std::thread([this]() {
				// 新线程重新设置线程ID和本地存储，覆盖原来的主线程ID
				m_threadId = std::this_thread::get_id();
				t_currentThreadLoop = this;
				LOG_INFO("EventLoop thread started, thread ID: " + thread_id_to_str(m_threadId));
				this->Loop();
				});
		}

		std::stringstream ss;
		ss << "EventLoop created (large connection optimized), thread ID: " << m_threadId;
		LOG_INFO(ss.str());
	}

	EventLoop::~EventLoop()
	{
		// 先请求退出并唤醒，避免析构时线程仍阻塞在Poll。
		Quit();

		if (m_thread.joinable())
		{
			if (std::this_thread::get_id() != m_thread.get_id())
			{
				m_thread.join();
			}
			else
			{
				LOG_FATAL("EventLoop destroyed from its own worker thread, this is not allowed");
				std::abort();
			}
		}

		// Loop退出后直接从Poller移除唤醒fd，避免跨线程断言。
		if (m_wakeupChannel && m_wakeupChannel->IsAdded())
		{
			m_poller->RemoveChannel(m_wakeupChannel.get());
			m_wakeupChannel->SetAdded(false);
		}

		// 关闭唤醒fd
#ifdef _WIN32

		closesocket(m_wakeupFd);
		if (m_wakeupWriteFd != m_wakeupFd)
		{
			closesocket(m_wakeupWriteFd);
		}
#else

		close(m_wakeupFd);
		if (m_wakeupWriteFd != m_wakeupFd)
		{
			close(m_wakeupWriteFd);
		}
#endif

		t_currentThreadLoop = nullptr;
		LOG_INFO("EventLoop destroyed, thread ID: " + thread_id_to_str(m_threadId));
	}

	void EventLoop::Loop()
	{
		AssertInLoopThread();
		m_quit = false;
		LOG_INFO("EventLoop start looping, thread ID: " + thread_id_to_str(m_threadId));

		while (!m_quit)
		{
			// 清空上一次的就绪Channel
			m_activeChannels.clear();
			int timeout_ms = GetPollTimeoutMs();
			int numEvents = m_poller->Poll(timeout_ms, m_activeChannels);

			// 处理所有就绪IO事件
			for (Channel* channel : m_activeChannels)
			{
				channel->HandleEvent();
			}

			// 执行跨线程排队的任务
			DoPendingFunctors();
			// 处理过期的定时任务
			ProcessDelayedTasks();
		}

		LOG_INFO("EventLoop stop looping, thread ID: " + thread_id_to_str(m_threadId));
	}

	void EventLoop::Quit()
	{
		m_quit = true;
		// 如果不在Loop线程，唤醒阻塞的Poll
		if (!IsInLoopThread())
		{
			Wakeup();
		}
		LOG_INFO("EventLoop quit requested, thread ID: " + thread_id_to_str(m_threadId));
	}

	void EventLoop::UpdateChannel(Channel* channel)
	{
		AssertInLoopThread();
		m_poller->UpdateChannel(channel);
	}

	void EventLoop::RemoveChannel(Channel* channel)
	{
		AssertInLoopThread();
		m_poller->RemoveChannel(channel);
	}

	void EventLoop::RunInLoop(Functor cb)
	{
		if (IsInLoopThread())
		{
			// 已经在Loop线程，直接执行，无锁
			cb();
		}
		else
		{
			// 不在，排队唤醒
			QueueInLoop(std::move(cb));
		}
	}

	void EventLoop::QueueInLoop(Functor cb)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_pendingFunctors.push_back(std::move(cb));
		}

		// 需要唤醒的两种情况：
		// 1. 调用者不在Loop线程 → Loop阻塞在Poll，需要唤醒处理新任务
		// 2. 调用者在Loop线程，但是当前正在执行pending任务 → 新任务加入，需要唤醒下次循环处理
		if (!IsInLoopThread() || m_callingPendingFunctors.load())
		{
			Wakeup();
		}
	}

	void EventLoop::Wakeup()
	{
		// 写一个8字节数据，唤醒读端，多次唤醒也只会触发一次读，读完就干净
		uint64_t one = 1;
		ssize_t n;
#ifdef _WIN32

		n = send(m_wakeupWriteFd, reinterpret_cast<const char*>(&one), sizeof(one), 0);
#else

		n = write(m_wakeupWriteFd, &one, sizeof(one));
#endif

		if (n != sizeof(one))
		{
			int err = SocketUtils::GetLastError();
			LOG_WARN("Wakeup write partial data: " + SocketUtils::GetLastErrorStr(err));
		}
		LOG_DEBUG("EventLoop wakeup sent");
	}

	bool EventLoop::IsInLoopThread() const
	{
		return m_threadId == std::this_thread::get_id();
	}

	void EventLoop::AssertInLoopThread()
	{
		if (!IsInLoopThread())
		{
			std::stringstream ss;
			ss << "AssertInLoopThread failed: EventLoop created in thread " << m_threadId
				<< ", current thread is " << std::this_thread::get_id();
			LOG_FATAL(ss.str());
			std::abort();
		}
	}

	std::thread& EventLoop::GetThread()
	{
		return m_thread;
	}
	const std::thread& EventLoop::GetThread() const
	{
		return m_thread;
	}

	EventLoop* EventLoop::GetCurrentThreadEventLoop()
	{
		return t_currentThreadLoop;
	}

	void EventLoop::HandleWakeupRead()
	{
		uint64_t one;
		ssize_t n;
		// 循环读完所有唤醒数据，保证不会残留数据触发多余读事件
		do
		{
#ifdef _WIN32

			n = recv(m_wakeupFd, reinterpret_cast<char*>(&one), sizeof(one), 0);
#else

			n = read(m_wakeupFd, &one, sizeof(one));
#endif

			if (n <= 0)
			{
				if (n == 0)
				{
					LOG_DEBUG("Wakeup fd closed by peer");
				}
				else
				{
					int err = SocketUtils::GetLastError();
#ifdef _WIN32

					if (err != WSAEWOULDBLOCK)
#else

					if (err != EAGAIN && err != EWOULDBLOCK)
#endif

					{
						LOG_ERROR("Wakeup read error: " + SocketUtils::GetLastErrorStr(err));
					}
				}
				break;
			}
		} while (true);

		LOG_DEBUG("EventLoop waked up");
	}

	void EventLoop::DoPendingFunctors()
	{
		std::vector<Functor> functors;
		m_callingPendingFunctors.store(true);

		// 缩小锁范围：swap到局部变量，释放锁再执行，减少锁持有时间，大并发下不阻塞
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			functors.swap(m_pendingFunctors);
		}

		// 无锁执行所有任务，非常快
		for (const Functor& functor : functors)
		{
			functor();
		}

		m_callingPendingFunctors.store(false);
	}

	EventLoop::TimerId EventLoop::RunAfter(double delay_seconds, std::function<void()> cb)
	{
		auto delay_ms = std::chrono::milliseconds(static_cast<int64_t>(delay_seconds * 1000));
		if (delay_ms.count() < 0)
		{
			delay_ms = std::chrono::milliseconds(0);
		}
		return RunAt(std::chrono::steady_clock::now() + delay_ms, std::move(cb));
	}

	EventLoop::TimerId EventLoop::RunAt(std::chrono::steady_clock::time_point when, std::function<void()> cb)
	{
		TimerTaskPtr task = std::make_shared<TimerTask>();
		task->id = m_next_timer_id.fetch_add(1);
		task->expire_time = when;
		task->interval = std::chrono::milliseconds(0);
		task->repeat = false;
		task->cancelled = false;
		task->cb = std::move(cb);

		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_timers[task->id] = task;
			m_delayed_tasks.push(task);
		}

		if (!IsInLoopThread() || m_callingPendingFunctors.load())
		{
			Wakeup();
		}

		return task->id;
	}

	EventLoop::TimerId EventLoop::RunEvery(double interval_seconds, std::function<void()> cb)
	{
		auto interval_ms = std::chrono::milliseconds(static_cast<int64_t>(interval_seconds * 1000));
		if (interval_ms.count() <= 0)
		{
			interval_ms = std::chrono::milliseconds(1);
		}

		TimerTaskPtr task = std::make_shared<TimerTask>();
		task->id = m_next_timer_id.fetch_add(1);
		task->expire_time = std::chrono::steady_clock::now() + interval_ms;
		task->interval = interval_ms;
		task->repeat = true;
		task->cancelled = false;
		task->cb = std::move(cb);

		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_timers[task->id] = task;
			m_delayed_tasks.push(task);
		}

		if (!IsInLoopThread() || m_callingPendingFunctors.load())
		{
			Wakeup();
		}

		return task->id;
	}

	bool EventLoop::Cancel(TimerId timer_id)
	{
		bool cancelled = false;
		{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto it = m_timers.find(timer_id);
		if (it == m_timers.end())
		{
			return false;
		}

		it->second->cancelled = true;
		m_timers.erase(it);
		cancelled = true;
		}

		if (cancelled && !IsInLoopThread())
		{
			Wakeup();
		}

		return cancelled;
	}

	void EventLoop::ProcessDelayedTasks()
	{
		AssertInLoopThread();
		std::vector<TimerTaskPtr> ready_tasks;

		// 加锁取出所有过期任务，因为是小顶堆，top就是最早过期的
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			auto now = std::chrono::steady_clock::now();
			// 一直取到第一个没过期的就停止，O(k log n)，k是本次过期的任务数
			while (!m_delayed_tasks.empty() && m_delayed_tasks.top()->expire_time <= now)
			{
				TimerTaskPtr task = m_delayed_tasks.top();
				m_delayed_tasks.pop();

				auto it = m_timers.find(task->id);
				if (task->cancelled || it == m_timers.end())
				{
					continue;
				}

				ready_tasks.push_back(task);
			}
		}

		for (const auto& task : ready_tasks)
		{
			if (!task->cancelled && task->cb)
			{
				task->cb();
			}

			if (task->repeat)
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				auto it = m_timers.find(task->id);
				if (it != m_timers.end() && !task->cancelled)
				{
					task->expire_time = std::chrono::steady_clock::now() + task->interval;
					m_delayed_tasks.push(task);
				}
			}
			else
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_timers.erase(task->id);
			}
		}
	}

	int EventLoop::GetPollTimeoutMs()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto now = std::chrono::steady_clock::now();

		while (!m_delayed_tasks.empty())
		{
			TimerTaskPtr task = m_delayed_tasks.top();
			auto it = m_timers.find(task->id);
			if (task->cancelled || it == m_timers.end())
			{
				m_delayed_tasks.pop();
				continue;
			}

			if (task->expire_time <= now)
			{
				return 0;
			}

			auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(task->expire_time - now).count();
			if (remain <= 0)
			{
				return 0;
			}
			if (remain > 10000)
			{
				return 10000;
			}
			return static_cast<int>(remain);
		}

		return 10000;
	}
}