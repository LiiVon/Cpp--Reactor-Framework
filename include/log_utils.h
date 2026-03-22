
// log_utils.h
// 日志工具函数，日志级别转换、时间格式化等
#pragma once
#include "logger.h"
#include <iostream>
namespace TcFrame
{
	class Log_utils
	{
	public:
		// 日志级别转字符串
		static std::string LevelToString(LogLevel level);

		// 获取当前日期字符串，用来切割日志
		static std::string GetCurrentDate();

		// 获取当前格式化时间（yyyy-MM-dd HH:mm:ss），给LogEvent填时间用
		static std::string GetCurrentTime();
	};
}