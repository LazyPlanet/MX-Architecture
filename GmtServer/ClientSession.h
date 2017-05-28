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
	std::vector<std::shared_ptr<ClientSession>> _list; //定时清理断开的会话
	std::unordered_map<int64_t, std::shared_ptr<ClientSession>> _sessions; //根据玩家状态变化处理	
public:
	static ClientSessionManager& Instance()
	{
		static ClientSessionManager _instance;
		return _instance;
	}

	size_t GetCount();
	void Add(std::shared_ptr<ClientSession> session);
	bool StartNetwork(boost::asio::io_service& io_service, const std::string& bind_ip, int32_t port, int thread_count = 1) override;
protected:        
	NetworkThread<ClientSession>* CreateThreads() const override;
private:        
	static void OnSocketAccept(tcp::socket&& socket, int32_t thread_index);
};

#define ClientSessionInstance ClientSessionManager::Instance()

}
