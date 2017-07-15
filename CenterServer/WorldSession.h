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

class Player;

class WorldSession : public Socket<WorldSession>
{
	typedef Socket<WorldSession> SuperSocket;
public:
	typedef std::function<int32_t(Message*)> CallBack;
public:
	WorldSession(boost::asio::ip::tcp::socket&& socket);
	WorldSession(WorldSession const& right) = delete;    
	WorldSession& operator = (WorldSession const& right) = delete;
	
	virtual void Start() override;
	virtual bool Update() override; 
	virtual void OnClose() override;

	void InitializeHandler(const boost::system::error_code error, const std::size_t bytes_transferred);

	void SendMeta(const Asset::Meta& meta);
	void SendProtocol(const pb::Message& message);
	void SendProtocol(const pb::Message* message);

	void KickOutPlayer(Asset::KICK_OUT_REASON reason);
	void OnLogout();

	boost::asio::ip::tcp::endpoint GetRemotePoint() { return _remote_endpoint; }
	std::string GetRemoteAddress() {return _remote_endpoint.address().to_string(); }

	bool OnInnerProcess(const Asset::Meta& meta);
	void OnProcessMessage(const Asset::Meta& meta);

	int64_t GetID() { return _global_id; }
	void SetID(int64_t global_id) { _global_id = global_id; }

	void SetRoleType(Asset::ROLE_TYPE role_type, int64_t global_id) { 
		_role_type = role_type; 
		_global_id = global_id;
	}

	int32_t OnWechatLogin(const pb::Message* message);
	const Asset::WechatUnion& GetWechat() { return _wechat; }
	bool IsWechat() { return _account.account_type() == Asset::ACCOUNT_TYPE_WECHAT; }
private:
	Asset::WechatUnion _wechat; //微信数据
	Asset::WechatAccessToken _access_token;
	
	Asset::User _user; 
	Asset::Account _account;

	std::unordered_set<int64_t> _player_list;

	std::string _ip_address;
	boost::asio::ip::tcp::endpoint _remote_endpoint;

	int64_t _global_id = 0; //逻辑服务器ID或者玩家ID
	Asset::ROLE_TYPE _role_type; //会话类型
	std::shared_ptr<Player> _player = nullptr; //全局玩家定义，唯一的一个Player对象
	bool _online = true;
};

class WorldSessionManager : public SocketManager<WorldSession> 
{
	typedef SocketManager<WorldSession> SuperSocketManager;
private:
	std::mutex _mutex;
	//
	//理论上服务器ID和玩家ID不会重复
	//
	//防止未来设计突破限制，分开处理
	//
	std::unordered_map<int64_t/*玩家角色ID*/, std::shared_ptr<WorldSession>> _client_list; //玩家<->中心服务器会话	
	std::unordered_map<int64_t/*服务器ID*/, std::shared_ptr<WorldSession>> _server_list; //中心服务器<->逻辑服务器会话	
	std::unordered_map<int64_t/*玩家角色ID*/, std::shared_ptr<WorldSession>> _player_gs; //玩家<->游戏逻辑服务器会话	
public:
	static WorldSessionManager& Instance()
	{
		static WorldSessionManager _instance;
		return _instance;
	}

	void AddPlayer(int64_t player_id, std::shared_ptr<WorldSession> session) { _client_list.emplace(player_id, session); }
	void RemovePlayer(int64_t player_id) { _client_list.erase(player_id); }
	std::shared_ptr<WorldSession> GetPlayerSession(int64_t player_id) { return _client_list[player_id]; }

	void AddServer(int64_t server_id, std::shared_ptr<WorldSession> session) {	_server_list.emplace(server_id, session);}	
	std::shared_ptr<WorldSession> GetServerSession(int64_t server_id) { return _server_list[server_id]; }
	void SetPlayerSession(int64_t player_id, std::shared_ptr<WorldSession> session) { _player_gs[player_id] = session; }
	std::shared_ptr<WorldSession> RandomServer(); //随机选择游戏逻辑服务器
	
	void BroadCast2GameServer(const pb::Message& message); //游戏逻辑服务器
	void BroadCast2GameServer(const pb::Message* message); 
	
	void BroadCast(const pb::Message& message); //玩家
	void BroadCast(const pb::Message* message); 
	
	bool StartNetwork(boost::asio::io_service& io_service, const std::string& bind_ip, int32_t port, int thread_count = 1) override;
protected:        
	NetworkThread<WorldSession>* CreateThreads() const override;
private:        
	static void OnSocketAccept(tcp::socket&& socket, int32_t thread_index);
};

#define WorldSessionInstance WorldSessionManager::Instance()

}
