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
	DEBUG("匹配房开始匹配...");

	_scheduler.Schedule(std::chrono::seconds(3), [this](TaskContext task) {
			
		for (auto it = _match_list.begin(); it != _match_list.end(); ++it)
		{
			auto& player_list = it->second; 
	
			DEBUG("房间类型:{} 玩家数量:{}", it->first, it->second.size());
		
			auto room_type = (Asset::ROOM_TYPE)it->first;

			auto room_ptr = RoomInstance.GetMatchingRoom(room_type);
			if (!room_ptr) continue;

			Asset::EnterRoom enter_room;
			enter_room.mutable_room()->CopyFrom(room_ptr->Get());
			enter_room.set_enter_type(Asset::EnterRoom_ENTER_TYPE_ENTER_TYPE_ENTER); //进入房间

			const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_ROOM);

			auto room_it = std::find_if(messages.begin(), messages.end(), [room_type](pb::Message* message){
				 auto room_limit = dynamic_cast<Asset::RoomLimit*>(message);
				 if (!room_limit) return false;

				 return room_type == room_limit->room_type();
			 });
			if (room_it == messages.end()) return;

			auto room_limit = dynamic_cast<Asset::RoomLimit*>(*room_it);
			if (!room_limit) return;

			room_ptr->SetOptions(room_limit->room_options()); //玩法

			//
			//检查是否满足创建房间条件
			//
			for (auto it = player_list.begin(); it != player_list.end(); )
			{
				auto player = it->second;
				if (!player) continue;

				auto enter_status = room_ptr->TryEnter(player); //玩家尝试进入房间
				enter_room.set_error_code(enter_status); 

				if (enter_status == Asset::ERROR_SUCCESS || enter_status == Asset::ERROR_ROOM_HAS_BEEN_IN) 
				{
					bool success = room_ptr->Enter(player); //玩家进入房间
					if (success) player->OnEnterSuccess(room_ptr->GetID());
					
					enter_room.set_error_code(Asset::ERROR_SUCCESS); 

					it = player_list.erase(it);	//删除匹配玩家
				}
				else
				{
					ERROR("玩家:{} 加入房间{} 失败，原因:{}", it->first, enter_room.ShortDebugString(), enter_status);

					++it; //持续匹配
				}
				
				player->SendProtocol(enter_room); //提示Client是否成功
			}
		}
		
		task.Repeat(std::chrono::milliseconds(300)); //持续匹配
	});
}

}
