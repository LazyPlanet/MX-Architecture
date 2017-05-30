#include "ClientSession.h"
#include "RedisManager.h"
#include "MXLog.h"
#include "Player.h"

namespace Adoter
{

ClientSession::ClientSession(boost::asio::io_service& io_service, const boost::asio::ip::tcp::endpoint& endpoint) : 
	ClientSocket(io_service, endpoint)
{
	_remote_endpoint = endpoint;
	_ip_address = endpoint.address().to_string();
}
	
void ClientSession::OnConnected()
{
	DEBUG("Connet GmtServer success, ip_address:{}", _ip_address);

	//注册服务器角色
	Asset::Register message;
	message.set_server_type(Asset::SERVER_TYPE_GAME);
	message.set_server_id(1);
	SendProtocol(message);
}

void ClientSession::OnReceived(const std::string& message)
{
	Asset::InnerMeta meta;
	auto result = meta.ParseFromString(message);
	if (!result)
	{
		LOG(ERROR, "Receive message error from server:{}", _ip_address);
		return;
	}

	InnerProcess(meta);
	LOG(INFO, "Receive message:{} from server:{}", meta.ShortDebugString(), _ip_address);
}

bool ClientSession::InnerProcess(const Asset::InnerMeta& meta)
{
	TRACE("Receive message:{} from server", meta.ShortDebugString());

	switch (meta.type_t())
	{
		case Asset::INNER_TYPE_COMMAND: //GMT指令
		{
			Asset::Command message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			OnCommandProcess(message);
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

	std::shared_ptr<Player> player_ptr = nullptr;

	//玩家角色校验
	auto player_id = command.player_id();
	if (player_id <= 0) 
	{
		RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
	}
	
	if (command.count() <= 0) //不可能是负数
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

	if (player.logout_time() != 0 && player.login_time() == 0) //玩家在线
	{
		RETURN(Asset::COMMAND_ERROR_CODE_PLAYER_OFFLINE); //玩家目前不在线
	}

	player_ptr = PlayerInstance.Get(player_id);
	//
	//理论上玩家应该在线，但是没有查到该玩家，原因
	//
	//1.玩家已经下线; 2.玩家在其他服务器上;
	//
	if (!player_ptr) 
	{
		RETURN(Asset::COMMAND_ERROR_CODE_PLAYER_OFFLINE); //玩家目前不在线
	}

	switch(command.command_type())
	{
		case Asset::COMMAND_TYPE_RECHARGE:
		{
			player_ptr->IncreaseHuanledou(command.count());
		}
		break;
		
		case Asset::COMMAND_TYPE_ROOM_CARD:
		{
			auto ret = player_ptr->GainItem(command.item_id(), command.count());   
			if (!ret)
			{
				LOG(ERR, "Excute command to player_id:{} for gainning room card id:{} count:{} error.", player_id, command.item_id(), command.count());
				RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
			}
		}
		break;
		
		case Asset::COMMAND_TYPE_HUANLEDOU:
		{
			player_ptr->IncreaseDiamond(command.count());
		}
		break;
		
		default:
		{
		}
		break;
	}

	//存盘
	player_ptr->Save();

	RETURN(Asset::COMMAND_ERROR_CODE_SUCCESS); //玩家目前在线

#undef RETURN
}

void ClientSession::SendProtocol(pb::Message* message)
{
	SendProtocol(*message);
}

void ClientSession::SendProtocol(pb::Message& message)
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
	AsyncSendMessage(content);
}

bool ClientSession::StartSend()
{
	bool started = false;

	while (IsConnected())
	{
		if (_send_list.size())
		{
			auto& message = _send_list.front();
			AsyncWriteSome(message.c_str(), message.size());
			_send_list.pop_front();

			started = true;
		}
		else
		{
			break;
		}
	}
	return started;
}


void ClientSession::AsyncSendMessage(std::string message)
{
	if (IsClosed()) return;

	_send_list.push_back(message);

	StartSend();
}

void ClientSession::OnWriteSome(const boost::system::error_code& error, std::size_t bytes_transferred)
{
	if (!IsConnected()) return;

	if (error)
	{
		Close(error.message());
		CRITICAL("server:{} send success, bytes_transferred:{}, error:{}", _ip_address, bytes_transferred, error.message());
		return;
	}

	DEBUG("server:{} send success, bytes_transferred:{}, error:{}", _ip_address, bytes_transferred, error.message());
	/*
	std::deque<std::string> send_messages;
	send_messages.swap(_send_list);
	
	if (!IsClosed() && !send_messages.empty())
	{
		const std::string& message = send_messages.front();
		AsyncWriteSome(message.c_str(), message.size()); //发送数据
		send_messages.pop_front();
	}
	else
	{
		StartSend();
	}
	*/
}

bool ClientSession::StartReceive()
{
	if (!IsConnected()) return false;

	AsynyReadSome();
	return true;
}

void ClientSession::OnReadSome(const boost::system::error_code& error, std::size_t bytes_transferred)
{
	if (!IsConnected()) return;

	if (error)
	{
		if (error != boost::asio::error::eof)
		{
		}

		Close(error.message());
		return;
	}

	StartReceive(); //继续下一次数据接收
	
	std::deque<std::string> received_messages;
	received_messages.swap(_receive_list);

	//数据处理
	while (!IsClosed() && !received_messages.empty())
	{
		const std::string& message = received_messages.front();
		OnReceived(message);
		received_messages.pop_front();
	}
}

}
