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
	auto debug_string = meta.ShortDebugString();

	DEBUG("接收逻辑服务器数据:{}", debug_string);

	switch (meta.type_t())
	{
		case Asset::META_TYPE_S2S_REGISTER: //注册服务器
		{
			Asset::RegisterServer message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			SetRoleType(Asset::ROLE_TYPE_GAME_SERVER, message.global_id());
			WorldSessionInstance.AddServer(message.global_id(), shared_from_this());
					
			LOG(INFO, "注册逻辑服务器:{}", message.global_id());

			SendProtocol(message);
		}
		break;
		
		case Asset::META_TYPE_S2S_GMT_INNER_META: //GMT命令
		{
			Asset::GmtInnerMeta message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			if (!g_gmt_client) return false;

			Asset::InnerMeta inner_meta;
			inner_meta.ParseFromString(message.inner_meta());
			g_gmt_client->SendInnerMeta(inner_meta);
		}
		break;
		
		case Asset::META_TYPE_S2S_KICKOUT_PLAYER: //退出游戏逻辑服务器
		{
			Asset::KickOutPlayer message;
			auto result = message.ParseFromString(meta.stuff());
			if (!result) return false;

			if (message.player_id() != meta.player_id()) return false;
			
			auto player = PlayerInstance.Get(meta.player_id());
			if (!player) return false;

			player->OnEnterCenter(); //进入游戏，初始化数据
		}
		break;
		
		default:
		{
			WARN("接收逻辑服务器协议:{}, 类型:{}, 直接进行转发", debug_string, meta.type_t());
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
