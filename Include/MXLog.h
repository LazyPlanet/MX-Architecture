#pragma once

#include <string>
#include <memory>
#include <functional>
#include <algorithm>
#include <string>
#include <stdarg.h>

#include "spdlog/spdlog.h"

#include "Config.h"
#include "CommonUtil.h"
#include "P_Header.h"

namespace Adoter
{
//////////////////////////////////////////////////////////////////////////
/////各种日志的宏定义/////
//////////////////////////////////////////////////////////////////////////

#define DEBUG(fmt, ...) { \
	auto debug = ConfigInstance.GetBool("DebugModel", true); \
	if (debug) { \
		spdlog::get("console")->debug("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
	}\
}\

#define TRACE(fmt, ...) { \
		spdlog::get("console")->trace("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

#define ERROR(fmt, ...) { \
		spdlog::get("console")->error("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

#define WARN(fmt, ...) { \
		spdlog::get("console")->warn("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

#define CRITICAL(fmt, ...) { \
		spdlog::get("console")->critical("[file:#{} func:#{} line:#{}] " fmt, __FILE__, __func__, __LINE__, ##__VA_ARGS__); \
}\

//玩家数据
#define PLAYER(message) { \
		std::string json; \
		pbjson::pb2json(&message, json); \
		spdlog::get("player")->info(json); \
}\

namespace pb = google::protobuf;

////////////////////////////////////////////////////////////////////////////////
// 日志管理系统
////////////////////////////////////////////////////////////////////////////////
class MXLog : public std::enable_shared_from_this<MXLog> 
{
	std::string _dir; //存储路径
	int64_t _server_id; //服务器ID
	std::string _server_name; //服务器名称

public:
	~MXLog() { 
		spdlog::drop_all(); 
	}

	MXLog()
	{
		////////////////////日志格式////////////////////
		spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%f] [logname:%n] [level:%l] [thread:%t] %v");

		////////////////////日志定义////////////////////
		//
		//控制台日志
		auto console = spdlog::stdout_color_mt("console");
		console->set_level(spdlog::level::trace);
		//玩家日志
		auto players = spdlog::basic_logger_mt("players", "logs/players");
		players->flush_on(spdlog::level::trace);
		//异步日志
		spdlog::set_async_mode(4096); //队列大小必须是2的整数倍
		auto player = spdlog::daily_logger_st("player", "logs/player");
		player->flush_on(spdlog::level::trace);
	}

	static MXLog& Instance()
	{
		static MXLog _instance;
		return _instance;
	}

	void Load()
	{
		_dir = ConfigInstance.GetString("LogDirectory", "logs");

		if (!_dir.empty())
		{
			if ((_dir.at(_dir.length() - 1) != '/') && (_dir.at(_dir.length() - 1) != '\\'))
			{
				_dir.push_back('/');
			}
		}
	}
};

#define MXLogInstance MXLog::Instance()

//按级别日志
#define LOG(level, message) \
	message->set_level(Asset::level); \
	MXLog::Instance().Print(message); \

}
