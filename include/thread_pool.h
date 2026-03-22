
// thread_pool.h
// 通用线程池，支持任务提交、动态扩容、优雅停机
#pragma once
#include "global.h"
#include "logger.h"

namespace TcFrame
{
	class ThreadPool
	{
	public:
		explicit ThreadPool(size_t thread_num = 8);
		~ThreadPool();
		ThreadPool(const ThreadPool&) = delete;
		ThreadPool& operator=(const ThreadPool&) = delete;

		// 对外接口：提交任务
		// 支持任意类型的函数，支持任意参数，自动推导返回类型
		// 用户拿到std::future，可以异步获取结果、等待完成
		template<typename Func, typename... Args>
		auto Commit(Func&& func, Args&&... args)
			-> std::future<typename std::invoke_result_t<Func, Args...>>;

		// 停止线程池， graceful shutdown：执行完所有 pending 任务再退出
		void Shutdown();

		// 获取当前线程数、等待任务数
		size_t GetThreadNum() const;
		size_t GetPendingTaskNum() const;

		// 动态调整线程池大小
		void Resize(size_t new_thread_num);

		bool IsRunning() const;

	private:
		// 工作线程入口函数
		void WorkerLoop();

	private:
		std::vector<std::thread> m_workers;			// 所有工作线程
		std::queue<std::function<void()>> m_tasks;	// 等待处理的任务队列
		mutable std::mutex m_queue_mutex;			// 保护任务队列的互斥锁，mutable支持const方法加锁
		std::condition_variable m_cv;               // 条件变量，唤醒空闲线程
		std::atomic<bool> m_is_running;				// 线程池运行标志
		std::atomic<size_t> m_target_thread_num;	// 目标线程数（动态调整用）
	};

	template<typename Func, typename... Args>
	auto ThreadPool::Commit(Func&& func, Args&&... args)
		-> std::future<typename std::invoke_result_t<Func, Args...>>
	{
		using ReturnType = typename std::invoke_result_t<Func, Args...>;

		// 用shared_ptr管理packaged_task，解决packaged_task不能拷贝的问题
		auto task = std::make_shared<std::packaged_task<ReturnType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
		);

		// 给用户返回future，用于获取结果
		std::future<ReturnType> result_future = task->get_future();

		// 最小锁作用域，减少锁竞争
		{
			std::lock_guard<std::mutex> lock(m_queue_mutex);
			if (!m_is_running.load())
			{
				LOG_ERROR("commit task to already stopped thread pool, reject task");
				throw std::runtime_error("ThreadPool: commit to stopped thread pool");
			}
			m_tasks.emplace([task]() { (*task)(); });
		}

		// 唤醒一个空闲线程处理任务
		m_cv.notify_one();

		return result_future;
	}
}