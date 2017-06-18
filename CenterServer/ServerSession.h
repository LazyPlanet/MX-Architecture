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
 * 中心服务器.
 *
 * */

namespace Adoter
{

using namespace google::protobuf;
namespace pb = google::protobuf;

class ServerSession : public Socket<ServerSession>
{
public:
	typedef std::function<int32_t(Message*)> CallBack;
public:
	explicit ServerSession(boost::asio::ip::tcp::socket&& socket);
	ServerSession(ServerSession const& right) = delete;    
	ServerSession& operator=(ServerSession const& right) = delete;
	
	virtual void Start() override;
	virtual bool Update() override; 
	virtual void OnClose() override;

	void InitializeHandler(const boost::system::error_code error, const std::size_t bytes_transferred);

	void SendProtocol(const pb::Message& message);
	void SendProtocol(const pb::Message* message);

	const std::string GetRemoteAddress() { return _remote_endpoint.address().to_string(); }
	const boost::asio::ip::tcp::endpoint GetRemotePoint() { return _remote_endpoint; }
	bool InnerProcess(const Asset::InnerMeta& meta); //内部协议处理

private:
	boost::asio::ip::tcp::endpoint _remote_endpoint;
	std::string _ip_address;
};

class ServerSessionManager : public SocketManager<ServerSession> 
{
	typedef SocketManager<ServerSession> SuperSocketManager;
private:
	std::mutex _mutex;
	std::unordered_map<int64_t/*server_id*/, std::shared_ptr<ServerSession>> _sessions; //定时清理断开的会话

	std::shared_ptr<ServerSession> _gmt_session = nullptr;
public:
	static ServerSessionManager& Instance()
	{
		static ServerSessionManager _instance;
		return _instance;
	}
	
	void BroadCastProtocol(const pb::Message& message);
	void BroadCastProtocol(const pb::Message* message);

	void Add(int64_t server_id, std::shared_ptr<ServerSession> session);
	std::shared_ptr<ServerSession> Get(int64_t server_id) { return _sessions[server_id]; }

	bool StartNetwork(boost::asio::io_service& io_service, const std::string& bind_ip, int32_t port, int thread_count = 1) override;

protected:        
	NetworkThread<ServerSession>* CreateThreads() const override;
private:        
	static void OnSocketAccept(tcp::socket&& socket, int32_t thread_index);
};

#define ServerSessionInstance ServerSessionManager::Instance()

}
