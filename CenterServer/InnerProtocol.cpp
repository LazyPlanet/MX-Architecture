#include <spdlog/spdlog.h>

#include "WorldSession.h"
#include "MXLog.h"
#include "Player.h"

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

			SetRoleType(Asset::ROLE_TYPE_GAME_SERVER, message.global_id());
			WorldSessionInstance.AddServer(message.global_id(), shared_from_this());

			SendProtocol(message);
		}
		break;
		
		default:
		{
			WARN("接收逻辑服务器协议:{}, 类型:{}, 直接进行转发", meta.ShortDebugString(), meta.type_t());
			auto player = PlayerInstance.Get(meta.player_id());

			if (!player) 
			{
				ERROR("未能找到玩家:{}", meta.player_id());
				return false;
			}
			player->SendMeta(meta);
		}
		break;
	}
	return true;
}

}
