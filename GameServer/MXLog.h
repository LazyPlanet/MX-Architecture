#pragma once

#include <string>
#include <memory>
#include <functional>
#include <stdarg.h>

#include "spdlog/spdlog.h"

#include "Config.h"
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
#define PLAYER(message, ...) { \
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

	MXLog();
	~MXLog() { 
		spdlog::drop_all(); 
	}

	static MXLog& Instance()
	{
		static MXLog _instance;
		return _instance;
	}

	void Load(); //加载日志配置

    void InitColors(const std::string& init_str);

	void Print(Asset::LogMessage* message); //日志输出
	void ConsolePrint(Asset::LogMessage* message); //控制台输出
};

#define MXLogInstance MXLog::Instance()

//按级别日志
#define LOG(level, message) \
	message->set_level(Asset::level); \
	MXLog::Instance().Print(message); \

}
