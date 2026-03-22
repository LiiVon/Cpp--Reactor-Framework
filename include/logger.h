
// logger.h
// 日志系统核心，支持多级别、多输出器、异步写入，单例管理
#pragma once
#include "global.h"

namespace TcFrame
{
	// 日志级别
	enum LogLevel

	{
		DEBUG = 0,
		INFO = 1,
		WARN = 2,
		ERROR = 3,
		FATAL = 4
	};
	// 日志事件结构体
	struct LogEvent
	{
		LogLevel level;
		std::string content;
		std::string time_str;
		int64_t timestamp;
	};

	// 日志输出器接口
	class LogAppender
	{
	public:
		virtual ~LogAppender() = default;
		virtual void Append(const LogEvent& event) = 0;
		virtual void Flush() = 0;
	};

	// 控制台日志输出器
	class ConsoleAppender : public LogAppender
	{
	public:
		ConsoleAppender() = default;
		~ConsoleAppender() override = default;
		void Append(const LogEvent& event) override;
		void Flush() override;
	};

	// 文件日志输出器
	class FileAppender : public LogAppender
	{
	public:
		explicit FileAppender(const std::string& base_path);
		~FileAppender() override;
		void Append(const LogEvent& event) override;
		void Flush() override;

	private:
		// 检查是否需要切割文件（日期发生变化）
		void CheckRollOver();

		// 打开新的日志文件
		void OpenNewLogFile();

	private:
		std::string m_base_path; // Base path for log files
		std::string m_current_date;
		std::ofstream m_ofs;
		std::mutex m_files_mutex;
	};

	// 错误日志输出器，专门用于输出ERROR级别的日志
	class ErrorFileAppender : public FileAppender
	{
	public:
		explicit ErrorFileAppender(const std::string& base_path);
		~ErrorFileAppender() override = default;
		void Append(const LogEvent& event) override;
	};

	// 日志配置接口：专门管配置，动态调整、添加输出器
	class LogConfig
	{
	public:
		virtual ~LogConfig() = default;
		virtual void SetMinLevel(LogLevel level) = 0;
		virtual LogLevel GetMinLevel() const = 0;
		virtual void AddAppender(std::shared_ptr<LogAppender> appender) = 0;
	};

	// 日志写入接口：专门管写日志，对外只暴露写方法
	class LogWriter
	{
	public:
		virtual ~LogWriter() = default;
		virtual void Debug(const std::string& content) = 0;
		virtual void Info(const std::string& content) = 0;
		virtual void Warn(const std::string& content) = 0;
		virtual void Error(const std::string& content) = 0;
		virtual void Fatal(const std::string& content) = 0;

		// printf风格重载声明
		virtual void Debug(const char* fmt, ...) = 0;
		virtual void Info(const char* fmt, ...) = 0;
		virtual void Warn(const char* fmt, ...) = 0;
		virtual void Error(const char* fmt, ...) = 0;
		virtual void Fatal(const char* fmt, ...) = 0;
	};

	// 日志管理器：单例，全局唯一
	class Logger : public LogConfig, public LogWriter
	{
	public:
		static Logger& Instance();

		// 初始化和停止，生命周期管理
		void Init(LogLevel min_level, const std::string& log_base_path);
		void Shutdown();

		// 实现LogConfig接口
		void SetMinLevel(LogLevel level) override;
		LogLevel GetMinLevel() const override;
		void AddAppender(std::shared_ptr<LogAppender> appender) override;

		// 实现LogWriter接口
		void Debug(const std::string& content) override;
		void Info(const std::string& content) override;
		void Warn(const std::string& content) override;
		void Error(const std::string& content) override;
		void Fatal(const std::string& content) override;

		// printf风格重载声明
		void Debug(const char* fmt, ...) override;
		void Info(const char* fmt, ...) override;
		void Warn(const char* fmt, ...) override;
		void Error(const char* fmt, ...) override;
		void Fatal(const char* fmt, ...) override;

	private:
		Logger();
		~Logger();
		Logger(const Logger&) = delete;
		Logger& operator=(const Logger&) = delete;

		// 异步日志 后台工作循环
		void LogLoop();

		// 构建日志事件的元信息（时间戳、格式化时间等）
		void BuildLogMeta(LogEvent& event);

	private:
		LogLevel m_min_level;                          // 最低输出级别
		std::vector<std::shared_ptr<LogAppender>> m_appenders; // 多个输出器
		std::queue<LogEvent> m_log_queue;             // 异步日志队列
		std::mutex m_queue_mtx;                        // 队列锁
		std::condition_variable m_cv;                  // 条件变量
		std::thread m_log_thread;                      // 异步写线程
		std::atomic<bool> m_is_running;                // 原子运行标志
	};
}

// 定义常用的日志宏，简化日志调用

#define LOG_DEBUG(...) Logger::Instance().Debug(__VA_ARGS__)

#define LOG_INFO(...)  Logger::Instance().Info(__VA_ARGS__)

#define LOG_WARN(...)  Logger::Instance().Warn(__VA_ARGS__)

#define LOG_ERROR(...) Logger::Instance().Error(__VA_ARGS__)

#define LOG_FATAL(...) Logger::Instance().Fatal(__VA_ARGS__)