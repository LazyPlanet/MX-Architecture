#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

#include <boost/asio.hpp>

#include "Socket.h"
#include "P_Header.h"

/*
 *
 * 负责将运维或者运营发送的WEB指令，发给指定的玩家或者广播出去.
 *
 * 1.查询数据库，检查玩家登陆和登出时间判断玩家是否在线.
 *
 * 2.如果玩家不在线，则直接修改数据库中内容.
 * 
 * 3.如果玩家在线，则广播，接收服务器负责修改内容，同时存储.
 *
 * 不考虑，检查时玩家在线，但是发送数据时，玩家不在线这种情况.
 *
 * */

namespace Adoter
{

using namespace google::protobuf;
namespace pb = google::protobuf;

class ClientSession : public Socket<ClientSession>
{
public:
	typedef std::function<int32_t(Message*)> CallBack;
public:
	explicit ClientSession(boost::asio::ip::tcp::socket&& socket);
	ClientSession(ClientSession const& right) = delete;    
	ClientSession& operator=(ClientSession const& right) = delete;
	
	virtual void Start() override;
	virtual bool Update() override; 
	virtual void OnClose() override;

	void InitializeHandler(const boost::system::error_code error, const std::size_t bytes_transferred);

	void SendProtocol(pb::Message& message);
	void SendProtocol(pb::Message* message);

	const std::string GetRemoteAddress() { return _remote_endpoint.address().to_string(); }
	const boost::asio::ip::tcp::endpoint GetRemotePoint() { return _remote_endpoint; }
	bool InnerCommand(const Asset::InnerMeta& command); //内部协议处理

private:
	boost::asio::ip::tcp::endpoint _remote_endpoint;
	std::string _ip_address;

	std::string _account; //账号信息
	int64_t _plyaer_id = 0; //角色信息
};

class ClientSessionManager : public SocketManager<ClientSession> 
{
	typedef SocketManager<ClientSession> SuperSocketManager;
private:
	std::mutex _mutex;
	std::vector<std::shared_ptr<ClientSession>> _sessions; //定时清理断开的会话

	std::shared_ptr<ClientSession> _gmt_session = nullptr;
public:
	static ClientSessionManager& Instance()
	{
		static ClientSessionManager _instance;
		return _instance;
	}

	void Add(std::shared_ptr<ClientSession> session);
	bool StartNetwork(boost::asio::io_service& io_service, const std::string& bind_ip, int32_t port, int thread_count = 1) override;

	void SetGmtServer(std::shared_ptr<ClientSession> session) { _gmt_session = session; }
	std::shared_ptr<ClientSession> GetGmtServer() { return _gmt_session; }
	bool IsGmtServer(std::shared_ptr<ClientSession> sesssion) {
		if (!sesssion || !_gmt_session) return false;
		return sesssion->GetRemotePoint() == _gmt_session->GetRemotePoint();
	}
protected:        
	NetworkThread<ClientSession>* CreateThreads() const override;
private:        
	static void OnSocketAccept(tcp::socket&& socket, int32_t thread_index);
};

#define ClientSessionInstance ClientSessionManager::Instance()

}
