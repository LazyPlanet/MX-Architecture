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
    
	virtual void OnConnected(); //连接上服务器
	virtual void OnReceived(const std::string& message); //处理服务器的数据

    virtual bool StartReceive()
    {
        return TryStartReceive();
    }
    
	bool TryStartReceive()
    {
        if (!IsConnected()) return false;

		AsynyReadSome();
		return true;
    }
    
    virtual bool StartSend()
    {
        return TryStartSend();
    }
    
	bool TryStartSend()
    {
        bool started = false;

        while (IsConnected())
        {
			if (_send_list.size())
			{
				const auto& message = _send_list.front();
				AsyncSendMessage(message);

				started = true;
			}
			else
			{
				break;
			}
        }
        return started;
    }
    
	void AsyncSendMessage(const std::string& message)
    {
        if (IsClosed()) return;

		_send_list.push_back(message);

        TryStartSend();
    }
    
    virtual void OnReadSome(const boost::system::error_code& error, std::size_t bytes_transferred)
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

        TryStartReceive(); //继续下一次数据接收
        
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

    virtual void OnWriteSome(const boost::system::error_code& error, std::size_t bytes_transferred)
    {
        if (!IsConnected()) return;

        if (error)
        {
            Close(error.message());
            return;
        }

		std::deque<std::string> send_messages;
		send_messages.swap(_send_list);
		
		if (!IsClosed() && !send_messages.empty())
		{
            const std::string& message = send_messages.front();
			AsyncWriteSome(message.c_str(), message.size());
            send_messages.pop_front();
		}
		else
		{
			TryStartSend();
		}
    }
            
private:
	std::deque<std::string> _send_list;
	std::deque<std::string> _receive_list;
	boost::asio::ip::tcp::endpoint _remote_endpoint;
	std::string _ip_address;
};

}
