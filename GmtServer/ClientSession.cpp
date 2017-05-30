#include "ClientSession.h"
#include "RedisManager.h"
#include "MXLog.h"

namespace Adoter
{

ClientSession::ClientSession(boost::asio::ip::tcp::socket&& socket) : Socket(std::move(socket))
{
	_remote_endpoint = _socket.remote_endpoint();
	_ip_address = _remote_endpoint.address().to_string();
}

void ClientSession::InitializeHandler(const boost::system::error_code error, const std::size_t bytes_transferred)
{
	try
	{
		if (error)
		{
			ERROR("Remote client disconnect, remote_ip:{} account:{} player_id:{}", _ip_address, _account, _plyaer_id);
			return;
		}
		else
		{
			TRACE("Receive message from game server:{} bytes_transferred:{}", _ip_address, bytes_transferred);

			Asset::InnerMeta meta;
			auto result = meta.ParseFromArray(_buffer.data(), bytes_transferred);
			if (!result) 
			{
				LOG(ERROR, "Receive message error from server:{}", _ip_address);
				return;
			}
				
			InnerProcess(meta);
			LOG(INFO, "Receive message:{} from server:{}", meta.ShortDebugString(), _ip_address);
		}
	}
	catch (std::exception& e)
	{
		return;
	}
	//递归持续接收	
	AsyncReceiveWithCallback(&ClientSession::InitializeHandler);
}

bool ClientSession::InnerProcess(const Asset::InnerMeta& meta)
{
	TRACE("Receive message:{} from server", meta.ShortDebugString());

	switch (meta.type_t())
	{
		case Asset::INNER_TYPE_REGISTER: //注册服务器
		{
			Asset::Register message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			if (message.server_type() == Asset::SERVER_TYPE_GMT) //GMT服务器
			{
				ClientSessionInstance.SetGmtServer(shared_from_this());
			}
			else if (message.server_type() == Asset::SERVER_TYPE_GAME) //游戏服务器
			{
				ClientSessionInstance.Add(shared_from_this());
			}
		}
		break;
		
		case Asset::INNER_TYPE_COMMAND: //发送指令
		{
			Asset::Command message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			if (ClientSessionInstance.IsGmtServer(shared_from_this())) //处理GMT服务器发送的数据
			{
				auto error_code = OnCommandProcess(message); //处理离线玩家的指令执行
				if (Asset::COMMAND_ERROR_CODE_PLAYER_ONLINE == error_code) ClientSessionInstance.BroadCastProtocol(meta); //处理在线玩家的指令执行
				DEBUG("Server:{} server send gmt message:{} error_code:{}", _ip_address, message.ShortDebugString(), error_code);
			}
			else //处理游戏服务器发送的数据
			{
				auto gmt_server = ClientSessionInstance.GetGmtServer();
				if (!gmt_server) return false;
			
				gmt_server->SendProtocol(message);
			}
		}
		break;

		default:
		{
			WARN("Receive message:{} from server has no process type:{}", meta.ShortDebugString(), meta.type_t());
		}
		break;
	}
	return true;
}
			
Asset::COMMAND_ERROR_CODE ClientSession::OnCommandProcess(const Asset::Command& command)
{
#define RETURN(x) \
	auto response = command; \
	response.set_error_code(x); \
	ERROR("command excute for:{} command:{}", x, command.ShortDebugString()); \
	SendProtocol(response); \
	return x; \

	auto redis = make_unique<Redis>();

	Asset::User user; //账号数据
	auto result = user.ParseFromString(redis->GetUser(command.account()));
	if (!result)
	{
		RETURN(Asset::COMMAND_ERROR_CODE_NO_ACCOUNT);
	}

	//玩家角色校验
	auto player_id = command.player_id();
	if (player_id <= 0) 
	{
		RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
	}
	
	auto it = std::find(user.player_list().begin(), user.player_list().end(), player_id);
	if (it == user.player_list().end()) 
	{
		RETURN(Asset::COMMAND_ERROR_CODE_NO_PLAYER); //账号下不存在该角色
	}

	Asset::Player player;
	result = player.ParseFromString(redis->GetPlayer(player_id));
	if (!result)
	{
		RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
	}

	if (player.logout_time() == 0 && player.login_time() != 0) //玩家在线不支持
	{
		RETURN(Asset::COMMAND_ERROR_CODE_PLAYER_ONLINE); //玩家目前在线
	}
		
	if (command.count() <= 0) //不可能是负数
	{
		RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
	}

	switch(command.command_type())
	{
		case Asset::COMMAND_TYPE_RECHARGE:
		{
			player.mutable_common_prop()->set_diamond(player.common_prop().diamond() + command.count());
		}
		break;
		
		case Asset::COMMAND_TYPE_ROOM_CARD:
		{
		}
		break;
		
		case Asset::COMMAND_TYPE_HUANLEDOU:
		{
			player.mutable_common_prop()->set_huanledou(player.common_prop().huanledou() + command.count());
		}
		break;
		
		default:
		{
		}
		break;
	}

	//存盘
	auto stuff = player.SerializeAsString();
	redis->SavePlayer(player_id, stuff);

	RETURN(Asset::COMMAND_ERROR_CODE_SUCCESS); //成功执行

#undef RETURN
}

void ClientSession::Start()
{
	AsyncReceiveWithCallback(&ClientSession::InitializeHandler);
}
	
bool ClientSession::Update() 
{ 
	return true;
}

void ClientSession::OnClose()
{
}

void ClientSession::SendProtocol(const pb::Message* message)
{
	if (!message) return;
	SendProtocol(*message);
}

void ClientSession::SendProtocol(const pb::Message& message)
{
	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::INNER_TYPE_IsValid(type_t)) return;	//如果不合法，不检查会宕线
	
	Asset::InnerMeta meta;
	meta.set_type_t((Asset::INNER_TYPE)type_t);
	meta.set_stuff(message.SerializeAsString());

	std::string content = meta.SerializeAsString();

	if (content.empty()) 
	{
		ERROR("server:{} send nothing, message:{}", _ip_address, meta.ShortDebugString());
		return;
	}

	TRACE("server:{} send message to gmt server, message:{}", _ip_address, meta.ShortDebugString());
	AsyncSend(content);
}
	
void ClientSessionManager::BroadCastProtocol(const pb::Message* message)
{
	if (!message) return;
	BroadCastProtocol(*message);
}

void ClientSessionManager::BroadCastProtocol(const pb::Message& message)
{
	for (auto session : _sessions)
	{
		if (!session) continue;
		session->SendProtocol(message);
	}
}

void ClientSessionManager::Add(std::shared_ptr<ClientSession> session)
{
	//std::lock_guard<std::mutex> lock(_mutex);
	_sessions.push_back(session);
}	

NetworkThread<ClientSession>* ClientSessionManager::CreateThreads() const
{    
	return new NetworkThread<ClientSession>[GetNetworkThreadCount()];
}

void ClientSessionManager::OnSocketAccept(tcp::socket&& socket, int32_t thread_index)
{    
	ClientSessionInstance.OnSocketOpen(std::forward<tcp::socket>(socket), thread_index);
}

bool ClientSessionManager::StartNetwork(boost::asio::io_service& io_service, const std::string& bind_ip, int32_t port, int thread_count)
{
	if (!SuperSocketManager::StartNetwork(io_service, bind_ip, port, thread_count)) return false;
	_acceptor->SetSocketFactory(std::bind(&SuperSocketManager::GetSocketForAccept, this));    
	_acceptor->AsyncAcceptWithCallback<&OnSocketAccept>();    
	return true;
}

}
