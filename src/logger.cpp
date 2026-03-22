#include "logger.h"
// logger.cpp
// 日志系统实现，支持多级别、多输出器、异步写入
#include "log_utils.h"
#include "global.h"

namespace TcFrame
{
	// -----控制台日志输出器实现-----
	void ConsoleAppender::Append(const LogEvent& event)
	{
		std::string log_str =
			"[" + event.time_str + "] "
			"[" + Log_utils::LevelToString(event.level) + "] "
			+ event.content + "\n";
		std::cout << log_str;
	}

	void ConsoleAppender::Flush()
	{
		std::cout.flush();
	}

	// -----文件日志输出器实现-----
	FileAppender::FileAppender(const std::string& base_path)
		: m_base_path(base_path)
	{
		m_current_date = Log_utils::GetCurrentDate();
		OpenNewLogFile();
	}

	FileAppender::~FileAppender()
	{
		Flush();
		if (m_ofs.is_open())
		{
			m_ofs.close();
		}
	}

	void FileAppender::OpenNewLogFile()
	{
		// 外层已经加锁，这里不需要再加
		std::string full_path = m_base_path + "." + m_current_date + ".log";
		m_ofs.open(full_path, std::ios::out | std::ios::app);
		if (!m_ofs.is_open())
		{
			std::cerr << "open log file failed: " << full_path << std::endl;
		}
	}

	void FileAppender::CheckRollOver()
	{
		std::string today = Log_utils::GetCurrentDate();
		if (today != m_current_date)
		{
			m_ofs.close();
			m_current_date = today;
			OpenNewLogFile();
		}
	}

	void FileAppender::Append(const LogEvent& event)
	{
		std::lock_guard<std::mutex> lock(m_files_mutex);
		CheckRollOver();
		std::string log_str =
			"[" + event.time_str + "] "
			"[" + Log_utils::LevelToString(event.level) + "] "
			+ event.content + "\n";
		if (m_ofs.is_open())
		{
			m_ofs << log_str;
		}
	}

	void FileAppender::Flush()
	{
		std::lock_guard<std::mutex> lock(m_files_mutex);
		if (m_ofs.is_open())
		{
			m_ofs.flush();
		}
	}

	// -----错误日志输出器实现-----
	ErrorFileAppender::ErrorFileAppender(const std::string& base_path)
		: FileAppender(base_path)
	{
	}

	void ErrorFileAppender::Append(const LogEvent& event)
	{
		if (event.level >= LogLevel::ERROR)
		{
			FileAppender::Append(event);
		}
	}

	// -----日志管理器实现-----
	Logger::Logger()
		: m_min_level(LogLevel::DEBUG), m_is_running(false)
	{
	}

	Logger::~Logger()
	{
		Shutdown();
	}

	Logger& Logger::Instance()
	{
		static Logger instance;
		return instance;
	}

	void Logger::LogLoop()
	{
		while (true)
		{
			std::unique_lock<std::mutex> lock(m_queue_mtx);
			m_cv.wait(lock, [this]() { return !m_log_queue.empty() || !m_is_running.load(); });

			if (!m_is_running.load() && m_log_queue.empty())
			{
				break;
			}

			while (!m_log_queue.empty())
			{
				LogEvent event = std::move(m_log_queue.front());
				m_log_queue.pop();
				lock.unlock();

				for (auto& appender : m_appenders)
				{
					appender->Append(event);
				}
				lock.lock();
			}

			for (auto& appender : m_appenders)
			{
				appender->Flush();
			}
		}
	}

	void Logger::BuildLogMeta(LogEvent& event)
	{
		auto now = std::chrono::system_clock::now();
		event.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
		std::time_t time_t_now = std::chrono::system_clock::to_time_t(now);
		std::tm tm;

#ifdef _WIN32

		localtime_s(&tm, &time_t_now);
#else

		localtime_r(&time_t_now, &tm);
#endif

		std::ostringstream oss;
		oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
			<< "." << std::setw(3) << std::setfill('0') << (event.timestamp % 1000);
		event.time_str = oss.str();
	}

	void Logger::Init(LogLevel min_level, const std::string& log_base_path)
	{
		m_min_level = min_level;
		m_is_running = true;
		// 默认添加三个输出器
		AddAppender(std::make_shared<ConsoleAppender>());
		AddAppender(std::make_shared<FileAppender>(log_base_path + "/tcframe"));
		AddAppender(std::make_shared<ErrorFileAppender>(log_base_path + "/tcframe.error"));
		// 启动异步日志线程
		m_log_thread = std::thread(&Logger::LogLoop, this);
	}

	void Logger::Shutdown()
	{
		m_is_running = false;
		m_cv.notify_all();
		if (m_log_thread.joinable())
		{
			m_log_thread.join();
		}

		for (auto& appender : m_appenders)
		{
			appender->Flush();
		}

		std::lock_guard<std::mutex> lock(m_queue_mtx);
		while (!m_log_queue.empty())
		{
			m_log_queue.pop();
		}
		m_appenders.clear();
	}

	void Logger::SetMinLevel(LogLevel level)
	{
		m_min_level = level;
	}

	LogLevel Logger::GetMinLevel() const
	{
		return m_min_level;
	}

	void Logger::AddAppender(std::shared_ptr<LogAppender> appender)
	{
		m_appenders.push_back(appender);
	}

	void Logger::Debug(const std::string& content)
	{
		if (!m_is_running.load()) return;
		if (LogLevel::DEBUG >= m_min_level)
		{
			LogEvent event{ LogLevel::DEBUG, content };
			BuildLogMeta(event);
			{
				std::lock_guard<std::mutex> lock(m_queue_mtx);
				m_log_queue.push(event);
			}
			m_cv.notify_one();
		}
	}

	void Logger::Info(const std::string& content)
	{
		if (!m_is_running.load()) return;
		if (LogLevel::INFO >= m_min_level)
		{
			LogEvent event{ LogLevel::INFO, content };
			BuildLogMeta(event);
			{
				std::lock_guard<std::mutex> lock(m_queue_mtx);
				m_log_queue.push(event);
			}
			m_cv.notify_one();
		}
	}

	void Logger::Warn(const std::string& content)
	{
		if (!m_is_running.load()) return;
		if (LogLevel::WARN >= m_min_level)
		{
			LogEvent event{ LogLevel::WARN, content };
			BuildLogMeta(event);
			{
				std::lock_guard<std::mutex> lock(m_queue_mtx);
				m_log_queue.push(event);
			}
			m_cv.notify_one();
		}
	}

	void Logger::Error(const std::string& content)
	{
		if (!m_is_running.load()) return;
		if (LogLevel::ERROR >= m_min_level)
		{
			LogEvent event{ LogLevel::ERROR, content };
			BuildLogMeta(event);
			{
				std::lock_guard<std::mutex> lock(m_queue_mtx);
				m_log_queue.push(event);
			}
			m_cv.notify_one();
		}
	}

	void Logger::Fatal(const std::string& content)
	{
		if (!m_is_running.load()) return;
		if (LogLevel::FATAL >= m_min_level)
		{
			LogEvent event{ LogLevel::FATAL, content };
			BuildLogMeta(event);
			{
				std::lock_guard<std::mutex> lock(m_queue_mtx);
				m_log_queue.push(event);
			}
			m_cv.notify_one();
		}
	}

	void Logger::Debug(const char* fmt, ...)
	{
		if (!m_is_running.load()) return;
		if (LogLevel::DEBUG >= m_min_level)
		{
			char buf[4096]; // 栈上分配缓冲区，足够用
			va_list args;
			va_start(args, fmt);
			vsnprintf(buf, sizeof(buf), fmt, args); // 用vsnprintf格式化，安全不越界
			va_end(args);

			LogEvent event{ LogLevel::DEBUG, std::string(buf), "", 0 };
			BuildLogMeta(event);
			{
				std::lock_guard<std::mutex> lock(m_queue_mtx);
				m_log_queue.push(event);
			}
			m_cv.notify_one();
		}
	}

	void Logger::Info(const char* fmt, ...)
	{
		if (!m_is_running.load()) return;
		if (LogLevel::INFO >= m_min_level)
		{
			char buf[4096];
			va_list args;
			va_start(args, fmt);
			vsnprintf(buf, sizeof(buf), fmt, args);
			va_end(args);

			LogEvent event{ LogLevel::INFO, std::string(buf), "", 0 };
			BuildLogMeta(event);
			{
				std::lock_guard<std::mutex> lock(m_queue_mtx);
				m_log_queue.push(event);
			}
			m_cv.notify_one();
		}
	}

	void Logger::Warn(const char* fmt, ...)
	{
		if (!m_is_running.load()) return;
		if (LogLevel::WARN >= m_min_level)
		{
			char buf[4096];
			va_list args;
			va_start(args, fmt);
			vsnprintf(buf, sizeof(buf), fmt, args);
			va_end(args);

			LogEvent event{ LogLevel::WARN, std::string(buf), "", 0 };
			BuildLogMeta(event);
			{
				std::lock_guard<std::mutex> lock(m_queue_mtx);
				m_log_queue.push(event);
			}
			m_cv.notify_one();
		}
	}

	void Logger::Error(const char* fmt, ...)
	{
		if (!m_is_running.load()) return;
		if (LogLevel::ERROR >= m_min_level)
		{
			char buf[4096];
			va_list args;
			va_start(args, fmt);
			vsnprintf(buf, sizeof(buf), fmt, args);
			va_end(args);

			LogEvent event{ LogLevel::ERROR, std::string(buf), "", 0 };
			BuildLogMeta(event);
			{
				std::lock_guard<std::mutex> lock(m_queue_mtx);
				m_log_queue.push(event);
			}
			m_cv.notify_one();
		}
	}

	void Logger::Fatal(const char* fmt, ...)
	{
		if (!m_is_running.load()) return;
		if (LogLevel::FATAL >= m_min_level)
		{
			char buf[4096];
			va_list args;
			va_start(args, fmt);
			vsnprintf(buf, sizeof(buf), fmt, args);
			va_end(args);

			LogEvent event{ LogLevel::FATAL, std::string(buf), "", 0 };
			BuildLogMeta(event);
			{
				std::lock_guard<std::mutex> lock(m_queue_mtx);
				m_log_queue.push(event);
			}
			m_cv.notify_one();
		}
	}
}