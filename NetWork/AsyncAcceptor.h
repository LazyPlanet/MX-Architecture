#pragma once

#include <atomic>
#include <functional>

#include <boost/asio.hpp>

using boost::asio::ip::tcp;

class AsyncAcceptor
{
public:
	typedef void(*AcceptCallback)(tcp::socket&& socket, int32_t thread_index);

	AsyncAcceptor(boost::asio::io_service& io_service, const std::string& bind_ip, int32_t port) :
	_acceptor(io_service, tcp::endpoint(boost::asio::ip::address::from_string(bind_ip), port)),
		_socket(io_service), _closed(false), _socket_factory(std::bind(&AsyncAcceptor::DefeaultSocketFactory, this))
	{
	}

	template<class T> void AsyncAccept();

	template<AcceptCallback accept_callback> void AsyncAcceptWithCallback();

	void Close()
	{
        try
        {
			if (_closed.exchange(true)) return;

			boost::system::error_code err;
			_acceptor.close(err);
		}
		catch (const boost::system::system_error& err)
		{
			spdlog::get("console")->warn("{0} Line:{1} error:{2}", __func__, __LINE__, err.what());
		}
	}

    void SetSocketFactory(std::function<std::pair<tcp::socket*, int32_t>()> func) { _socket_factory = func; }

private:
	std::pair<tcp::socket*, int32_t> DefeaultSocketFactory() { return std::make_pair(&_socket, 0); }

	tcp::acceptor _acceptor;
	tcp::socket _socket;
	std::atomic<bool> _closed;
	std::function<std::pair<tcp::socket*, int32_t>()> _socket_factory;
};

template<class T> 
void AsyncAcceptor::AsyncAccept()
{
   	_acceptor.async_accept(_socket, [this](boost::system::error_code error)
    	{
        	if (!error)
        	{
            		try
            		{
                		std::make_shared<T>(std::move(this->_socket))->Start();
            		}
            		catch (const boost::system::system_error& err)
            		{
						spdlog::get("console")->warn("{0} Line:{1} error:{2}", __func__, __LINE__, err.what());
            		}
        	}

        	if (!_closed) this->AsyncAccept<T>();
    	});
}

template<AsyncAcceptor::AcceptCallback accept_callback> 
void AsyncAcceptor::AsyncAcceptWithCallback()
{
	tcp::socket* socket;
	int32_t thread_index;
	std::tie(socket, thread_index) = _socket_factory();
	_acceptor.async_accept(*socket, [this, socket, thread_index](boost::system::error_code error)
	{
		if (!error)
		{
			try
			{
				socket->non_blocking(true);

				accept_callback(std::move(*socket), thread_index);
			}
			catch (const boost::system::system_error& err)
			{
				spdlog::get("console")->warn("{0} Line:{1} error:{2}", __func__, __LINE__, err.what());
			}
		}

		if (!_closed) this->AsyncAcceptWithCallback<accept_callback>();
	});
}

