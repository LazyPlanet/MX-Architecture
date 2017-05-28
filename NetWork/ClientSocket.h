#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <iostream>
#include <queue>
#include <unordered_map>
#include <sstream>

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

namespace Adoter 
{

class ClientSocket : public std::enable_shared_from_this<ClientSocket>
{
public:
	ClientSocket(boost::asio::io_service& io_service, const boost::asio::ip::tcp::endpoint& endpoint) : 
		_timer(io_service), _socket(io_service), _remote_endpoint(endpoint)
	{

	}

    virtual void AsyncConnect()
    {
        _status = STATUS_CONNECTING;

        _socket.async_connect(_remote_endpoint, boost::bind(&ClientSocket::OnConnect, shared_from_this(), _1));

        if (_connect_timeout > 0) 
        {
            _timer.expires_from_now(boost::posix_time::milliseconds(_connect_timeout));
            _timer.async_wait(boost::bind(&ClientSocket::OnConnectTimeout, shared_from_this(), _1));
        }
    }
    
	virtual void OnConnectTimeout(const boost::system::error_code& error) 
    {
        if (_status != STATUS_CONNECTING) return;

        if (error == boost::asio::error::operation_aborted) return;

        Close("connect timeout");
    }
    
    void Close(const std::string& reason)
    {
		boost::system::error_code error;
		_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);

		std::cout << __func__ << " Line:" << __LINE__ << " reason:" << reason << " error:" << error << std::endl;

		OnClose();
    }

    void AsynyReadSome()
    {
        _socket.async_read_some(boost::asio::buffer(_buffer), boost::bind(&ClientSocket::OnReadSome, shared_from_this(), _1, _2));
    }

    void AsyncWriteSome(const char* data, size_t size)
    {
        _socket.async_write_some(boost::asio::buffer(data, size), boost::bind(&ClientSocket::OnWriteSome, shared_from_this(), _1, _2));
    }

    virtual void OnClose() { }

    virtual void OnReadSome(const boost::system::error_code& error, std::size_t bytes_transferred) { }
	virtual void OnWriteSome(const boost::system::error_code& error, std::size_t bytes_transferred) { }

protected:
	boost::asio::deadline_timer _timer;
	boost::asio::ip::tcp::socket _socket; 
	boost::asio::ip::tcp::endpoint _local_endpoint;
	boost::asio::ip::tcp::endpoint _remote_endpoint;
    volatile int64_t _ticks = 0;
    volatile int64_t _last_rw_ticks = 0;
	
	//接收缓存
	std::array<unsigned char, 4096> _buffer;
    
    int64_t _connect_timeout = 10;

    enum {
        STATUS_INIT       = 0,
        STATUS_CONNECTING = 1,
        STATUS_CONNECTED  = 2,
        STATUS_CLOSED     = 3,
    };
    volatile int _status = STATUS_INIT;
    
	virtual void OnConnect(const boost::system::error_code& error)
    {
        if (_status != STATUS_CONNECTING) return;

        if (error)
        {
            Close("init stream failed: " + error.message());
            return;
        }

        boost::system::error_code ec;
        _socket.set_option(boost::asio::ip::tcp::no_delay(true), ec);

        if (ec)
        {
            Close("init stream failed: " + ec.message());
            return;
        }

        _local_endpoint = _socket.local_endpoint(ec);

        if (ec)
        {
            Close("init stream failed: " + ec.message());
            return;
        }

        _status = STATUS_CONNECTED;
        _timer.cancel();

        StartReceive(); //开始接收数据
        StartSend(); //开始发送数据
    }
    
	//网络状态查询
    bool IsConnecting() const { return _status == STATUS_CONNECTING; }
    bool IsConnected() const { return _status == STATUS_CONNECTED; }
    bool IsClosed() const { return _status == STATUS_CLOSED; }

	virtual bool StartSend() { return true; }
	virtual bool StartReceive() { return true; }
};	

}
