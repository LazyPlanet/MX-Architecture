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

#include "MXLog.h"

namespace Adoter 
{

class ClientSocket : public std::enable_shared_from_this<ClientSocket>
{
public:
	ClientSocket(boost::asio::io_service& io_service, const boost::asio::ip::tcp::endpoint& endpoint) : 
		_timer(io_service), _socket(io_service), _remote_endpoint(endpoint), _closed(false), _closing(false)
	{

	}
	
	virtual bool Update() 
	{
		if (_closed) 
		{
			ERROR("{} has closed", _socket.remote_endpoint().address().to_string().c_str());
			return false;
		}

		//发送可以放到消息队列里面处理
		if (_is_writing_async || (_write_queue.empty() && !_closing)) 
		{
			return true;
		}

		for (; HandleQueue(); ) {}

		return true;
	}

    virtual void AsyncConnect()
    {
        _socket.async_connect(_remote_endpoint, std::bind(&ClientSocket::OnConnect, shared_from_this(), std::placeholders::_1));

        if (_connect_timeout > 0) 
        {
            _timer.expires_from_now(boost::posix_time::milliseconds(_connect_timeout));
            _timer.async_wait(std::bind(&ClientSocket::OnConnectTimeout, shared_from_this(), std::placeholders::_1));
        }
    }
    
	virtual void OnConnectTimeout(const boost::system::error_code& error) 
    {
        if (error == boost::asio::error::operation_aborted) return;

		ERROR("服务器内部连接超时失败，必须处理解决，错误码:{}", error.message());
		
        //Close("超时");
    }
    
    void Close(const std::string& reason)
    {
		boost::system::error_code error;
		_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);

		ERROR("服务器内部连接超时失败，必须处理解决，错误码:{} 原因:{}", error.message(), reason);

		OnClose();
    }

    void AsynyReadSome()
    {
        _socket.async_read_some(boost::asio::buffer(_buffer), std::bind(&ClientSocket::OnReadSome, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
    }

	void EnterQueue(std::string&& meta)
	{
		auto content = std::move(meta);
		//
		//数据包头
		//
		unsigned short body_size = content.size();
		if (body_size >= 4096) 
		{
			LOG(ERROR, "protocol has extend max size:{}", body_size);
			return;
		}

		unsigned char header[2] = { 0 };

		header[0] = (body_size >> 8) & 0xff;
		header[1] = body_size & 0xff;

		//
		//包数据体
		//
		auto body = content.c_str(); 

		//数据整理发送
		char buffer[4096] = { 0 }; //发送数据缓存
		for (int i = 0; i < 2; ++i) buffer[i] = header[i];
		for (int i = 0; i < body_size; ++i) buffer[i + 2] = body[i];

		_write_queue.push(std::string(buffer, body_size + 2));
	}
	
	void AsyncSendMessage(std::string meta)
	{
		EnterQueue(std::move(meta));
	}

    void AsyncWriteSome(const char* data, size_t size)
    {
		std::string meta(data, size);
		EnterQueue(std::move(meta));
    }
	
	bool AsyncProcessQueue()    
	{
		if (_is_writing_async) return false;
		_is_writing_async = true;

		_socket.async_write_some(boost::asio::null_buffers(), std::bind(&ClientSocket::OnWriteSome, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
		return false;
	}

	bool HandleQueue()
	{
		if (!_socket.is_open()) 
		{
			ERROR("网络已断开连接.");
			return false;
		}
		if (_write_queue.empty()) return false;

		std::string& meta = _write_queue.front();  //其实是META数据

		std::size_t bytes_to_send = meta.size();

		boost::system::error_code error;
		std::size_t bytes_sent = _socket.write_some(boost::asio::buffer(meta.c_str(), bytes_to_send), error);

		/*
		std::stringstream str;
		const char* c = meta.c_str();
		for (std::size_t i = 0; i < bytes_to_send; ++i)
		{
			str << (int)c[i] << " ";
		}
		DEBUG("游戏逻辑服务器发送数据:{}", str.str());
		*/

		if (error == boost::asio::error::would_block || error == boost::asio::error::try_again)
		{
			ERROR("bytes_to_send:{} bytes_sent:{}", bytes_to_send, bytes_sent);
			return AsyncProcessQueue();

			_write_queue.pop();

			if (_closing && _write_queue.empty()) Close("关闭");

			return false;
		}
		else if (bytes_sent == 0)
		{
			ERROR("bytes_to_send:{} bytes_sent:{} error:{}", bytes_to_send, bytes_sent, error.message());

			_write_queue.pop();

			if (_closing && _write_queue.empty()) Close("关闭");

			return false;
		}
		else if (bytes_sent < bytes_to_send) //一般不会出现这个情况，重新发送，记个ERROR
		{
			ERROR("bytes_to_send:{} bytes_sent:{}", bytes_to_send, bytes_sent);
			return AsyncProcessQueue();
		}

		DEBUG("client bytes_to_send:{} bytes_sent:{}", bytes_to_send, bytes_sent);
		_write_queue.pop();

		if (_closing && _write_queue.empty()) Close("关闭");

		return !_write_queue.empty();
	}

	virtual void DelayedClose() { _closing = true; } //发送队列为空时再进行关闭
	virtual bool IsConnected() { return _socket.is_open(); }
	virtual bool IsOpen() const { return !_closed && !_closing; }
	virtual bool IsClosed() const { return _closed || _closing; }

    virtual void OnClose() { }
    virtual void OnConnected() { }

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

	virtual void OnConnect(const boost::system::error_code& error)
    {
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

        _timer.cancel();

		OnConnected();

        StartReceive(); //开始接收数据
        StartSend(); //开始发送数据
    }
    
	virtual bool StartSend() { return true; }
	virtual bool StartReceive() { return true; }
private:
	std::atomic<bool> _closed;    
	std::atomic<bool> _closing;
	bool _is_writing_async = false;
	std::queue<std::string> _write_queue;
	std::mutex _mutex;
};	

}
