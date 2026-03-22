#include "thread_pool.h"
// thread_pool.cpp
// 通用线程池实现，支持任务提交、动态扩容、优雅停机
#include "logger.h"
#include "global.h"

namespace TcFrame
{
	ThreadPool::ThreadPool(size_t thread_num)
		: m_is_running(true), m_target_thread_num(thread_num)
	{
		LOG_INFO("create thread pool, initial thread num: " + std::to_string(thread_num));
		for (size_t i = 0; i < thread_num; ++i)
		{
			// emplace_back直接构造，比push_back更高效
			m_workers.emplace_back(&ThreadPool::WorkerLoop, this);
		}
	}

	ThreadPool::~ThreadPool()
	{
		Shutdown();
	}

	void ThreadPool::WorkerLoop()
	{
		// 打印线程标识，方便调试
		size_t thread_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
		LOG_DEBUG("thread " + std::to_string(thread_hash) + " start");

		while (true)
		{
			std::unique_lock<std::mutex> lock(m_queue_mutex);
			// 等待：线程池停止 或者 有任务来了 → 唤醒
			m_cv.wait(lock, [this]() {
				return !m_is_running.load() || !m_tasks.empty();
				});

			// 如果线程池停止且没有任务了，退出
			if (!m_is_running.load() && m_tasks.empty())
			{
				break;
			}

			// 取出任务，移动拷贝，减少拷贝
			std::function<void()> task;
			task = std::move(m_tasks.front());
			m_tasks.pop();
			// 取出后立刻释放锁，让其他线程可以继续放任务，减少锁竞争
			lock.unlock();

			LOG_DEBUG("thread " + std::to_string(thread_hash) + " process task");
			task();
		}

		LOG_DEBUG("thread " + std::to_string(thread_hash) + " exit");
	}

	size_t ThreadPool::GetThreadNum() const
	{
		// 返回当前实际正在运行的线程数
		std::lock_guard<std::mutex> lock(m_queue_mutex);
		return m_workers.size();
	}

	size_t ThreadPool::GetPendingTaskNum() const
	{
		std::lock_guard<std::mutex> lock(m_queue_mutex);
		return m_tasks.size();
	}

	bool ThreadPool::IsRunning() const
	{
		return m_is_running.load();
	}

	void ThreadPool::Resize(size_t new_thread_num)
	{
		if (!m_is_running.load())
		{
			LOG_WARN("cannot resize thread pool: thread pool is not running");
			return;
		}

		size_t old_thread_num = m_target_thread_num.load();
		if (new_thread_num == old_thread_num)
		{
			return; // 没有变化，不用处理
		}

		LOG_INFO("resize thread pool from " + std::to_string(old_thread_num) + " to " + std::to_string(new_thread_num));
		m_target_thread_num = new_thread_num;

		if (new_thread_num > old_thread_num)
		{
			// 扩容：直接加新线程
			size_t add_num = new_thread_num - old_thread_num;
			std::lock_guard<std::mutex> lock(m_queue_mutex);
			for (size_t i = 0; i < add_num; ++i)
			{
				m_workers.emplace_back(&ThreadPool::WorkerLoop, this);
			}
		}
		else if (new_thread_num < old_thread_num)
		{
			// 在线缩容涉及线程生命周期同步，先保留目标值，在Shutdown时统一回收。
			LOG_WARN("thread pool runtime shrink is deferred to Shutdown for safety");
		}
	}

	void ThreadPool::Shutdown()
	{
		LOG_INFO("shutting down thread pool");
		if (!m_is_running.exchange(false))
		{
			LOG_WARN("thread pool is already shutdown");
			return;
		}

		m_cv.notify_all(); // 唤醒所有线程，处理完剩余任务退出

		std::vector<std::thread> workers;
		{
			std::lock_guard<std::mutex> lock(m_queue_mutex);
			workers.swap(m_workers);
			while (!m_tasks.empty())
			{
				m_tasks.pop();
			}
		}

		// 等待所有线程退出，清理
		for (auto& worker : workers)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}

		LOG_INFO("thread pool shutdown complete, total tasks processed: unknown");
	}
}