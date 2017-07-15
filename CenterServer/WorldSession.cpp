#include <spdlog/spdlog.h>
#include <CkHttp.h>

#include "WorldSession.h"
#include "RedisManager.h"
#include "CommonUtil.h"
#include "MXLog.h"
#include "Protocol.h"
#include "Player.h"
#include "PlayerName.h"
#include "Timer.h"

namespace Adoter
{

namespace spd = spdlog;

WorldSession::WorldSession(boost::asio::ip::tcp::socket&& socket) : Socket(std::move(socket))
{
	_remote_endpoint = _socket.remote_endpoint();
	_ip_address = _remote_endpoint.address().to_string();
}

void WorldSession::InitializeHandler(const boost::system::error_code error, const std::size_t bytes_transferred)
{
	try
	{
		if (error)
		{
			ERROR("Remote client disconnect, remote_ip:{} port:{}, player_id:{}", _ip_address, _remote_endpoint.port(), _player ? _player->GetID() : 0);
			KickOutPlayer(Asset::KICK_OUT_REASON_DISCONNECT);
			return;
		}
		else
		{
			for (size_t index = 0; index < bytes_transferred;)
			{
				unsigned short body_size = _buffer[index] * 256 + _buffer[1 + index];
					
				if (body_size > 4096)
				{
					LOG(ERROR, "接收来自地址:{} 端口:{} 太大的包长:{} 丢弃.", _ip_address, _remote_endpoint.port(), body_size)
					return;
				}

				char buffer[4096] = {0}; //数据缓存  
				for (size_t i = 0; i < body_size; ++i) buffer[i] = _buffer[i + index + 2];  

				Asset::Meta meta;
				bool result = meta.ParseFromArray(buffer, body_size);

				if (!result) return; //非法协议

				OnProcessMessage(meta);

				index += (body_size + 2); //下个包的起始位置
			}
		}
	}
	catch (std::exception& e)
	{
		ERROR("Remote client disconnect, remote_ip:{}, error:{}, player_id:{}", _ip_address, e.what(), _player ? _player->GetID() : 0);
		KickOutPlayer(Asset::KICK_OUT_REASON_DISCONNECT);
		return;
	}

	//WARN("中心服务器接收数据之后, 当前缓存数量大小:{} 本次接收数据大小:{}", _buffer.size(), bytes_transferred);
	AsyncReceiveWithCallback(&WorldSession::InitializeHandler); //递归持续接收	
}
			
void WorldSession::OnProcessMessage(const Asset::Meta& meta)
{
	pb::Message* msg = ProtocolInstance.GetMessage(meta.type_t());	
	if (!msg) 
	{
		ERROR("Could not found message of type:{}", meta.type_t());
		return;		//非法协议
	}

	auto message = msg->New();
	
	auto result = message->ParseFromArray(meta.stuff().c_str(), meta.stuff().size());
	if (!result) 
	{
		DEBUG_ASSERT(false);
		return;		//非法协议
	}

	//调试
	const pb::FieldDescriptor* field = message->GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;

	const pb::EnumValueDescriptor* enum_value = message->GetReflection()->GetEnum(*message, field);
	if (!enum_value) return;


	WARN("中心服务器接收数据, 玩家:{} 协议:{} {} 内容:{}", meta.player_id(), meta.type_t(), enum_value->name(), message->ShortDebugString());

	//
	// C2S协议可能存在两种情况：
	//
	// 1.Client发送上来的数据;
	//
	// 2.转发到游戏逻辑服务器，经过逻辑服务器处理的结果返回;
	//
	//
	
	if (Asset::META_TYPE_C2S_COUNT <= meta.type_t()) 
	{
		//
		//游戏服务器内部协议处理
		//
		OnInnerProcess(meta); //内部处理

		DEBUG("1.中心服务器接收游戏服务器内部协议:{}", meta.ShortDebugString());
	}
	else if (meta.player_id() > 0)
	{
		DEBUG("2.中心服务器接收游戏服务器数据, 玩家:{} 协议:{}", meta.player_id(), meta.ShortDebugString());

		auto player = PlayerInstance.Get(meta.player_id());
		if (!player) 
		{
			ERROR("未能找到玩家:{}", meta.player_id());
			return;
		}
		player->SendProtocol(message);
	}
	else //if (meta.player_id() == 0)
	{
		//
		//游戏逻辑处理流程
		//
		//特别注意：不能传入player_id在Meta数据当中
		//
		//来自Client协议均在此处理，逻辑程序员请勿在此后添加代码
		//
		
		DEBUG("3.中心服务器接收来自Client的协议:{}", meta.ShortDebugString());
		
		if (Asset::META_TYPE_C2S_LOGIN == meta.type_t()) //账号登陆
		{
			Asset::Login* login = dynamic_cast<Asset::Login*>(message);
			if (!login) return; 
			
			_account.CopyFrom(login->account()); //账号信息
			_player_list.clear(); //账号下玩家列表，目前只有一个玩家
		
			auto success = RedisInstance.GetUser(login->account().username(), _user);

			if (!success) //没有该用户
			{
				_user.mutable_account()->CopyFrom(login->account());
			}
			else
			{
				LOG(TRACE, "get player_name success, username:{} who has exist.", login->account().username());
			}

			//
			//如果账号下没有角色，则创建一个
			//
			//对于需要玩家自定义角色数据的游戏，此处要单独处理，比如需要选择性别
			//
			if (_user.player_list().size() == 0)
			{
				int64_t player_id = RedisInstance.CreatePlayer(); //如果账号下没有角色，创建一个给Client
				if (player_id == 0) return;
				
				_player = std::make_shared<Player>(player_id, shared_from_this());
				std::string player_name = NameInstance.Get();

				TRACE("get player_name success, player_id:{}, player_name:{}", player_id, player_name.c_str());

				_player->SetName(player_name);
				_player->SetAccount(login->account().username());

				_player->Save(); //存盘，防止数据库无数据

				_user.mutable_player_list()->Add(player_id);
				RedisInstance.SaveUser(_access_token.openid(), _user); //账号数据存盘
			}

			for (auto player_id : _user.player_list()) _player_list.emplace(player_id); //玩家数据

			LOG(INFO, "user:{} account:{} wechat:{} token:{}", _user.ShortDebugString(), _account.ShortDebugString(), _wechat.ShortDebugString(), _access_token.ShortDebugString());
				
			if (!_user.has_wechat_token()) _user.mutable_wechat_token()->CopyFrom(_access_token);
			if (!_user.has_wechat()) _user.mutable_wechat()->CopyFrom(_wechat); //微信数据

			if (_access_token.has_openid())	RedisInstance.SaveUser(_access_token.openid(), _user); //账号数据存盘
			
			//
			//发送当前的角色信息
			//
			Asset::PlayerList player_list; 
			player_list.mutable_player_list()->CopyFrom(_user.player_list());
			player_list.mutable_wechat()->CopyFrom(_user.wechat());
			SendProtocol(player_list); 
		}
		else if (Asset::META_TYPE_C2S_LOGOUT == meta.type_t()) //账号登出
		{
			Asset::Logout* logout = dynamic_cast<Asset::Logout*>(message);
			if (!logout) return; 

			OnLogout();
		}
		else if (Asset::META_TYPE_C2S_WECHAT_LOGIN == meta.type_t()) //微信登陆
		{
			Asset::WechatLogin* login = dynamic_cast<Asset::WechatLogin*>(message);
			if (!login) return; 

			OnWechatLogin(message); //初始化账号信息
		}
		else if (Asset::META_TYPE_SHARE_GUEST_LOGIN == meta.type_t()) //游客登陆
		{
			Asset::GuestLogin* login = dynamic_cast<Asset::GuestLogin*>(message);
			if (!login) return; 
			
			std::string account;
			if (RedisInstance.GetGuestAccount(account)) login->set_account(account);

			SendProtocol(login); 
		}
		else if (Asset::META_TYPE_C2S_RECONNECT == meta.type_t()) //断线重连
		{
			Asset::ReConnect* connect = dynamic_cast<Asset::ReConnect*>(message);
			if (!connect) return; 

			_player = PlayerInstance.Get(connect->player_id());
			if (!_player) return;

			_player->OnEnterGame();
		}
		else if (Asset::META_TYPE_C2S_ENTER_GAME == meta.type_t()) //进入游戏
		{
			const Asset::EnterGame* enter_game = dynamic_cast<Asset::EnterGame*>(message);
			if (!enter_game) return; 

			if (_player_list.find(enter_game->player_id()) == _player_list.end())
			{
				LOG(ERROR, "player_id:{} has not found in username:{}, maybe it is cheated.", enter_game->player_id(), _account.username());
				return; //账号下没有该角色数据
			}

			if (!_player) 
			{
				_player = std::make_shared<Player>(enter_game->player_id(), shared_from_this());
				SetRoleType(Asset::ROLE_TYPE_PLAYER, _player->GetID());
			}
			//
			// 已经在线玩家检查
			//
			// 对于已经进入游戏内操作的玩家进行托管
			//
			auto session = WorldSessionInstance.GetPlayerSession(_player->GetID());
			if (session) //已经在线
			{
				session->KickOutPlayer(Asset::KICK_OUT_REASON_OTHER_LOGIN);
			}
			//WorldSessionInstance.AddPlayer(_player->GetID(), shared_from_this()); //在线玩家
			
			//
			//此时才可以真正进入游戏大厅
			//
			//加载数据
			//
			if (_player->OnEnterGame()) //理论上不会出现
			{
				Asset::AlertMessage alert;
				alert.set_error_type(Asset::ERROR_TYPE_NORMAL);
				alert.set_error_show_type(Asset::ERROR_SHOW_TYPE_CHAT);
				alert.set_error_code(Asset::ERROR_DATABASE); //数据库错误

				SendProtocol(alert);
			}
		}
		else if (Asset::META_TYPE_SHARE_ENTER_ROOM == meta.type_t()) //进入房间：进入游戏逻辑服务器的入口
		{
			//
			//进入房间根据房间号选择房间所在逻辑服务器进行会话选择
			//
			if (!_player) return;

			Asset::EnterRoom* enter_room = dynamic_cast<Asset::EnterRoom*>(message);
			if (!enter_room) return; 

			auto room_id = enter_room->room().room_id();
			auto server_id = room_id >> 16;

			auto game_server = WorldSessionInstance.GetServerSession(server_id);
			if (!game_server) return;

			_player->SetGameServer(game_server);
			_player->SendProtocol2GameServer(enter_room); //转发

			WorldSessionInstance.SetPlayerSession(_player->GetID(), game_server);
		}
		else if (Asset::META_TYPE_SHARE_UPDATE_CLIENT_DATA == meta.type_t()) //Client参数数据
		{
			auto client_data = dynamic_cast<Asset::UpdateClientData*>(message);
			if (!client_data) return;

			_user.mutable_client_info()->CopyFrom(client_data->client_info());
				
			RedisInstance.SaveUser(_user.account().username(), _user); //账号数据存盘

			SendProtocol(message);
		}
		else
		{
			if (!_player) 
			{
				LOG(ERROR, "Player has not inited.");
				return; //尚未初始化
			}
			//协议处理
			_player->HandleProtocol(meta.type_t(), message);
		}
	}
}

void WorldSession::KickOutPlayer(Asset::KICK_OUT_REASON reason)
{
	if (!_player) return;

	_player->OnKickOut(reason); //玩家退出登陆
	
	_online = false;
}
	
void WorldSession::OnLogout()
{
	if (!_player) return;
	
	_player->Logout(nullptr);

	_online = false;
}
	
int32_t WorldSession::OnWechatLogin(const pb::Message* message)
{
	const Asset::WechatLogin* login = dynamic_cast<const Asset::WechatLogin*>(message);
	if (!login) return 1; 

	const auto& access_code = login->access_code();

	CkHttp http;
	std::string err;

	bool success = http.UnlockComponent("ARKTOSHttp_decCLPWFQXmU");
	if (success != true) {
		ERROR("使用开源库获取HTTP服务失败:{}", http.lastErrorText());
		return 2;
	}

	//
	//1.通过code获取access_token
	//
	std::string request = "https://api.weixin.qq.com/sns/oauth2/access_token?appid=wx5763b38a2613f9fb&secret=f9128ba451c51ce44fdd3bddf2fa45e7&code=" + access_code +"&grant_type=authorization_code";
	const char *html = http.quickGetStr(request.c_str());

	std::string response(html);

	if (response.find("access_token") != std::string::npos)
	{
		int ret = pbjson::json2pb(html, &_access_token, err);
		if (ret)
		{
			LOG(ERROR, "json2pb ret:{} error:{} html:{}", ret, err, html);
			return ret;
		}

		LOG(INFO, "微信: html:{} access_token:{}", html, _access_token.ShortDebugString());

		auto expires_in = _access_token.expires_in();

		if (expires_in) //尚未过期
		{
			//
			//3.获取用户个人信息（UnionID机制）
			//
			request = "https://api.weixin.qq.com/sns/userinfo?access_token=" + _access_token.access_token() + "&openid=" + _access_token.openid();
			html = http.quickGetStr(request.c_str());

			ret = pbjson::json2pb(html, &_wechat, err);
			if (ret)
			{
				LOG(ERROR, "json2pb ret:{} error:{} html:{}", ret, err, html);
				return ret;
			}

			LOG(INFO, "微信: html:{} union_info:{}", html, _wechat.ShortDebugString());

			Asset::WeChatInfo proto;
			proto.mutable_wechat()->CopyFrom(_wechat);
			SendProtocol(proto); //同步Client
		}
		else
		{
			//
			//2.刷新或续期access_token使用
			//
			auto refresh_token = _access_token.refresh_token();
			request = "https://api.weixin.qq.com/sns/oauth2/refresh_token?appid=wx5763b38a2613f9fb&grant_type=refresh_token&refresh_token=" + refresh_token;
			html = http.quickGetStr(request.c_str());
			std::string response(html);

			int ret = pbjson::json2pb(html, &_access_token, err);
			if (ret)
			{
				LOG(ERROR, "json2pb ret:{} error:{} html:{}", ret, err, html);
				return ret;
			}
	
			if (response.find("errcode") != std::string::npos) //刷新失败
			{
				Asset::WechatError wechat_error;
				int ret = pbjson::json2pb(html, &wechat_error, err);
				if (ret)
				{
					LOG(ERROR, "json2pb ret:{} error:{} html:{}", ret, err, html);
					return ret;
				}
				Asset::WeChatInfo proto;
				proto.mutable_wechat_error()->CopyFrom(wechat_error);
				SendProtocol(proto); //同步Client
			}
			else //刷新成功
			{
				//
				//3.获取用户个人信息（UnionID机制）
				//
				auto openid = _access_token.openid();
				auto refresh_token = _access_token.refresh_token();

				request = "https://api.weixin.qq.com/sns/userinfo?access_token=" + refresh_token + "&openid=" + openid;
				html = http.quickGetStr(request.c_str());

				ret = pbjson::json2pb(html, &_wechat, err);
				if (ret)
				{
					LOG(ERROR, "json2pb ret:{} error:{} html:{}", ret, err, html);
					return ret;
				}
			
				LOG(INFO, "微信: html:{} _wechat:{}", html, _wechat.ShortDebugString());

				Asset::WeChatInfo proto;
				proto.mutable_wechat()->CopyFrom(_wechat);
				SendProtocol(proto); //同步Client
			}
			
			LOG(INFO, "微信: html:{} refresh:{}", html, _access_token.ShortDebugString());
		}
	}
	else if (response.find("errcode") != std::string::npos)
	{
		Asset::WechatError wechat_error;
		int ret = pbjson::json2pb(html, &wechat_error, err);
		if (ret)
		{
			LOG(ERROR, "json2pb ret:{} error:{} html:{}", ret, err, html);
			return ret;
		}
		Asset::WeChatInfo proto;
		proto.mutable_wechat_error()->CopyFrom(wechat_error);
		SendProtocol(proto); //同步Client
	}
			
	//
	//微信登陆之后，直接进行存盘，防止玩家此时退出
	//

	if (!_user.has_wechat_token()) _user.mutable_wechat_token()->CopyFrom(_access_token);
	if (!_user.has_wechat()) _user.mutable_wechat()->CopyFrom(_wechat); //微信数据

	if (_access_token.has_openid())	RedisInstance.SaveUser(_access_token.openid(), _user); //账号数据存盘

	return 0;
}

void WorldSession::Start()
{
	AsyncReceiveWithCallback(&WorldSession::InitializeHandler);
}
	
bool WorldSession::Update() 
{ 
	if (!Socket::Update()) return false;

	if (!_player) return true; //长时间未能上线

	if (!_online) return false;

	_player->Update(); 

	return true;
}

void WorldSession::OnClose()
{
	KickOutPlayer(Asset::KICK_OUT_REASON_DISCONNECT);
}

void WorldSession::SendProtocol(const pb::Message* message)
{
	SendProtocol(*message);
}

void WorldSession::SendProtocol(const pb::Message& message)
{
	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::META_TYPE_IsValid(type_t)) return;	//如果不合法，不检查会宕线
	
	Asset::Meta meta;
	meta.set_type_t((Asset::META_TYPE)type_t);
	meta.set_stuff(message.SerializeAsString());

	std::string content = meta.SerializeAsString();

	if (content.empty()) return;

	EnterQueue(std::move(content));
}

void WorldSession::SendMeta(const Asset::Meta& meta)
{
	std::string content = meta.SerializeAsString();

	if (content.empty()) return;

	EnterQueue(std::move(content));
}

void WorldSessionManager::BroadCast2GameServer(const pb::Message* message)
{
	if (!message) return;
	BroadCast2GameServer(*message);
}

void WorldSessionManager::BroadCast2GameServer(const pb::Message& message)
{
	for (auto session : _server_list)
	{
		if (!session.second) continue;
		session.second->SendProtocol(message);
	}
}
	
void WorldSessionManager::BroadCast(const pb::Message& message)
{
	for (auto session : _client_list)
	{
		if (!session.second) continue;
		session.second->SendProtocol(message);
	}
}

void WorldSessionManager::BroadCast(const pb::Message* message)
{
	if (!message) return;
	BroadCast(*message);
}

std::shared_ptr<WorldSession> WorldSessionManager::RandomServer()
{
	if (_server_list.size() == 0) return nullptr;

	std::vector<int32_t> server_list;
	for (auto it = _server_list.begin(); it != _server_list.end(); ++it) server_list.push_back(it->first);

	std::random_shuffle(server_list.begin(), server_list.end()); //随机

	return _server_list[server_list[0]];
}

NetworkThread<WorldSession>* WorldSessionManager::CreateThreads() const
{    
	return new NetworkThread<WorldSession>[GetNetworkThreadCount()];
}

void WorldSessionManager::OnSocketAccept(tcp::socket&& socket, int32_t thread_index)
{    
	WorldSessionInstance.OnSocketOpen(std::forward<tcp::socket>(socket), thread_index);
}

bool WorldSessionManager::StartNetwork(boost::asio::io_service& io_service, const std::string& bind_ip, int32_t port, int thread_count)
{
	if (!SuperSocketManager::StartNetwork(io_service, bind_ip, port, thread_count)) return false;
	_acceptor->SetSocketFactory(std::bind(&SuperSocketManager::GetSocketForAccept, this));    
	_acceptor->AsyncAcceptWithCallback<&OnSocketAccept>();    
	return true;
}

}
