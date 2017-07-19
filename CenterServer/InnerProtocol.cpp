#include <spdlog/spdlog.h>

#include "WorldSession.h"
#include "MXLog.h"
#include "Player.h"
#include "GmtSession.h"

namespace Adoter
{

namespace spd = spdlog;

extern std::shared_ptr<GmtSession> g_gmt_client;

bool WorldSession::OnInnerProcess(const Asset::Meta& meta)
{
	DEBUG("接收逻辑服务器数据:{}", meta.ShortDebugString());

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
		
		case Asset::META_TYPE_S2S_GMT_INNER_META: //GMT命令
		{
			Asset::GmtInnerMeta message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			if (!g_gmt_client) return false;

			g_gmt_client->SendInnerMeta(message.inner_meta());
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
