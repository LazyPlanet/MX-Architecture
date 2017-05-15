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

using boost::asio::ip::tcp;
using namespace boost::asio;

template<class T, class S = boost::asio::ip::tcp::socket>
class Socket : public std::enable_shared_from_this<T>
{
public:
	S _socket; 
public:
	explicit Socket(boost::asio::ip::tcp::socket&& socket) : _socket(std::move(socket)), _closed(false), _closing(false) { }

public:
    void AsyncConnect()
    {
        _last_rw_ticks = _ticks;

        _status = STATUS_CONNECTING;
        _socket.async_connect(_remote_endpoint,
                boost::bind(&RpcByteStream::OnConnect, shared_from_this(), _1));
        if (_connect_timeout > 0) 
        {
            _timer.expires_from_now(boost::posix_time::milliseconds(_connect_timeout));
            _timer.async_wait(boost::bind(&RpcByteStream::OnConnectTimeout, shared_from_this(), _1));
        }
    }
    
	void OnConnectTimeout(const boost::system::error_code& error) 
    {
        if (_status != STATUS_CONNECTING) 
        {
            return;
        }
        if (error == boost::asio::error::operation_aborted) 
        {
            return;
        }
        Close("connect timeout");
    }
    
    void Close(const std::string& reason)
    {
        if (atomic_swap(&_status, (int)STATUS_CLOSED) != STATUS_CLOSED)
        {
            snprintf(_error_message, sizeof(_error_message), "%s", reason.c_str());
            boost::system::error_code ec;
            _socket.shutdown(tcp::socket::shutdown_both, ec);
            OnClose();
            if (_remote_endpoint != RpcEndpoint())
            {
            }
        }
    }
protected:
    void AsynyReadSome()
    {
        _socket.async_read_some(boost::asio::buffer(_buffer),
                boost::bind(&Socket::OnReadSome, shared_from_this(), _1, _2));
    }

    void AsyncWriteSome(const char* data, size_t size)
    {
        _socket.async_write_some(boost::asio::buffer(data, size),
                boost::bind(&Socket::OnWriteSome, shared_from_this(), _1, _2));
    }

    virtual bool OnConnected() = 0;

    virtual void OnClose() = 0;

    virtual void OnReadSome(const boost::system::error_code& error, std::size_t bytes_transferred) = 0;
    virtual void OnWriteSome(const boost::system::error_code& error, std::size_t bytes_transferred) = 0;

protected:
    //RpcEndpoint _local_endpoint;
    //RpcEndpoint _remote_endpoint;
    volatile int64_t _ticks;
    volatile int64_t _last_rw_ticks;
	
private:
	//接收缓存
	std::array<unsigned char, 4096> _buffer;
    
	deadline_timer _timer;
    int64_t _connect_timeout;

    enum {
        STATUS_INIT       = 0,
        STATUS_CONNECTING = 1,
        STATUS_CONNECTED  = 2,
        STATUS_CLOSED     = 3,
    };
    volatile int _status;
    
private:
	void OnConnect(const boost::system::error_code& error)
    {
        if (_status != STATUS_CONNECTING) return;

        if (error)
        {
            Close("init stream failed: " + error.message());
            return;
        }

        boost::system::error_code ec;
        _socket.set_option(tcp::no_delay(SOFA_PBRPC_TCP_NO_DELAY), ec);
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

        if (!OnConnected())
        {
            Close("init stream failed: call OnConnected() failed");
            return;
        }

        _status = STATUS_CONNECTED;
        _timer.cancel();

        StartReceive();
        StartSend();
    }
    
    bool IsConnecting() const { return _status == STATUS_CONNECTING; }

    bool IsConnected() const { return _status == STATUS_CONNECTED; }

    bool IsClosed() const { return _status == STATUS_CLOSED; }
};	

}
