#include "ClientSession.h"
#include "MXLog.h"

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
