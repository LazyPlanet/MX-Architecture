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
		case Asset::INNER_TYPE_SEND_MAIL: //发送邮件
		{
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
