#include "CenterSession.h"
#include "RedisManager.h"
#include "MXLog.h"
#include "Player.h"
#include "Room.h"
#include "Config.h"
#include "Timer.h"
#include "Activity.h"
#include "Protocol.h"

namespace Adoter
{

CenterSession::CenterSession(boost::asio::io_service& io_service, const boost::asio::ip::tcp::endpoint& endpoint) : 
	ClientSocket(io_service, endpoint)
{
	_remote_endpoint = endpoint;
	_ip_address = endpoint.address().to_string();
}
	
void CenterSession::OnConnected()
{
	//DEBUG("游戏逻辑服务器连接到中心服务器[{} {}]成功", _ip_address, _remote_endpoint.port());

	Asset::RegisterServer message;
	message.set_role_type(Asset::ROLE_TYPE_GAME_SERVER);
	message.set_global_id(ConfigInstance.GetInt("ServerID", 1)); //全球唯一

	SendProtocol(message); //注册服务器角色
}

void CenterSession::OnReceived(const Asset::Meta& message)
{
	InnerProcess(message);

	DEBUG("Receive message:{} from server:{}", message.ShortDebugString(), _ip_address);
}

bool CenterSession::InnerProcess(const Asset::Meta& meta)
{
	switch (meta.type_t())
	{
		case Asset::META_TYPE_S2S_REGISTER: //注册服务器成功
		{
			DEBUG("游戏逻辑服务器注册到中心服成功.");
		}
		break;

		case Asset::META_TYPE_SHARE_ENTER_ROOM:
		{
			Asset::EnterRoom message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			auto player = PlayerInstance.Get(meta.player_id());
			if (!player) player = std::make_shared<Player>(meta.player_id());

			if (player->OnEnterGame()) 
			{
				ERROR("玩家{}进入游戏失败", meta.player_id());
			}
			player->CmdEnterRoom(&message);
		}
		break;

		default:
		{
			WARN("Receive message:{} from server has no process type:{}", meta.ShortDebugString(), meta.type_t());
			std::shared_ptr<Player> player = PlayerInstance.Get(meta.player_id());
			if (!player) 
			{
				WARN("游戏逻辑服务器未能找到玩家:{}", meta.player_id());
				player = std::make_shared<Player>(meta.player_id());
				if (player->OnEnterGame()) 
				{
					ERROR("玩家{}进入游戏失败", meta.player_id());
				}
				return false;
			}

			pb::Message* msg = ProtocolInstance.GetMessage(meta.type_t());	
			if (!msg) 
			{
				TRACE("Could not found message of type:%d", meta.type_t());
				return false;		//非法协议
			}

			auto message = msg->New();
			
			auto result = message->ParseFromArray(meta.stuff().c_str(), meta.stuff().size());
			if (!result) 
			{
				DEBUG_ASSERT(false);
				return false;		//非法协议
			}
			player->HandleProtocol(meta.type_t(), message);
		}
		break;
	}
	return true;
}
	
void CenterSession::SendProtocol(pb::Message* message)
{
	SendProtocol(*message);
}

void CenterSession::SendProtocol(pb::Message& message)
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
		ERROR("server:{} send nothing, message:{}", _ip_address, meta.ShortDebugString());
		return;
	}

	DEBUG("游戏逻辑服务器发送协议到中心服务器[{} {}], 协议数据:{}", _ip_address, _remote_endpoint.port(), meta.ShortDebugString());
	AsyncSendMessage(content);
}

bool CenterSession::StartSend()
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

void CenterSession::AsyncSendMessage(std::string message)
{
	if (IsClosed()) return;

	_send_list.push_back(message);

	StartSend();
}

void CenterSession::OnWriteSome(const boost::system::error_code& error, std::size_t bytes_transferred)
{
	if (!IsConnected()) return;

	if (error)
	{
		Close(error.message());
		CRITICAL("server:{} send success, bytes_transferred:{}, error:{}", _ip_address, bytes_transferred, error.message());
		return;
	}

	DEBUG("游戏逻辑服务器发送数据到中心服务器[{} {}], 长度:{}, 返回码:{}", _ip_address, _remote_endpoint.port(), bytes_transferred, error.message());
}

bool CenterSession::StartReceive()
{
	if (!IsConnected()) return false;

	AsynyReadSome();
	return true;
}

void CenterSession::OnReadSome(const boost::system::error_code& error, std::size_t bytes_transferred)
{
	if (!IsConnected()) return;
		
	if (error)
	{
		Close(error.message());
		return;
	}
	
	DEBUG("接收中心服务器[{} {}]数据, 长度:{}, 返回码:{}", _ip_address, _remote_endpoint.port(), bytes_transferred, error.message());

	Asset::Meta meta;
	auto result = meta.ParseFromArray(_buffer.data() + 2, bytes_transferred - 2);
	if (!result)
	{
		LOG(ERROR, "Receive message error from server:{} {} cannot parse from data.", _ip_address, _remote_endpoint.port());
		ERROR("接收中心服务器[{} {}]的协议数据, 长度:{}, 不能正确转换为Protobuff数据", _ip_address, _remote_endpoint.port(), bytes_transferred);
		return;
	}

	_receive_list.push_back(meta);

	std::deque<Asset::Meta> received_messages;
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

#undef RETURN
}
