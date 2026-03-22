
// Buffer.h
// TCP连接的私有读写缓冲区，支持自动扩展、拆包粘包处理，适配非阻塞IO
#pragma once

#include "global.h"

#ifdef _WIN32
#include <winsock2.h>
#undef ERROR
typedef ptrdiff_t ssize_t; // 只有Windows需要定义ssize_t
using SocketType = SOCKET;

#else
#include <sys/uio.h>
#include <sys/socket.h>
using SocketType = int;
#define INVALID_SOCKET_VALUE (-1)
#endif

namespace TcFrame
{
	// TCP连接私有读写缓冲区，支持自动扩展、前置预留，适配非阻塞IO拆包粘包
	class Buffer
	{
	public:
		// 初始预留1024字节，后续根据需要自动扩展
		static const size_t kInitialSize = 1024;
		explicit Buffer(size_t initial_size = kInitialSize);

		// 禁止拷贝，缓冲区不应该被拷贝
		Buffer(const Buffer&) = delete;
		Buffer& operator=(const Buffer&) = delete;
		// 支持移动，方便放入容器
		Buffer(Buffer&&) = default;
		Buffer& operator=(Buffer&&) = default;

		// 基础信息查询
		size_t ReadableBytes() const; // 可读数据长度
		size_t WritableBytes() const; // 可写空间长度
		size_t PrependableBytes() const; // 前置可预留空间长度

		// 拿到可读数据的起始地址，const+非const重载
		char* Peek();
		const char* Peek() const;

		// 检索换行符 \r\n，适配HTTP等协议
		const char* FindCRLF() const;
		const char* FindCRLF(const char* start) const;

		// 检索单个 \n，适配行式协议
		const char* FindEOL() const;
		const char* FindEOL(const char* start) const;

		// 读完移动读指针，标记数据已经处理完
		void Retrieve(size_t len); // 读取len字节
		void RetrieveUntil(const char* end); // 读取到某个位置
		void RetrieveAll(); // 读取所有数据，清空缓冲区

		// 转成string读取，自动移动读指针
		std::string RetrieveAsString(size_t len);
		std::string RetrieveAllAsString();

		// 追加数据，自动扩容，移动写指针
		void Append(const char* data, size_t len);
		void Append(const std::string& str);
		void Append(const void* data, size_t len);

		// 从socket读取数据到缓冲区，分散读优化，返回读到的字节数
		ssize_t ReadFromFd(SocketType fd, int* savedErrno);
		// 把缓冲区可读数据写入socket，写成功自动移动读指针
		ssize_t WriteToFd(SocketType fd, int* savedErrno);

		// 保证有len字节可写空间，不够就扩容
		void EnsureWritableBytes(size_t len);
		// 标记已经写入了len字节，移动写指针
		void HasWritten(size_t len);

		// 往已读完的前置空间添加数据，用于前置加长度头等场景
		void Prepend(const void* data, size_t len);

		// 获取底层缓冲区引用
		const std::vector<char>& GetBuffer() const;

		// 读取4字节int（网络字节序），不移动读指针
		int32_t PeekInt32() const;
		// 追加4字节int，自动转成网络字节序，移动写指针
		void AppendInt32(int32_t x);

		// 获取可读区域起始指针，和Peek等价，命名更语义化
		char* ReadBegin();
		const char* ReadBegin() const;

	private:
		// 空间不足时分配空间，优先复用空闲空间，不够再扩容
		void MakeSpace(size_t len);

	private:
		std::vector<char> m_buf; // 底层连续存储缓冲区
		size_t m_read_idx;  // 读指针：下一个待读字节的位置
		size_t m_write_idx; // 写指针：下一个待写字节的位置
	};
}