#include "WorldSession.h"

namespace Adoter
{

WorldSession::~WorldSession()
{
}

WorldSession::WorldSession(boost::asio::ip::tcp::socket&& socket) : Socket(std::move(socket))
{
}

void WorldSession::InitializeHandler(const boost::system::error_code error, const std::size_t bytes_transferred)
{
	try
	{
		if (error)
		{
			Close();
			return;
		}
		else
		{
			Asset::Meta meta;
			bool result = meta.ParseFromArray(_buffer.data(), bytes_transferred);

			if (!result) 
			{
				Close();
				return;		//非法协议
			}
			
			/////////////////////////////////////////////////////////////////////////////打印收到协议提示信息
		
			const pb::FieldDescriptor* type_field = meta.GetDescriptor()->FindFieldByName("type_t");
			if (!type_field) return;

			const pb::EnumValueDescriptor* enum_value = meta.GetReflection()->GetEnum(meta, type_field);
			if (!enum_value) return;

			const std::string& enum_name = enum_value->name();
			
			google::protobuf::Message* msg = ProtocolInstance.GetMessage(meta.type_t());	
			if (!msg) 
			{
				Close();
				return;		//非法协议
			}

			auto message = msg->New();
			
			//result = message->ParseFromString(meta.stuff());
			result = message->ParseFromArray(meta.stuff().c_str(), meta.stuff().size());
			if (!result) 
			{
				Close();
				return;		//非法协议
			}
		
			message->PrintDebugString(); //打印出来Message.

			/////////////////////////////////////////////////////////////////////////////游戏逻辑处理流程
			
			if (Asset::META_TYPE_C2S_LOGOUT == meta.type_t()) //账号登出
			{
				Asset::Logout* logout = dynamic_cast<Asset::Logout*>(message);
				if (!logout) return; 
			}
			else if (Asset::META_TYPE_SHARE_CREATE_PLAYER == meta.type_t()) //创建角色
			{
			}
			else
			{
				//g_player->HandleProtocol(meta.type_t(), message);
			}
		}
	}
	catch (std::exception& e)
	{
		return;
	}
	//递归持续接收	
	AsyncReceiveWithCallback(&WorldSession::InitializeHandler);
}

void WorldSession::Start()
{
	AsyncReceiveWithCallback(&WorldSession::InitializeHandler);
}
	
bool WorldSession::Update() 
{ 
	//g_player->Update(); 

	return true;
}

void WorldSession::OnClose()
{
}

void WorldSession::SendProtocol(pb::Message* message)
{
	SendProtocol(*message);
}

void WorldSession::SendProtocol(pb::Message& message)
{
	message.PrintDebugString(); //打印出来MESSAGE

	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::META_TYPE_IsValid(type_t)) return;	//如果不合法，不检查会宕线
	
	Asset::Meta meta;
	meta.set_type_t((Asset::META_TYPE)type_t);
	meta.set_stuff(message.SerializeAsString());

	std::string content = meta.SerializeAsString();
	AsyncSend(content);
}

void WorldSessionManager::Add(std::shared_ptr<WorldSession> session)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_list.push_back(session);
}	

size_t WorldSessionManager::GetCount()
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _list.size();
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
