#include "buffer.h"
// Buffer.cpp
// TCP连接的私有读写缓冲区实现，支持自动扩展、拆包粘包处理
#include "global.h"
#include "logger.h"
#include "socket_utils.h"

namespace TcFrame
{
	Buffer::Buffer(size_t initial_size)
		: m_buf(initial_size)
		, m_read_idx(0)
		, m_write_idx(0)
	{
	}

	size_t Buffer::ReadableBytes() const
	{
		return m_write_idx - m_read_idx;
	}

	size_t Buffer::WritableBytes() const
	{
		return m_buf.size() - m_write_idx;
	}

	size_t Buffer::PrependableBytes() const
	{
		return m_read_idx;
	}

	char* Buffer::Peek()
	{
		return &m_buf[m_read_idx];
	}

	const char* Buffer::Peek() const
	{
		return &m_buf[m_read_idx];
	}

	void Buffer::Retrieve(size_t len)
	{
		assert(len <= ReadableBytes());
		m_read_idx += len;
	}

	void Buffer::RetrieveUntil(const char* end)
	{
		assert(Peek() <= end);
		Retrieve(end - Peek());
	}

	void Buffer::RetrieveAll()
	{
		m_read_idx = 0;
		m_write_idx = 0;
	}

	std::string Buffer::RetrieveAsString(size_t len)
	{
		assert(len <= ReadableBytes());
		std::string result(Peek(), len);
		Retrieve(len);
		return result;
	}

	std::string Buffer::RetrieveAllAsString()
	{
		return RetrieveAsString(ReadableBytes());
	}

	void Buffer::EnsureWritableBytes(size_t len)
	{
		if (WritableBytes() >= len)
		{
			return;
		}
		MakeSpace(len);
		assert(WritableBytes() >= len);
	}

	void Buffer::HasWritten(size_t len)
	{
		assert(len <= WritableBytes());
		m_write_idx += len;
	}

	void Buffer::MakeSpace(size_t len)
	{
		// 如果前置空闲 + 后置可写足够，就把未读数据挪到开头，复用空间，不用重新分配
		if (PrependableBytes() + WritableBytes() >= len)
		{
			size_t readable = ReadableBytes();
			std::copy(m_buf.begin() + m_read_idx, m_buf.begin() + m_write_idx, m_buf.begin());
			m_read_idx = 0;
			m_write_idx = readable;
			assert(readable == ReadableBytes());
		}
		else
		{
			// 空间不够，直接扩容
			m_buf.resize(m_write_idx + len);
		}
	}

	void Buffer::Append(const char* data, size_t len)
	{
		EnsureWritableBytes(len);
		std::copy(data, data + len, m_buf.begin() + m_write_idx);
		m_write_idx += len;
	}

	void Buffer::Append(const std::string& str)
	{
		Append(str.data(), str.size());
	}

	void Buffer::Append(const void* data, size_t len)
	{
		Append(static_cast<const char*>(data), len);
	}

	void Buffer::Prepend(const void* data, size_t len)
	{
		assert(len <= PrependableBytes());
		m_read_idx -= len;
		const char* p = static_cast<const char*>(data);
		std::copy(p, p + len, &m_buf[m_read_idx]);
	}

	const char* Buffer::FindCRLF() const
	{
		return FindCRLF(Peek());
	}

	const char* Buffer::FindCRLF(const char* start) const
	{
		assert(start >= Peek());
		const char* end = Peek() + ReadableBytes();
		const char* p = start;
		// 用memchr找'\r'，比手动遍历更快
		while ((p = static_cast<const char*>(memchr(p, '\r', end - p))) != nullptr) {
			if (p + 1 < end && *(p + 1) == '\n') {
				return p;
			}
			p++;
		}
		return nullptr;
	}

	const char* Buffer::FindEOL() const
	{
		return FindEOL(Peek());
	}

	const char* Buffer::FindEOL(const char* start) const
	{
		assert(start >= Peek());
		const char* p = static_cast<const char*>(memchr(start, '\n', Peek() + ReadableBytes() - start));
		return p;
	}

	ssize_t Buffer::ReadFromFd(SocketType fd, int* saved_errno)
	{
		char extrabuf[65536]; // 64K栈上缓冲区，一次读完socket内核缓冲区，减少系统调用
		const size_t writable = WritableBytes();

#ifdef _WIN32

		// Windows用WSABUF做分散读
		WSABUF vec[2];
		vec[0].buf = reinterpret_cast<char*>(&m_buf[m_write_idx]);
		vec[0].len = static_cast<ULONG>(writable);
		vec[1].buf = extrabuf;
		vec[1].len = static_cast<ULONG>(sizeof(extrabuf));
		// 可写空间够就只用第一个缓冲区
		const DWORD iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;

		DWORD bytes_read;
		DWORD flags = 0;
		int ret = WSARecv(fd, vec, iovcnt, &bytes_read, &flags, nullptr, nullptr);
		if (ret != 0)
		{
			*saved_errno = SocketUtils::GetLastError();
			return -1;
		}
		if (bytes_read == 0)
		{
			// 对端关闭连接
			return 0;
		}
		ssize_t n = static_cast<ssize_t>(bytes_read);
#else

		// Linux用iovec分散读
		iovec vec[2];
		vec[0].iov_base = &m_buf[m_write_idx];
		vec[0].iov_len = writable;
		vec[1].iov_base = extrabuf;
		vec[1].iov_len = sizeof(extrabuf);
		const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;

		ssize_t n = readv(fd, vec, iovcnt);
		if (n < 0)
		{
			*saved_errno = errno;
		}
#endif

		if (static_cast<size_t>(n) <= writable)
		{
			// 全部读完，直接移动写指针
			m_write_idx += static_cast<size_t>(n);
		}
		else
		{
			// 可写空间不够，剩下的数据在extrabuf，追加进去
			m_write_idx = m_buf.size();
			Append(extrabuf, static_cast<size_t>(n - writable));
		}
		return n;
	}

	ssize_t Buffer::WriteToFd(SocketType fd, int* saved_errno)
	{
		size_t readable = ReadableBytes();
		if (readable == 0)
		{
			return 0;
		}

#ifdef _WIN32

		// Windows用WSABUF写
		WSABUF vec;
		vec.buf = Peek();
		vec.len = static_cast<ULONG>(readable);
		DWORD bytes_written;
		DWORD flags = 0;
		int ret = WSASend(fd, &vec, 1, &bytes_written, flags, nullptr, nullptr);
		if (ret != 0)
		{
			*saved_errno = SocketUtils::GetLastError();
			return -1;
		}
		ssize_t n = static_cast<ssize_t>(bytes_written);
#else

		// Linux直接write
		ssize_t n = write(fd, Peek(), readable);
		if (n < 0)
		{
			*saved_errno = errno;
		}
#endif

		if (n > 0)
		{
			Retrieve(static_cast<size_t>(n));
		}
		return n;
	}

	const std::vector<char>& Buffer::GetBuffer() const
	{
		return m_buf;
	}

	int32_t Buffer::PeekInt32() const
	{
		uint32_t net_result;
		memcpy(&net_result, Peek(), sizeof(uint32_t));
		uint32_t host_result = SocketUtils::NetToHostLong(net_result);
		return static_cast<int32_t>(host_result);
	}

	void Buffer::AppendInt32(int32_t x)
	{
		uint32_t host_x = static_cast<uint32_t>(x);
		uint32_t net_x = SocketUtils::HostToNetLong(host_x);
		Append(&net_x, sizeof(net_x));
	}

	char* Buffer::ReadBegin()
	{
		return Peek();
	}

	const char* Buffer::ReadBegin() const
	{
		return Peek();
	}
}