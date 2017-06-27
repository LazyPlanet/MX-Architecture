#include <spdlog/spdlog.h>

#include "WorldSession.h"
#include "RedisManager.h"
#include "CommonUtil.h"
#include "MXLog.h"
#include "Protocol.h"
#include "Player.h"
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
			
			pb::Message* msg = ProtocolInstance.GetMessage(meta.type_t());	
			if (!msg) 
			{
				TRACE("Could not found message of type:{}", meta.type_t());
				return;		//非法协议
			}

			auto message = msg->New();
			
			result = message->ParseFromArray(meta.stuff().c_str(), meta.stuff().size());
			if (!result) 
			{
				DEBUG_ASSERT(false);
				return;		//非法协议
			}
			
			if (Asset::META_TYPE_C2S_COUNT <= meta.type_t()) 
			{
				//
				//游戏服务器内部协议处理
				//
				OnInnerProcess(meta); //内部处理
			}
			else
			{
				//
				//游戏逻辑处理流程
				//
				//来自Client协议均在此处理，逻辑程序员请勿在此后添加代码
				//
				
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
				else if (Asset::META_TYPE_SHARE_GUEST_LOGIN == meta.type_t()) //游客登陆
				{
					Asset::GuestLogin* login = dynamic_cast<Asset::GuestLogin*>(message);
					if (!login) return; 
					
					std::string account;
					auto redis = std::make_shared<Redis>();
					if (redis->GetGuestAccount(account)) login->set_account(account);
					SendProtocol(login); 
				}
				else if (Asset::META_TYPE_C2S_RECONNECT == meta.type_t()) //断线重连
				{
					Asset::ReConnect* connect = dynamic_cast<Asset::ReConnect*>(message);
					if (!connect) return; 

					g_player = PlayerInstance.Get(connect->player_id());
					if (!g_player) return;

					g_player->OnEnterGame();
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

					if (!g_player) 
					{
						g_player = std::make_shared<Player>(enter_game->player_id(), shared_from_this());

						SetID(g_player->GetID());
						SetRoleType(Asset::ROLE_TYPE_PLAYER);
					}
					//
					// 已经在线玩家检查
					//
					// 对于已经进入游戏内操作的玩家进行托管
					//
					auto session = WorldSessionInstance.GetPlayer(g_player->GetID());
					if (session) //已经在线
					{
						session->KillOutPlayer();
					}
					WorldSessionInstance.AddPlayer(g_player->GetID(), shared_from_this()); //在线玩家
					
					//
					//此时才可以真正进入游戏大厅
					//
					//加载数据
					//
					if (g_player->OnEnterGame()) //理论上不会出现
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
					if (!g_player) return;

					Asset::EnterRoom* enter_room = dynamic_cast<Asset::EnterRoom*>(message);
					if (!enter_room) return; 

					auto room_id = enter_room->room().room_id();
					auto server_id = room_id >> 16;

					auto game_server = WorldSessionInstance.GetServer(server_id);
					if (!game_server) return;

					WorldSessionInstance.SetPlayerSession(g_player->GetID(), game_server);

					g_player->SendProtocol2GameServer(enter_room); //转发
				}
				else
				{
					if (!g_player) 
					{
						LOG(ERROR, "Player has not inited.");
						return; //尚未初始化
					}
					//协议处理
					g_player->HandleProtocol(meta.type_t(), message);
				}
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
		//WorldSessionInstance.Erase(g_player->GetID()); //玩家自行处理对象管理

		//提示Client
		Asset::KillOut message;
		message.set_player_id(g_player->GetID());
		message.set_out_reason(Asset::KILL_OUT_REASON_OTHER_LOGIN);
		SendProtocol(message); //有可能发送失败，因为此时玩家已经断开网络连接

		//玩家退出登陆
		g_player->Logout(nullptr);
		g_player.reset();
		g_player = nullptr;
	}
}
	
void WorldSession::OnLogout()
{
	if (g_player) 
	{
		WARN("player_id:{}", g_player->GetID())
		g_player->Logout(nullptr);
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

	if (content.empty()) 
	{
		ERROR("player_id:{} send nothing.");
		return;
	}

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
