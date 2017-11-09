#include "ServerSession.h"
#include "RedisManager.h"
#include "MXLog.h"
#include "Asset.h"
#include "Timer.h"

namespace Adoter
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

ServerSession::ServerSession(boost::asio::ip::tcp::socket&& socket) : Socket(std::move(socket))
{
	_remote_endpoint = _socket.remote_endpoint();
	_ip_address = _remote_endpoint.address().to_string();
}

void ServerSession::InitializeHandler(const boost::system::error_code error, const std::size_t bytes_transferred)
{
	try
	{
		if (error)
		{
			ERROR("远程断开与GMT服务器连接, 地址:{} 端口:{}，错误码:{} 错误描述:{}", _ip_address, _remote_endpoint.port(), error.value(), error.message());

			Close();
			return;
		}
		else
		{
			for (size_t index = 0; index < bytes_transferred;)
			{
				unsigned short body_size = _buffer[index] * 256 + _buffer[1 + index];
					
				//DEBUG("解析的头:{} {} 包长:{}", (int)_buffer[index] * 256, (int)_buffer[1 + index], body_size);

				if (body_size > MAX_DATA_SIZE)
				{
					LOG(ERROR, "接收来自地址:{} 端口:{} 太大的包长:{} 丢弃.", _ip_address, _remote_endpoint.port(), body_size)
					return;
				}

				char buffer[MAX_DATA_SIZE] = { 0 }; //数据缓存  
				for (size_t i = 0; i < body_size; ++i) buffer[i] = _buffer[i + index + 2];  

				Asset::InnerMeta meta;
				auto result = meta.ParseFromArray(buffer, body_size);
				if (!result) 
				{
					LOG(ERROR, "Receive message error from server:{}", _ip_address);
					return;
				}
				
				OnInnerProcess(meta);

				index += (body_size + 2); //下个包的起始位置
			}
		}
	}
	catch (std::exception& e)
	{
		ERROR("远程连接断开与GMT服务器连接，IP地址:{}, 错误信息:{}", _ip_address, e.what());

		Close();
		return;
	}
	//递归持续接收	
	AsyncReceiveWithCallback(&ServerSession::InitializeHandler);
}

//
// 处理来自GMT和游戏服务器的的消息数据
//
bool ServerSession::OnInnerProcess(const Asset::InnerMeta& meta)
{
	DEBUG("接收协议数据:{} 来自地址:{}", meta.ShortDebugString(), _ip_address);

	switch (meta.type_t())
	{
		case Asset::INNER_TYPE_REGISTER: //注册服务器
		{
			Asset::Register message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;
			
			DEBUG("GMT服务器接收其他服务器的注册:{} ip_address:{}", message.ShortDebugString(), _ip_address);

			if (message.server_type() == Asset::SERVER_TYPE_GMT) //GMT服务器
			{
				ServerSessionInstance.SetGmtServer(shared_from_this());
			}
			else if (message.server_type() == Asset::SERVER_TYPE_GAME) //游戏服务器
			{
				ServerSessionInstance.Add(message.server_id(), shared_from_this());
			}

			_server_id = message.server_id();

			SendProtocol(message);
		}
		break;
		
		case Asset::INNER_TYPE_COMMAND: //发放钻石//房卡//欢乐豆
		{
			Asset::Command message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			std::string gmt_server_address;
			auto gmt_server = ServerSessionInstance.GetGmtServer();
			if (gmt_server) gmt_server_address = gmt_server->GetRemoteAddress();

			DEBUG("收到命令:{} 来自服务器:{} GMT服务器:{}", message.ShortDebugString(), _ip_address, gmt_server_address);

			if (ServerSessionInstance.IsGmtServer(shared_from_this())) //处理GMT服务器发送的数据
			{
				auto error_code = OnCommandProcess(message); //处理离线玩家的指令执行
				if (Asset::COMMAND_ERROR_CODE_PLAYER_ONLINE == error_code) 
				{
					DEBUG("玩家{}当前在线，发往游戏服务器进行处理", message.player_id());
					ServerSessionInstance.BroadCastProtocol(message); //处理在线玩家的指令执行
				}

				if (Asset::COMMAND_ERROR_CODE_SUCCESS == error_code)
				{
					LOG(INFO, "服务器:{} 处理指令数据成功:{}", _ip_address, message.ShortDebugString());
				}
				else if (Asset::COMMAND_ERROR_CODE_PLAYER_ONLINE != error_code) //在线
				{
					LOG(ERROR, "Server:{} server send gmt message:{} error_code:{}", _ip_address, message.ShortDebugString(), error_code);
				}
			}
			else //处理游戏服务器发送的数据
			{
				auto gmt_server = ServerSessionInstance.GetGmtServer();
				if (!gmt_server) return false;
			
				gmt_server->SendProtocol(message);
			}
		}
		break;

		case Asset::INNER_TYPE_OPEN_ROOM: //代开房
		{
			Asset::OpenRoom message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			LOG(INFO, "Receive command:{} from server:{}", message.ShortDebugString(), _ip_address);

			if (ServerSessionInstance.IsGmtServer(shared_from_this())) //处理GMT服务器发送的数据
			{
				auto game_server = ServerSessionInstance.Get(message.server_id());
				if (!game_server) 
				{
					message.set_error_code(Asset::COMMAND_ERROR_CODE_SERVER_NOT_FOUND);
					SendProtocol(message); //返回给GMT服务器
				}
				else
				{
					game_server->SendProtocol(message);
				}
			}
			else //处理游戏服务器返回的数据
			{
				auto gmt_server = ServerSessionInstance.GetGmtServer();
				if (!gmt_server) return false;
			
				gmt_server->SendProtocol(message);
			}
		}
		break;

		case Asset::INNER_TYPE_SEND_MAIL: //发送邮件
		{
			Asset::SendMail message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			if (ServerSessionInstance.IsGmtServer(shared_from_this())) //处理GMT服务器发送的数据
			{
				OnSendMail(message);
			}
			else //处理游戏服务器返回的数据
			{
				auto gmt_server = ServerSessionInstance.GetGmtServer();
				if (!gmt_server) return false;
			
				gmt_server->SendProtocol(message);
			}
		}
		break;
		
		case Asset::INNER_TYPE_SYSTEM_BROADCAST: //系统广播
		{
			Asset::SystemBroadcast message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;
	
			DEBUG("接收GMT跑马灯:{} 来自地址:{}", message.ShortDebugString(), _ip_address);

			if (!ServerSessionInstance.IsGmtServer(shared_from_this())) return false; //处理GMT服务器发送的数据
				
			OnSystemBroadcast(message);
		}
		break;
		
		case Asset::INNER_TYPE_ACTIVITY_CONTROL: //活动控制
		{
			Asset::ActivityControl message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			LOG(TRACE, "Receive command:{} from server:{}", message.ShortDebugString(), _ip_address);

			if (ServerSessionInstance.IsGmtServer(shared_from_this())) //处理GMT服务器发送的数据
			{
				OnActivityControl(message);
			}
		}
		break;

		case Asset::INNER_TYPE_QUERY_PLAYER:
		{
			Asset::QueryPlayer message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;
	
			auto redis = make_unique<Redis>();

			Asset::Player player; //玩家数据
			auto success = redis->GetPlayer(message.player_id(), player);
			if (!success) message.set_error_code(Asset::COMMAND_ERROR_CODE_NO_PLAYER);
			else message.set_common_prop(player.common_prop().SerializeAsString());

			message.set_error_code(Asset::COMMAND_ERROR_CODE_SUCCESS);

			SendProtocol(message);
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
	
Asset::COMMAND_ERROR_CODE ServerSession::OnActivityControl(const Asset::ActivityControl& command)
{
	ServerSessionInstance.BroadCastProtocol(command);

	RETURN(Asset::COMMAND_ERROR_CODE_SUCCESS); //成功执行
}
			
Asset::COMMAND_ERROR_CODE ServerSession::OnCommandProcess(const Asset::Command& command)
{
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
	
	/*
	auto it = std::find(user.player_list().begin(), user.player_list().end(), player_id);
	if (it == user.player_list().end()) 
	{
		RETURN(Asset::COMMAND_ERROR_CODE_NO_PLAYER); //账号下不存在该角色
	}
	*/

	Asset::Player player; //玩家数据

	auto success = redis->GetPlayer(player_id, player);
	if (!success)
	{
		RETURN(Asset::COMMAND_ERROR_CODE_NO_PLAYER); //没有角色数据
	}

	//
	// 玩家在线不做回发处理，直接发到游戏逻辑服务器进行处理
	//
	// 此处直接退出即可
	//
	if (player.logout_time() == 0 && player.login_time() != 0) //玩家在线不支持
	{
		return Asset::COMMAND_ERROR_CODE_PLAYER_ONLINE; //玩家目前在线
	}
		
	/*
	if (command.count() <= 0) //不可能是负数
	{
		RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
	}
	*/

	switch(command.command_type())
	{
		case Asset::COMMAND_TYPE_RECHARGE:
		{
			player.mutable_common_prop()->set_diamond(player.common_prop().diamond() + command.count());
			if (player.common_prop().diamond() < 0) player.mutable_common_prop()->set_diamond(0);
		}
		break;
		
		case Asset::COMMAND_TYPE_ROOM_CARD:
		{
			player.mutable_common_prop()->set_room_card_count(player.common_prop().room_card_count() + command.count());
			if (player.common_prop().room_card_count() < 0) player.mutable_common_prop()->set_room_card_count(0);

			/*
			auto global_id = command.item_id();
			const auto message = AssetInstance.Get(global_id); //例如：Asset::Item_Potion
			if (!message) //没找到物品
			{
				RETURN(Asset::COMMAND_ERROR_CODE_ITEM_NOT_FOUND); //物品未能找到
			}

			auto message_item = message->New(); 
			message_item->CopyFrom(*message);

			if (!message_item)
			{
				RETURN(Asset::COMMAND_ERROR_CODE_ITEM_NOT_FOUND); //物品未能找到
			}
			
			const pb::FieldDescriptor* prop_field = message_item->GetDescriptor()->FindFieldByName("item_common_prop"); //物品公共属性变量
			if (!prop_field) //不是物品
			{
				RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
			}

			try {
				const pb::Message& const_item_common_prop = message_item->GetReflection()->GetMessage(*message_item, prop_field);

				pb::Message& item_common_prop = const_cast<pb::Message&>(const_item_common_prop);
				auto& common_prop = dynamic_cast<Asset::Item_CommonProp&>(item_common_prop);

				auto inventory_type = common_prop.inventory(); //物品默认进包

				auto it_inventory = std::find_if(player.mutable_inventory()->mutable_inventory()->begin(), player.mutable_inventory()->mutable_inventory()->end(), 
						[inventory_type](const Asset::Inventory_Element& element){
					return inventory_type == element.inventory_type();		
				});

				if (it_inventory == player.mutable_inventory()->mutable_inventory()->end())
				{
					RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
				}

				auto inventory_items = it_inventory->mutable_items(); //包裹中物品数据

				if (!inventory_items)
				{
					RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
				}

				const pb::FieldDescriptor* type_field = message_item->GetDescriptor()->FindFieldByName("type_t");
				if (!type_field)
				{
					RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
				}

				auto type_t = type_field->default_value_enum()->number();

				auto it_item = inventory_items->begin(); //查找包裹中该物品数据
				for ( ; it_item != inventory_items->end(); ++it_item)
				{
					if (type_t == it_item->type_t())
					{
						auto item = message_item->New();
						item->ParseFromString(it_item->stuff()); //解析存盘数据

						const FieldDescriptor* item_prop_field = item->GetDescriptor()->FindFieldByName("item_common_prop");
						if (!item_prop_field) continue;

						const Message& item_prop_message = item->GetReflection()->GetMessage(*item, item_prop_field);
						prop_field = item_prop_message.GetDescriptor()->FindFieldByName("common_prop");
						if (!prop_field) continue;

						const Message& prop_message = item_prop_message.GetReflection()->GetMessage(item_prop_message, prop_field);
						const FieldDescriptor* global_id_field = prop_message.GetDescriptor()->FindFieldByName("global_id");
						if (!global_id_field) continue;

						auto item_global_id = prop_message.GetReflection()->GetInt64(prop_message, global_id_field);
						if (item_global_id == global_id) break; //TODO:不限制堆叠
					}
				}
				
				if (it_item == inventory_items->end()) //没有该类型物品
				{
					auto item_toadd = inventory_items->Add();
					item_toadd->set_type_t((Adoter::Asset::ASSET_TYPE)type_t);
					common_prop.set_count(command.count()); //Item_CommonProp
					item_toadd->set_stuff(message_item->SerializeAsString());
				}
				else
				{
					common_prop.set_count(common_prop.count() + command.count()); //Item_CommonProp
					it_item->set_stuff(message_item->SerializeAsString());
				}
			}
			catch (std::exception& e)
			{
				LOG(ERR, "const_cast or dynamic_cast exception:{}", e.what());	
				RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
			}
			*/
		}
		break;
		
		case Asset::COMMAND_TYPE_HUANLEDOU:
		{
			player.mutable_common_prop()->set_huanledou(player.common_prop().huanledou() + command.count());
			if (player.common_prop().huanledou() < 0) player.mutable_common_prop()->set_huanledou(0);
		}
		break;
		
		default:
		{
		}
		break;
	}

	//存盘
	redis->SavePlayer(player_id, player);

	RETURN(Asset::COMMAND_ERROR_CODE_SUCCESS); //成功执行
}

Asset::COMMAND_ERROR_CODE ServerSession::OnSendMail(const Asset::SendMail& command)
{
	const auto player_id = command.player_id(); 

	if (player_id != 0) //玩家定向邮件
	{
		Asset::Player player; //玩家数据

		auto redis = make_unique<Redis>();
		auto success = redis->GetPlayer(player_id, player);
		if (!success)
		{
			RETURN(Asset::COMMAND_ERROR_CODE_NO_PLAYER); //数据错误
		}

		if (player.logout_time() == 0 && player.login_time() != 0) //玩家目前在线
		{
			auto server_id = player.server_id(); //直接发给玩家所在服务器

			auto game_server = ServerSessionInstance.Get(server_id);
			if (!game_server) 
			{
				RETURN(Asset::COMMAND_ERROR_CODE_SERVER_NOT_FOUND); //未能找到服务器
			}
			else
			{
				game_server->SendProtocol(command);
			}
		}
		else
		{
			auto mail_id = command.mail_id();

			if (mail_id > 0)
			{
				player.mutable_mail_list_system()->Add(mail_id);
			}
			else
			{
				auto mail = player.mutable_mail_list_customized()->Add();
				mail->set_title(command.title());
				mail->set_content(command.content());
				mail->set_send_time(CommonTimerInstance.GetTime());

				//钻石
				auto attachment = mail->mutable_attachments()->Add();
				attachment->set_attachment_type(Asset::ATTACHMENT_TYPE_DIAMOND);
				attachment->set_count(command.diamond_count());
				
				//欢乐豆
				attachment = mail->mutable_attachments()->Add();
				attachment->set_attachment_type(Asset::ATTACHMENT_TYPE_HUANLEDOU);
				attachment->set_count(command.huanledou_count());
				
				//房卡
				attachment = mail->mutable_attachments()->Add();
				attachment->set_attachment_type(Asset::ATTACHMENT_TYPE_ROOM_CARD);
				attachment->set_count(command.room_card_count());
			}

			//存盘
			redis->SavePlayer(player_id, player);
		}
	}
	else //全服邮件
	{
		ServerSessionInstance.BroadCastProtocol(command);
	}
	
	RETURN(Asset::COMMAND_ERROR_CODE_SUCCESS); //成功执行
}
	
Asset::COMMAND_ERROR_CODE ServerSession::OnSystemBroadcast(const Asset::SystemBroadcast& command)
{
	DEBUG("GMT广播数据:{}", command.ShortDebugString());

	ServerSessionInstance.BroadCastProtocol(command);

	RETURN(Asset::COMMAND_ERROR_CODE_SUCCESS); //成功执行
}

void ServerSession::Start()
{
	AsyncReceiveWithCallback(&ServerSession::InitializeHandler);
}
	
bool ServerSession::Update() 
{ 
	return true;
}

void ServerSession::OnClose()
{
	ServerSessionInstance.Remove(_server_id);
}

void ServerSession::SendProtocol(const pb::Message* message)
{
	if (!message) return;
	SendProtocol(*message);
}

void ServerSession::SendProtocol(const pb::Message& message)
{
	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::INNER_TYPE_IsValid(type_t)) return;	//如果不合法，不检查会宕线
	
	Asset::InnerMeta meta;
	meta.set_type_t((Asset::INNER_TYPE)type_t);
	meta.set_stuff(message.SerializeAsString());

	std::string content = meta.SerializeAsString();
	if (content.empty()) return;

	DEBUG("GMT服务器向服务器ID:{} 地址:{} 发送协议数据:{} 具体内容:{}", _server_id, _ip_address, meta.ShortDebugString(), message.ShortDebugString());
	AsyncSend(content);
}
	
void ServerSessionManager::BroadCastProtocol(const pb::Message* message)
{
	if (!message) return;
	BroadCastProtocol(*message);
}

void ServerSessionManager::BroadCastProtocol(const pb::Message& message)
{
	std::lock_guard<std::mutex> lock(_server_mutex);

	for (auto session : _sessions)
	{
		if (!session.second) continue;
		session.second->SendProtocol(message);
	}
}

void ServerSessionManager::Add(int64_t server_id, std::shared_ptr<ServerSession> session)
{
	std::lock_guard<std::mutex> lock(_server_mutex);

	_sessions.emplace(server_id, session);
}	
	
void ServerSessionManager::Remove(int64_t server_id)
{
	std::lock_guard<std::mutex> lock(_server_mutex);

	auto it = _sessions.find(server_id);
	if (it == _sessions.end()) return;

	if (it->second) it->second.reset();
	_sessions.erase(it);
}

NetworkThread<ServerSession>* ServerSessionManager::CreateThreads() const
{    
	return new NetworkThread<ServerSession>[GetNetworkThreadCount()];
}

void ServerSessionManager::OnSocketAccept(tcp::socket&& socket, int32_t thread_index)
{    
	ServerSessionInstance.OnSocketOpen(std::forward<tcp::socket>(socket), thread_index);
}

bool ServerSessionManager::StartNetwork(boost::asio::io_service& io_service, const std::string& bind_ip, int32_t port, int thread_count)
{
	if (!SuperSocketManager::StartNetwork(io_service, bind_ip, port, thread_count)) return false;
	_acceptor->SetSocketFactory(std::bind(&SuperSocketManager::GetSocketForAccept, this));    
	_acceptor->AsyncAcceptWithCallback<&OnSocketAccept>();    
	return true;
}

#undef RETURN

}
