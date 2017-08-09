#include "CenterSession.h"
#include "RedisManager.h"
#include "MXLog.h"
#include "Room.h"
#include "Config.h"
#include "Timer.h"
#include "Activity.h"
#include "Protocol.h"
#include "GmtSession.h"

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
	Asset::RegisterServer message;
	message.set_role_type(Asset::ROLE_TYPE_GAME_SERVER);
	message.set_global_id(ConfigInstance.GetInt("ServerID", 1)); //全球唯一

	SendProtocol(message); //注册服务器角色
}

bool CenterSession::OnMessageProcess(const Asset::Meta& meta)
{
	DEBUG("接收来自中心服务器:{} {}的数据:{}", _ip_address, _remote_endpoint.port(), meta.ShortDebugString());

	if (meta.type_t() == Asset::META_TYPE_S2S_REGISTER) //注册服务器成功
	{
		DEBUG("游戏逻辑服务器注册到中心服成功.");
	}
	else if (meta.type_t() == Asset::META_TYPE_S2S_GMT_INNER_META) //GMT命令
	{
		Asset::GmtInnerMeta message;
		auto result = message.ParseFromString(meta.stuff());
		if (!result) return false;

		Asset::InnerMeta inner_meta;
		inner_meta.ParseFromString(message.inner_meta());
		GmtInstance.InnerProcess(inner_meta);
	}
	else if (meta.type_t() == Asset::META_TYPE_SHARE_ENTER_ROOM) //进入游戏，从中心服务器首次进入逻辑服务器通过此处
	{
		Asset::EnterRoom message;
		auto result = message.ParseFromString(meta.stuff());
		if (!result) return false;

		auto player = _players[meta.player_id()];
		if (!player) player = std::make_shared<Player>(meta.player_id());
		_players[meta.player_id()] = player;

		if (player->OnLogin()) 
		{
			ERROR("玩家{}进入游戏失败", meta.player_id());
		}
		player->CmdEnterRoom(&message);
	}
	else
	{
		if (meta.player_id() == 0) return false;

		auto player = _players[meta.player_id()];
		if (!player) 
		{
			player = std::make_shared<Player>(meta.player_id());
			if (player->OnLogin()) 
			{
				ERROR("玩家{}进入游戏失败", meta.player_id());
			}
			_players[meta.player_id()] = player;
		}

		pb::Message* msg = ProtocolInstance.GetMessage(meta.type_t());	
		if (!msg) 
		{
			DEBUG("Could not found message of type:%d", meta.type_t());
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
	if (!Asset::META_TYPE_IsValid(type_t)) 
	{
		DEBUG_ASSERT(false);
		return;	//如果不合法，不检查会宕线
	}
	
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

	std::deque<std::string> send_list;
	send_list.swap(_send_list);

	while (IsConnected() && send_list.size())
	{
		const auto& message = send_list.front();
		AsyncWriteSome(message.c_str(), message.size());
		send_list.pop_front();

		started = true;
	}
	return started;
}

void CenterSession::OnWriteSome(const boost::system::error_code& error, std::size_t bytes_transferred)
{
	if (!IsConnected()) return;

	if (error)
	{
		Close(error.message());
		LOG(ERROR, "server:{} send success, bytes_transferred:{}, error:{}", _ip_address, bytes_transferred, error.message());
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
	if (!IsConnected()) 
	{
		LOG(ERROR, "接收来自地址:{} {} 的数据长度，此时网络已经断开.", _ip_address, _remote_endpoint.port(), bytes_transferred);
		return;
	}
		
	if (error)
	{
		Close(error.message());
		return;
	}
	
	//std::lock_guard<std::mutex> lock(_mutex);

	for (size_t index = 0; index < bytes_transferred;)
	{
		unsigned short body_size = _buffer[index] * 256 + _buffer[1 + index];
			
		if (body_size > MAX_DATA_SIZE)
		{
			LOG(ERROR, "接收来自地址:{} 端口:{} 太大的包长:{} 丢弃.", _ip_address, _remote_endpoint.port(), body_size)
			return;
		}

		char buffer[MAX_DATA_SIZE] = {0}; //数据缓存  
		for (size_t i = 0; i < body_size; ++i) buffer[i] = _buffer[i + index + 2]; //字节复制

		Asset::Meta meta;
		bool result = meta.ParseFromArray(buffer, body_size);

		if (!result)
		{
			LOG(ERROR, "Receive message error from server:{} {} cannot parse from data.", _ip_address, _remote_endpoint.port());
			return;
		}
		
		_receive_list.push_back(meta);

		index += (body_size + 2); //下个包的起始位置
	}

	std::deque<Asset::Meta> received_messages;
	received_messages.swap(_receive_list);

	//数据处理
	while (!IsClosed() && !received_messages.empty())
	{
		const auto& message = received_messages.front();
		OnMessageProcess(message);
		received_messages.pop_front();
	}
	
	AsynyReadSome(); //继续下一次数据接收
}

void CenterSession::SayHi()
{
	Asset::SayHi message;
	message.set_heart_count(_heart_count);
	SendProtocol(message);
}
	
//
//心跳周期50MS
//
bool CenterSession::Update()
{
	ClientSocket::Update();
	
	++_heart_count;
	
	if (_heart_count % 20 == 0) //1s
	{
		for (auto player : _players)
		{
			if (!player.second) continue;
			player.second->Update();
		}
	}

	if (_heart_count % 100 == 0) //5s
	{
		SayHi(); //心跳
	}

	return true;
}

#undef RETURN
}
