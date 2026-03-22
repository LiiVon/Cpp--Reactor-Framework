
// Channel.h
// Reactor模式事件通道，绑定fd，描述关心的事件并分发回调，供EventLoop/Poller使用
#pragma once
#include "global.h"

namespace TcFrame
{
	class EventLoop;

	/*
	@brief：Reactor模式的事件通道，每个Channel绑定一个socket，描述关心的事件，事件来了分发回调
	*/
	class Channel
	{
	public:
		// 事件类型，复用系统poll/epoll的常量，和系统事件直接对应，不用转换
		static const int kNoneEvent;    // 不关心任何事件
		static const int kReadEvent;    // 关心读事件，对应POLLIN
		static const int kWriteEvent;   // 关心写事件，对应POLLOUT

		// 每个Channel唯一绑定一个fd和所属EventLoop，explicit禁止隐式转换
		explicit Channel(EventLoop* loop, SocketType fd);
		~Channel();

		Channel(const Channel&) = delete;
		Channel& operator=(const Channel&) = delete;

		// 设置各类事件的回调函数
		void SetReadCallback(std::function<void()> cb);
		void SetWriteCallback(std::function<void()> cb);
		void SetErrorCallback(std::function<void()> cb);
		void SetCloseCallback(std::function<void()> cb);

		// 事件操作：开启/关闭对应事件，自动更新到EventLoop
		void EnableReading();	// 开启读事件监听
		void DisableReading();	// 关闭读事件监听
		void EnableWriting();	// 开启写事件监听
		void DisableWriting();	// 关闭写事件监听
		void DisableAll();      // 关闭所有事件监听

		// 给Poller用的接口
		int GetEvents() const;		// 获取当前关心的所有事件（位掩码）
		void SetRevents(int revents);	// 设置Poller检测到的就绪事件
		void HandleEvent();		// 处理就绪事件，由EventLoop调用，分发对应回调

		// 状态查询
		bool IsWriting() const;  // 是否正在监听写事件
		bool IsReading() const;  // 是否正在监听读事件
		bool HasNoEvents() const; // 是否不监听任何事件

		SocketType GetFd() const;          // 获取绑定的socket fd
		EventLoop* GetOwnerLoop() const;   // 获取所属的EventLoop

		// 标记是否已经添加到Poller
		void SetAdded(bool added);
		bool IsAdded() const;

	private:
		EventLoop* m_ownerLoop;	// 所属的EventLoop，每个Channel只属于一个Loop
		const SocketType m_fd;	// 绑定的原生socket fd，不可变
		int m_events;			// 当前用户关心的事件（位掩码）
		int m_revents;			// Poller检测到的实际发生的就绪事件
		bool m_added;           // 是否已经添加到Poller中

		// 各类事件对应的回调
		std::function<void()> m_read_callback;
		std::function<void()> m_write_callback;
		std::function<void()> m_error_callback;
		std::function<void()> m_close_callback;
	};
}