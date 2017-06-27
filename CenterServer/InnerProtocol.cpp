#include <spdlog/spdlog.h>

#include "WorldSession.h"
#include "MXLog.h"

namespace Adoter
{

namespace spd = spdlog;

bool WorldSession::OnInnerProcess(const Asset::Meta& meta)
{
	switch (meta.type_t())
	{
		case Asset::META_TYPE_S2S_REGISTER: //注册服务器
		{
			Asset::RegisterServer message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			SetID(message.server_id());
			SetRoleType(Asset::ROLE_TYPE_GAME_SERVER);
			WorldSessionInstance.AddServer(message.server_id(), shared_from_this());

			SendProtocol(message);
		}
		break;
		
		default:
		{
			WARN("Receive message:{} from server has no process type:{}", meta.ShortDebugString(), meta.type_t());
		}
		break;
	}
	return true;
}

}
