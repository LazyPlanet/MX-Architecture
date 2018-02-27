#include <chrono>

#include "PlayerMatch.h"
#include "Player.h"
#include "Room.h"
#include "Timer.h"
#include "MXLog.h"

namespace Adoter
{
	
void PlayerMatch::Update(int32_t diff)
{
	_scheduler.Update(diff);
}

void PlayerMatch::Join(std::shared_ptr<Player> player, pb::Message* message)
{
	if (!player) return;

	auto player_id = player->GetID();

	auto enter_room = dynamic_cast<Asset::EnterRoom*>(message);
	if (!enter_room) return;

	Asset::ROOM_TYPE room_type = enter_room->room().room_type();
	if (room_type == Asset::ROOM_TYPE_FRIEND) return; //好友房不能进入匹配
	
	auto& match_list = _match_list[room_type];

	if (enter_room->enter_type() == Asset::EnterRoom_ENTER_TYPE_ENTER_TYPE_ENTER)
	{
		match_list.emplace(player_id, player);
	}
	else
	{
		match_list.erase(player_id);
	}
}
	
void PlayerMatch::DoMatch()
{
	return; //朝阳关闭匹配

	DEBUG("匹配房开始匹配...");

	_scheduler.Schedule(std::chrono::seconds(3), [this](TaskContext task) {
			
		for (auto it = _match_list.begin(); it != _match_list.end(); ++it)
		{
			auto& player_list = it->second; 
	
			DEBUG("房间类型:{} 玩家数量:{}", it->first, it->second.size());
		
			if (player_list.size() < 4) continue;
				
			auto room_id = RoomInstance.AllocRoom();
			if (room_id <= 0) continue;

			Asset::Room room;
			room.set_room_id(room_id);
			room.set_room_type((Asset::ROOM_TYPE)it->first);

			auto local_room = RoomInstance.CreateRoom(room);
			if (!local_room) continue;

			Asset::EnterRoom enter_room;
			enter_room.mutable_room()->CopyFrom(room);
			enter_room.set_enter_type(Asset::EnterRoom_ENTER_TYPE_ENTER_TYPE_ENTER); //进入房间

			DEBUG("预创建房间数据:{}", enter_room.ShortDebugString());

			bool match_success = true;

			//
			//检查是否满足创建房间条件
			//
			for (auto it = player_list.begin(); it != std::next(player_list.begin(), 4); ++it)
			{
				auto ret = local_room->TryEnter(it->second); //玩家进入房间

				enter_room.set_error_code(ret); //是否可以进入场景//房间

				it->second->SendProtocol(enter_room); //提示Client是否成功

				if (Asset::ERROR_SUCCESS != ret) 
				{
					match_success = false; //理论上不应该出现，TODO：如果该玩家一直进不去，可能会导致后面玩家都进不去，需要处理

					LOG(ERROR, "玩家:{} 加入房间:{} 失败，原因:{}", it->second->GetID(), room_id, ret);
				}
			}

			//
			//成功创建，删除匹配玩家
			//
			if (match_success)
			{
				auto erase_end_it = player_list.begin();
				std::advance(erase_end_it, 4);
				for (auto it = player_list.begin(); it != erase_end_it; ) it = player_list.erase(it); //删除队列中该几位玩家
			}
		}
		
		task.Repeat(std::chrono::milliseconds(300)); //持续匹配
	});
}

}
