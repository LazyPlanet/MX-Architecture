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
}

void ClientSession::OnReceived(const std::string& message)
{
}

}
