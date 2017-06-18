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
			WARN("Remote client disconnect, remote_ip:{}", _ip_address);
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
	AsyncReceiveWithCallback(&ServerSession::InitializeHandler);
}

//
// 处理来自GMT和游戏服务器的的消息数据
//
bool ServerSession::InnerProcess(const Asset::InnerMeta& meta)
{
	TRACE("Receive message:{} from server:{}", meta.ShortDebugString(), _ip_address);

	switch (meta.type_t())
	{
		case Asset::INNER_TYPE_REGISTER: //注册服务器
		{
			Asset::Register message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;
			
			TRACE("Receive server register:{} ip_address:{}", message.ShortDebugString(), _ip_address);

			if (message.server_type() == Asset::SERVER_TYPE_GMT) //GMT服务器
			{
				ServerSessionInstance.SetGmtServer(shared_from_this());
			}
			else if (message.server_type() == Asset::SERVER_TYPE_GAME) //游戏服务器
			{
				ServerSessionInstance.Add(message.server_id(), shared_from_this());
			}

			SendProtocol(message);
		}
		break;
		
		case Asset::INNER_TYPE_COMMAND: //发送指令
		{
			Asset::Command message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			std::string gmt_server_address;
			auto gmt_server = ServerSessionInstance.GetGmtServer();
			if (gmt_server) gmt_server_address = gmt_server->GetRemoteAddress();

			LOG(TRACE, "Receive command:{} from server:{} gmt_server:{}", message.ShortDebugString(), _ip_address, gmt_server_address);

			if (ServerSessionInstance.IsGmtServer(shared_from_this())) //处理GMT服务器发送的数据
			{
				auto error_code = OnCommandProcess(message); //处理离线玩家的指令执行
				if (Asset::COMMAND_ERROR_CODE_PLAYER_ONLINE == error_code) ServerSessionInstance.BroadCastProtocol(message); //处理在线玩家的指令执行

				if (Asset::COMMAND_ERROR_CODE_SUCCESS == error_code)
				{
					TRACE("Server:{} server send gmt message:{} error_code:{}", _ip_address, message.ShortDebugString(), error_code);
				}
				else if (Asset::COMMAND_ERROR_CODE_PLAYER_ONLINE != error_code) //在线
				{
					ERROR("Server:{} server send gmt message:{} error_code:{}", _ip_address, message.ShortDebugString(), error_code);
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

			LOG(TRACE, "Receive command:{} from server:{}", message.ShortDebugString(), _ip_address);

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

			OnSendMail(message);
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
		RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
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
			player.mutable_common_prop()->set_room_card_count(player.common_prop().room_card_count() + command.count());
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
			RETURN(Asset::COMMAND_ERROR_CODE_PARA); //数据错误
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

	if (content.empty()) 
	{
		ERROR("server:{} send nothing, message:{}", _ip_address, meta.ShortDebugString());
		return;
	}

	TRACE("send message to server:{} message:{}", _ip_address, meta.ShortDebugString());
	AsyncSend(content);
}
	
void ServerSessionManager::BroadCastProtocol(const pb::Message* message)
{
	if (!message) return;
	BroadCastProtocol(*message);
}

void ServerSessionManager::BroadCastProtocol(const pb::Message& message)
{
	for (auto session : _sessions)
	{
		if (!session.second) continue;
		session.second->SendProtocol(message);
	}
}

void ServerSessionManager::Add(int64_t server_id, std::shared_ptr<ServerSession> session)
{
	_sessions.emplace(server_id, session);
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
