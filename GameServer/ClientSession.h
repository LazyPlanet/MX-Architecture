#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <iostream>
#include <deque>

#include <boost/asio.hpp>

#include "ClientSocket.h"
#include "P_Header.h"

namespace Adoter
{

using namespace google::protobuf;
namespace pb = google::protobuf;

class ClientSession : public ClientSocket
{
public:
	explicit ClientSession(boost::asio::io_service& io_service, const boost::asio::ip::tcp::endpoint& endpoint);

	ClientSession(ClientSession const& right) = delete;    
	ClientSession& operator = (ClientSession const& right) = delete;
	
	const boost::asio::ip::tcp::endpoint GetRemotePoint() { return _remote_endpoint; }
	bool InnerProcess(const Asset::InnerMeta& meta); //内部协议处理
	Asset::COMMAND_ERROR_CODE OnCommandProcess(const Asset::Command& command);
    
	virtual void OnConnected(); //连接上服务器
	virtual void OnReceived(const std::string& message); //处理服务器的数据
	
	void SendProtocol(pb::Message& message);
	void SendProtocol(pb::Message* message);

    virtual bool StartReceive();
    virtual bool StartSend();
    
    virtual void OnReadSome(const boost::system::error_code& error, std::size_t bytes_transferred);

	void AsyncSendMessage(std::string message);
    virtual void OnWriteSome(const boost::system::error_code& error, std::size_t bytes_transferred);  
private:
	std::deque<std::string> _send_list;
	std::deque<std::string> _receive_list;
	boost::asio::ip::tcp::endpoint _remote_endpoint;
	std::string _ip_address;
};

}
