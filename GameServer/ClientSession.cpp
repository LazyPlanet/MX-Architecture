#include "ClientSession.h"
#include "RedisManager.h"
#include "MXLog.h"
#include "Player.h"
#include "Room.h"
#include "Config.h"

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
	DEBUG("Connected server:{} success.", _ip_address);

	Asset::Register message;
	message.set_server_type(Asset::SERVER_TYPE_GAME);
	message.set_server_id(ConfigInstance.GetInt("ServerID", 1)); //服务器ID，全球唯一

	SendProtocol(message); //注册服务器角色
}

void ClientSession::OnReceived(const Asset::InnerMeta& message)
{
	InnerProcess(message);
	LOG(INFO, "Receive message:{} from server:{}", message.ShortDebugString(), _ip_address);
}

bool ClientSession::InnerProcess(const Asset::InnerMeta& meta)
{
	LOG(INFO, "Receive message:{} from server:{}", meta.ShortDebugString(), _ip_address);

	switch (meta.type_t())
	{
		case Asset::INNER_TYPE_REGISTER: //注册服务器成功
		{
			TRACE("Register gameserver to gmtserver success.");
		}
		break;

		case Asset::INNER_TYPE_COMMAND: //GMT指令
		{
			Asset::Command message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			OnCommandProcess(message);
		}
		break;

		case Asset::INNER_TYPE_OPEN_ROOM: //代开房
		{
			Asset::OpenRoom message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			//获取策划好友房数据
			const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_ROOM);
			auto it = std::find_if(messages.begin(), messages.end(), [](pb::Message* message){
					auto room_limit = dynamic_cast<Asset::RoomLimit*>(message);
					if (!room_limit) return false;
					return Asset::ROOM_TYPE_FRIEND == room_limit->room_type();
				});
			if (it == messages.end()) return false;

			auto room_limit = dynamic_cast<Asset::RoomLimit*>(*it);
			if (!room_limit) return Asset::ERROR_ROOM_TYPE_NOT_FOUND;

			//房间属性
			Asset::Room room;
			room.set_room_type(Asset::ROOM_TYPE_FRIEND);
			room.mutable_options()->CopyFrom(room_limit->room_options());

			auto room_ptr = RoomInstance.CreateRoom(room);
			if (!room_ptr) return false; //未能创建成功房间，理论不会出现

			message.set_room_id(room_ptr->GetID());
			message.set_error_code(Asset::COMMAND_ERROR_CODE_SUCCESS); //成功创建
			SendProtocol(message); //发送给GTM服务器
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
	if (x) { \
		LOG(ERR, "command excute failed for:{} command:{}", x, command.ShortDebugString()); \
	} else { \
		LOG(TRACE, "command excute success for:{} command:{}", x, command.ShortDebugString()); \
	} \
	SendProtocol(response); \
	return x; \

	auto redis = make_unique<Redis>();

	/*
	Asset::User user; //账号数据
	auto result = user.ParseFromString(redis->GetUser(command.account()));
	if (!result)
	{
		RETURN(Asset::COMMAND_ERROR_CODE_NO_ACCOUNT);
	}
	*/

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
	
	/*
	auto it = std::find(user.player_list().begin(), user.player_list().end(), player_id);
	if (it == user.player_list().end()) 
	{
		RETURN(Asset::COMMAND_ERROR_CODE_NO_PLAYER); //账号下不存在该角色
	}
	*/

	auto player_ptr = PlayerInstance.Get(player_id);
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
			player_ptr->GainDiamond(command.count());
		}
		break;
		
		case Asset::COMMAND_TYPE_ROOM_CARD:
		{
			player_ptr->GainRoomCard(command.count());   
		}
		break;
		
		case Asset::COMMAND_TYPE_HUANLEDOU:
		{
			player_ptr->GainHuanledou(command.count());
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
		Close(error.message());
		return;
	}
	
	DEBUG("Receive message from server:{} bytes_transferred:{} error:{}", _ip_address, bytes_transferred, error.message());

	Asset::InnerMeta meta;
	auto result = meta.ParseFromArray(_buffer.data(), bytes_transferred);
	if (!result)
	{
		LOG(ERROR, "Receive message error from server:{} cannot parse from data.", _ip_address);
		return;
	}

	_receive_list.push_back(meta);

	std::deque<Asset::InnerMeta> received_messages;
	received_messages.swap(_receive_list);

	//数据处理
	while (!IsClosed() && !received_messages.empty())
	{
		const auto& message = received_messages.front();
		OnReceived(message);
		received_messages.pop_front();
	}
	
	AsynyReadSome(); //继续下一次数据接收
}

}
