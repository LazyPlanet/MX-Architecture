
#include "ClientSession.h"

namespace Adoter
{


ClientSession::ClientSession(boost::asio::io_service& io_service, const boost::asio::ip::tcp::endpoint& endpoint) : 
	ClientSocket(io_service, endpoint)
{

}

}
