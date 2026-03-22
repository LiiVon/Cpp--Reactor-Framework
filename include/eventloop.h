
// EventLoop.h
// Reactor核心事件循环，负责IO事件分发、跨线程任务、定时任务，One Loop Per Thread
#pragma once

#include "global.h"

namespace TcFrame
{
	class Poller;
	class Channel;

	/*
	@brief: Reactor模式核心事件循环类，One Loop Per Thread，适配大连接+多定时任务场景
	职责：
	1. 循环调用Poller等待IO事件，处理就绪Channel
	2. 执行跨线程投递的任务，线程安全
	3. 支持延迟定时任务，基于优先堆实现，多定时任务下性能优异
	*/
	class EventLoop
	{
	public:
		// 跨线程任务类型
		using Functor = std::function<void()>;
		using TimerId = uint64_t;

		// auto_start = true 自动启动新线程运行Loop，false 不自动启动（给主线程用）
		explicit EventLoop(bool auto_start = true);
		~EventLoop();

		EventLoop(const EventLoop&) = delete;
		EventLoop& operator=(const EventLoop&) = delete;

		void Loop(); // 启动事件循环，阻塞直到Quit
		void Quit(); // 安全退出事件循环

		// 更新/移除Channel，交给Poller处理，必须在Loop线程调用
		void UpdateChannel(Channel* channel);
		void RemoveChannel(Channel* channel);

		// 跨线程安全接口：让回调在Loop所在线程执行
		void RunInLoop(Functor cb);
		// 把回调放到队列，下次Loop循环执行
		void QueueInLoop(Functor cb);
		// 唤醒阻塞在Poll的Loop
		void Wakeup();

		// 线程判断断言
		bool IsInLoopThread() const; // 判断当前调用线程是否是Loop所在线程
		void AssertInLoopThread(); // 断言必须在Loop线程，否则报错退出，调试用

		std::thread& GetThread();
		const std::thread& GetThread() const;

		// 获取当前线程的EventLoop，线程本地存储，没有返回nullptr
		static EventLoop* GetCurrentThreadEventLoop();

		// 一次性定时任务：delay_seconds秒后执行，返回定时器ID
		TimerId RunAfter(double delay_seconds, std::function<void()> cb);
		// 一次性定时任务：在指定时间点执行，返回定时器ID
		TimerId RunAt(std::chrono::steady_clock::time_point when, std::function<void()> cb);
		// 周期定时任务：每interval_seconds秒执行一次，返回定时器ID
		TimerId RunEvery(double interval_seconds, std::function<void()> cb);
		// 取消定时器，成功取消返回true
		bool Cancel(TimerId timer_id);

	private:
		struct TimerTask
		{
			TimerId id;
			std::chrono::steady_clock::time_point expire_time;
			std::chrono::milliseconds interval;
			bool repeat;
			bool cancelled;
			std::function<void()> cb;
		};

		using TimerTaskPtr = std::shared_ptr<TimerTask>;

		// 优先队列比较器：小根堆，最早过期的排在最前面
		struct DelayedTaskCompare
		{
			bool operator()(const TimerTaskPtr& a, const TimerTaskPtr& b) const
			{
				return a->expire_time > b->expire_time;
			}
		};

		// 处理wakeup读事件，唤醒后清空唤醒数据
		void HandleWakeupRead();
		// 执行所有排队的跨线程任务
		void DoPendingFunctors();
		// 处理所有过期的定时任务，执行回调
		void ProcessDelayedTasks();
		// 根据最近定时任务计算Poll超时时间
		int GetPollTimeoutMs();

		// 创建跨线程唤醒的pair（Windows socket pair / Linux eventfd）
		static void CreateWakeupPair(SocketType& readFd, SocketType& writeFd);

	private:
		std::thread m_thread;               // 运行Loop的线程（auto_start=true时有效）
		std::thread::id m_threadId;         // Loop所在线程ID，用于断言判断
		std::atomic<bool> m_quit;           // 是否退出循环，atomic保证多线程可见
		std::atomic<bool> m_callingPendingFunctors; // 是否正在执行pending任务，用于判断是否需要唤醒

		std::unique_ptr<Poller> m_poller;   // IO多路复用器
		std::vector<Channel*> m_activeChannels; // 本次循环就绪的Channel列表

		// 跨线程唤醒相关
		SocketType m_wakeupFd;         // 唤醒读fd
		SocketType m_wakeupWriteFd;    // 唤醒写fd
		std::unique_ptr<Channel> m_wakeupChannel; // 唤醒fd的Channel，监听读事件

		std::mutex m_mutex;                 // 保护m_pendingFunctors和m_delayed_tasks
		std::vector<Functor> m_pendingFunctors; // 跨线程投递的待处理任务
		std::atomic<TimerId> m_next_timer_id; // 自增定时器ID

		// 优先队列存储定时任务
		std::priority_queue<TimerTaskPtr, std::vector<TimerTaskPtr>, DelayedTaskCompare> m_delayed_tasks;
		std::unordered_map<TimerId, TimerTaskPtr> m_timers;
	};

	// 线程本地存储：每个线程唯一的EventLoop实例
	extern thread_local EventLoop* t_currentThreadLoop;
}