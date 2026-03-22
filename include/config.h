
// Config.h
// 全局配置管理，支持yaml文件加载、热更新、变更回调，线程安全单例
#pragma once
#include "global.h"

namespace YAML
{
	class Node;
}

namespace TcFrame
{
	// 配置项key变更  调用回调函数
	using ConfigChangeCallback = std::function<void(const std::string& key)>;

	class Config
	{
	public:
		static Config& Instance();

		// 从指定路径加载yaml配置文件
		bool LoadFromFile(const std::string& file_path);

		// 热更新配置，重新加载文件并触发回调，不用重启服务
		bool Reload();

		// 配置项变更 调用回调
		void OnChange(const std::string& key, const ConfigChangeCallback& callback);

		// 根据给的配置key，从全局配置里读出对应类型的值
		// 如果key不存在或者类型不对，就返回你给的默认值
		template<typename T>
		T Get(const std::string& key, const T& default_value = T()) const
		{
			std::lock_guard<std::recursive_mutex> lock(m_mtx);
			if (m_configs.find(key) == m_configs.end())
			{
				return default_value;
			}
			try
			{
				return std::any_cast<T>(m_configs.at(key));
			}
			catch (const std::bad_any_cast& e)
			{
				std::cerr << "Config value for key '" << key << "' has wrong type: " << e.what() << std::endl;
				return default_value;
			}
		}

		std::string GetConfigFilePath() const { return m_config_file_path; }

	private:
		Config() = default;
		~Config() = default;
		Config(const Config&) = delete;
		Config& operator=(const Config&) = delete;

		// 递归解析yaml节点，扁平化为a.b.c格式key
		void ParseYamlNode(const std::string& prefix, const YAML::Node& node);

		// 对比新旧配置判断是否变更
		bool IsConfigChanged(const std::string& key, const std::unordered_map<std::string, std::any>& old_configs) const;

	private:
		mutable std::recursive_mutex m_mtx;    // 递归锁支持递归解析，支持const Get加锁
		std::unordered_map<std::string, std::any> m_configs; // 存储扁平化配置项
		std::unordered_map<std::string, std::vector<ConfigChangeCallback>> m_callbacks; // 存储变更回调
		std::string m_config_file_path; // 配置文件路径
	};
}