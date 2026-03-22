#include "log_utils.h"
// log_utils.cpp
// 日志工具函数实现，日志级别转换、时间格式化等
#include "global.h"

namespace TcFrame
{
	std::string Log_utils::LevelToString(LogLevel level)
	{
		switch (level)
		{
		case LogLevel::DEBUG:
			return "DEBUG";
		case LogLevel::INFO:
			return "INFO";
		case LogLevel::WARN:
			return "WARN";
		case LogLevel::ERROR:
			return "ERROR";
		case LogLevel::FATAL:
			return "FATAL";
		default:
			return "UNKNOWN";
		}
	}

	std::string Log_utils::GetCurrentDate()
	{
		auto now = std::chrono::system_clock::now();
		std::time_t t = std::chrono::system_clock::to_time_t(now);
		std::tm tm_now;

#ifdef _WIN32
		localtime_s(&tm_now, &t);
#else
		localtime_r(&t, &tm_now);
#endif

		std::ostringstream oss;
		oss << std::put_time(&tm_now, "%Y-%m-%d");
		return oss.str();
	}

	std::string Log_utils::GetCurrentTime()
	{
		auto now = std::chrono::system_clock::now();
		std::time_t t = std::chrono::system_clock::to_time_t(now);
		std::tm tm_now;

#ifdef _WIN32
		localtime_s(&tm_now, &t);
#else
		localtime_r(&t, &tm_now);
#endif

		std::ostringstream oss;
		oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S");
		return oss.str();
	}
}
