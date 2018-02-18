#include <spdlog/spdlog.h>

#include "WorldSession.h"
#include "MXLog.h"
#include "Player.h"
#include "GmtSession.h"
#include "Protocol.h"
#include "Clan.h"

namespace Adoter
{

namespace spd = spdlog;

extern std::shared_ptr<GmtSession> g_gmt_client;

bool WorldSession::OnInnerProcess(const Asset::Meta& meta)
{
	pb::Message* msg = ProtocolInstance.GetMessage(meta.type_t());	
	if (!msg) return false;

	auto message = msg->New();

	defer {
		delete message;
		message = nullptr;
	};

	if (meta.stuff().size() == 0) return true;
	
	auto result = message->ParseFromArray(meta.stuff().c_str(), meta.stuff().size());
	if (!result) return false; 

	switch (meta.type_t())
	{
		case Asset::META_TYPE_S2S_REGISTER: //注册服务器
		{
			//Asset::RegisterServer message;
			//auto result = message.ParseFromString(meta.stuff());
			//if (!result) return false;

			const auto register_server = dynamic_cast<const Asset::RegisterServer*>(message);
			if (!register_server) return false;

			SetRoleType(Asset::ROLE_TYPE_GAME_SERVER, register_server->global_id());
			WorldSessionInstance.AddServer(register_server->global_id(), shared_from_this());
					
			DEBUG("注册逻辑服务器:{}成功", register_server->global_id());

			SendProtocol(message);
		}
		break;
		
		case Asset::META_TYPE_S2S_GMT_INNER_META: //GMT命令
		{
			//Asset::GmtInnerMeta message;
			//auto result = message.ParseFromString(meta.stuff());
			//if (!result) return false;
			
			const auto gmt_inner_meta = dynamic_cast<const Asset::GmtInnerMeta*>(message);
			if (!gmt_inner_meta) return false;

			if (!g_gmt_client) return false;

			Asset::InnerMeta inner_meta;
			inner_meta.ParseFromString(gmt_inner_meta->inner_meta());
			g_gmt_client->SendInnerMeta(inner_meta);
		}
		break;
		
		case Asset::META_TYPE_S2S_KICKOUT_PLAYER: //退出游戏逻辑服务器
		{
			//Asset::KickOutPlayer message;
			//auto result = message.ParseFromString(meta.stuff());
			//if (!result) return false;
			
			const auto kick_out = dynamic_cast<const Asset::KickOutPlayer*>(message);
			if (!kick_out) return false;

			if (kick_out->player_id() != meta.player_id()) return false;
			
			auto player = PlayerInstance.Get(meta.player_id());
			if (!player) return false;

			if (kick_out->reason() == Asset::KICK_OUT_REASON_LOGOUT) player->OnEnterCenter(); //进入游戏，初始化数据//正常退出逻辑服务器
		}
		break;

		case Asset::META_TYPE_S2S_CLAN_ROOM_START:
		{
			const auto clan_room = dynamic_cast<const Asset::ClanRoomStart*>(message);
			if (!clan_room) return false;

			auto clan = ClanInstance.Get(clan_room->room().clan_id());
			if (!clan) return false;

			clan->OnGameStart(clan_room);
		}
		break;
		
		default:
		{
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
