#include "ClientSession.h"
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
			Asset::InnerMeta meta;
			auto result = meta.ParseFromArray(_buffer.data(), bytes_transferred);
			if (result) InnerCommand(meta);
		}
	}
	catch (std::exception& e)
	{
		return;
	}
	//递归持续接收	
	AsyncReceiveWithCallback(&ClientSession::InitializeHandler);
}

bool ClientSession::InnerCommand(const Asset::InnerMeta& meta)
{
	switch (meta.type_t())
	{
		case Asset::INNER_TYPE_REGISTER: //注册服务器
		{
			Asset::Register message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

		}
		break;

		default:
		{
		}
		break;
	}
	return true;
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

void ClientSession::SendProtocol(pb::Message* message)
{
	SendProtocol(*message);
}

void ClientSession::SendProtocol(pb::Message& message)
{
	std::string content = message.SerializeAsString();
	AsyncSend(content);
}

void ClientSessionManager::Add(std::shared_ptr<ClientSession> session)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_list.push_back(session);
}	

size_t ClientSessionManager::GetCount()
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _list.size();
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
