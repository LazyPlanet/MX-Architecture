#include <spdlog/spdlog.h>

#include "WorldSession.h"
#include "MXLog.h"

namespace Adoter
{

namespace spd = spdlog;

bool WorldSession::OnInnerProcess(const Asset::Meta& meta)
{
	//DEBUG("接收逻辑服务器数据:{}", meta.type_t());

	switch (meta.type_t())
	{
		case Asset::META_TYPE_S2S_REGISTER: //注册服务器
		{
			Asset::RegisterServer message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			SetID(message.global_id());
			SetRoleType(Asset::ROLE_TYPE_GAME_SERVER);
			WorldSessionInstance.AddServer(message.global_id(), shared_from_this());

			SendProtocol(message);
		}
		break;
		
		default:
		{
			WARN("接收逻辑服务器协议:{}, 类型:{}, 直接进行转发", meta.ShortDebugString(), meta.type_t());
			auto player_session = WorldSessionInstance.GetPlayerSession(meta.player_id());

			if (!player_session) return false;
			player_session->SendMeta(meta);
		}
		break;
	}
	return true;
}

}
