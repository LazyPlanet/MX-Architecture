#pragma once

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
};

#define GmtInstance GmtManager::Instance()

}
