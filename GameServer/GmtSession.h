#pragma once

#include <mutex>

#include "P_Header.h"

namespace Adoter
{

using namespace google::protobuf;
namespace pb = google::protobuf;

class GmtManager 
{
public:
	
	static GmtManager& Instance()
	{
		static GmtManager _instance;
		return _instance;
	}
	
	Asset::COMMAND_ERROR_CODE OnCommandProcess(const Asset::Command& command);
	Asset::COMMAND_ERROR_CODE OnSendMail(const Asset::SendMail& command);
	Asset::COMMAND_ERROR_CODE OnSystemBroadcast(const Asset::SystemBroadcast& command);
	Asset::COMMAND_ERROR_CODE OnActivityControl(const Asset::ActivityControl& command);
    
	bool OnInnerProcess(const Asset::InnerMeta& meta); //内部协议处理
	
	void SendProtocol(pb::Message& message);
	void SendProtocol(pb::Message* message);
	
	void SendInnerMeta(const Asset::InnerMeta& message);
private:
	std::mutex _gmt_lock;
	int64_t _session_id = 0; //操作会话
};

#define GmtInstance GmtManager::Instance()

}
