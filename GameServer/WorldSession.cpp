#include <spdlog/spdlog.h>

#include "WorldSession.h"
#include "RedisManager.h"
#include "CommonUtil.h"
#include "Player.h"
#include "MXLog.h"
#include "Protocol.h"
#include "PlayerName.h"

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
			WARN("Remote client disconnect, remote_ip:{}, player_id:{}", _ip_address, g_player ? g_player->GetID() : 0);
			KillOutPlayer();
			//Close(); ////断开网络连接，不要显示的关闭网络连接
			return;
		}
		else
		{
			Asset::Meta meta;
			bool result = meta.ParseFromArray(_buffer.data(), bytes_transferred);

			if (!result) return;		//非法协议
			
			/////////////////////////////////////////////////////////////////////////////打印收到协议提示信息
		
			/*
			const pb::FieldDescriptor* type_field = meta.GetDescriptor()->FindFieldByName("type_t");
			if (!type_field) return;

			const pb::EnumValueDescriptor* enum_value = meta.GetReflection()->GetEnum(meta, type_field);
			if (!enum_value) return;

			const std::string& enum_name = enum_value->name();
			
			if (g_player)
			{
				TRACE("player_id:{0} receive message type:{1}", g_player->GetID(), enum_name.c_str());
			}
			else
			{
				TRACE("player_id:0 receive message type:{0}", enum_name.c_str());
			}
			*/

			pb::Message* msg = ProtocolInstance.GetMessage(meta.type_t());	
			if (!msg) 
			{
				TRACE("Could not found message of type:%d", meta.type_t());
				return;		//非法协议
			}

			auto message = msg->New();
			
			result = message->ParseFromArray(meta.stuff().c_str(), meta.stuff().size());
			if (!result) 
			{
				DEBUG_ASSERT(false);
				return;		//非法协议
			}
		
			/////////////////////////////////////////////////////////////////////////////游戏逻辑处理流程
			
			if (Asset::META_TYPE_C2S_LOGIN == meta.type_t()) //账号登陆
			{
				Asset::Login* login = dynamic_cast<Asset::Login*>(message);
				if (!login) return; 
				
				_player_list.clear(); //账号下玩家列表，对于棋牌游戏，只有一个玩家
			
				Asset::User user;
				
			 	auto redis = std::make_shared<Redis>();
				auto success = redis->GetUser(login->account().username(), user);

				if (!success) //没有该用户
				{
					user.mutable_account()->CopyFrom(login->account());

					int64_t player_id = redis->CreatePlayer(); //如果账号下没有角色，创建一个给Client

					if (player_id == 0) 
					{
						LOG(ERROR, "create player failed, username:{}", login->account().username());
						return; //创建失败
					}

					user.mutable_player_list()->Add(player_id);

					redis->SaveUser(login->account().username(), user); //账号数据存盘

					g_player = std::make_shared<Player>(player_id, shared_from_this());
					std::string player_name = NameInstance.Get();

					TRACE("get player_name success, player_id:{}, player_name:{}", player_id, player_name.c_str());

					g_player->SetName(player_name);
					g_player->Save(); //存盘，防止数据库无数据
				}
				else
				{
					LOG(TRACE, "get player_name success, username:{} who has exist.", login->account().username());
				}

				_account.CopyFrom(login->account()); //账号信息

				for (auto player_id : user.player_list()) _player_list.emplace(player_id); //玩家数据
				
				//
				//发送当前的角色信息
				//
				Asset::PlayerList player_list; 
				player_list.mutable_player_list()->CopyFrom(user.player_list());
				SendProtocol(player_list); 
			}
			else if (Asset::META_TYPE_C2S_LOGOUT == meta.type_t()) //账号登出
			{
				Asset::Logout* logout = dynamic_cast<Asset::Logout*>(message);
				if (!logout) return; 

				OnLogout();
			}
			/*
			else if (Asset::META_TYPE_SHARE_CREATE_PLAYER == meta.type_t()) //创建角色
			{
				Asset::CreatePlayer* create_player = dynamic_cast<Asset::CreatePlayer*>(message);
				if (!create_player) return; 
				
			 	std::shared_ptr<Redis> redis = std::make_shared<Redis>();
				int64_t player_id = redis->CreatePlayer();
				if (player_id == 0) return; //创建失败

				g_player = std::make_shared<Player>(player_id, shared_from_this());
				g_player->Save(); //存盘，防止数据库无数据
				
				//返回结果
				create_player->set_player_id(player_id);
				g_player->SendProtocol(create_player);
			}
			*/
			else if (Asset::META_TYPE_C2S_ENTER_GAME == meta.type_t()) //进入游戏
			{
				const Asset::EnterGame* enter_game = dynamic_cast<Asset::EnterGame*>(message);
				if (!enter_game) return; 

				if (_player_list.find(enter_game->player_id()) == _player_list.end())
				{
					ERROR("player_id:{} has not found, maybe it is cheated.", enter_game->player_id());
					return; //账号下没有该角色数据
				}

				if (!g_player) g_player = std::make_shared<Player>(enter_game->player_id(), shared_from_this());
				//
				// 已经在线玩家检查
				//
				// 对于已经进入游戏内操作的玩家进行托管
				//
				auto session = WorldSessionInstance.Get(g_player->GetID());
				if (session) //已经在线
				{
					session->KillOutPlayer();
				}
				WorldSessionInstance.Emplace(g_player->GetID(), shared_from_this()); //在线玩家
				
				//
				//此时才可以真正进入游戏大厅
				//
				//加载数据
				//
				if (g_player->OnEnterGame())
				{
					Asset::AlertMessage alert;
					alert.set_error_type(Asset::ERROR_TYPE_NORMAL);
					alert.set_error_show_type(Asset::ERROR_SHOW_TYPE_CHAT);
					alert.set_error_code(Asset::ERROR_DATABASE); //数据库错误

					SendProtocol(alert);
				}
			}
			else
			{
				if (!g_player) 
				{
					ERROR("Player has not inited.");
					return; //未初始化的Player
				}
				//协议处理
				g_player->HandleProtocol(meta.type_t(), message);
			}
		}
	}
	catch (std::exception& e)
	{
		ERROR("Remote client disconnect, remote_ip:{}, error:{}, player_id:{}", _ip_address, e.what(), g_player ? g_player->GetID() : 0);
		//Close(); //不用显示关闭网络连接
		return;
	}
	//递归持续接收	
	AsyncReceiveWithCallback(&WorldSession::InitializeHandler);
}

void WorldSession::KillOutPlayer()
{
	if (g_player) //网络断开
	{
		WorldSessionInstance.Erase(g_player->GetID());

		//提示Client
		Asset::KillOut message;
		message.set_player_id(g_player->GetID());
		message.set_out_reason(Asset::KILL_OUT_REASON_OTHER_LOGIN);
		SendProtocol(message);

		//玩家退出登陆
		g_player->OnLogout(nullptr);
		g_player.reset();
		g_player = nullptr;
	}
}
	
void WorldSession::OnLogout()
{
	if (g_player) 
	{
		WARN("player_id:{}", g_player->GetID())
		g_player->OnLogout(nullptr);
	}
}

void WorldSession::Start()
{
	AsyncReceiveWithCallback(&WorldSession::InitializeHandler);
}
	
bool WorldSession::Update() 
{ 
	if (!Socket::Update()) return false;

	if (!g_player) 
	{
		return true; //长时间未能上线
	}

	g_player->Update(); 

	return true;
}

void WorldSession::OnClose()
{
	KillOutPlayer();
}

void WorldSession::SendProtocol(pb::Message* message)
{
	SendProtocol(*message);
}

void WorldSession::SendProtocol(pb::Message& message)
{
	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::META_TYPE_IsValid(type_t)) return;	//如果不合法，不检查会宕线
	
	Asset::Meta meta;
	meta.set_type_t((Asset::META_TYPE)type_t);
	meta.set_stuff(message.SerializeAsString());

	std::string content = meta.SerializeAsString();

	if (content.empty()) 
	{
		ERROR("player_id:{} send nothing.");
		return;
	}

	EnterQueue(std::move(content));
}

void WorldSessionManager::Emplace(int64_t player_id, std::shared_ptr<WorldSession> session)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (_sessions.find(player_id) == _sessions.end())
		_sessions.emplace(player_id, session);
}
	
void WorldSessionManager::Erase(int64_t player_id)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_sessions.erase(player_id);
}
	
std::shared_ptr<WorldSession> WorldSessionManager::Get(int64_t player_id)
{
	return _sessions[player_id];
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
