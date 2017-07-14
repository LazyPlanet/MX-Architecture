#include <vector>
#include <algorithm>

#include <boost/asio.hpp>

#include "Room.h"
#include "Game.h"
#include "MXLog.h"
#include "CommonUtil.h"
#include "RedisManager.h"
#include "Timer.h"

namespace Adoter
{

extern const Asset::CommonConst* g_const;

/////////////////////////////////////////////////////
//房间
/////////////////////////////////////////////////////

Asset::ERROR_CODE Room::TryEnter(std::shared_ptr<Player> player)
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (!player || IsFull()) return Asset::ERROR_ROOM_IS_FULL;

	auto it = std::find_if(_players.begin(), _players.end(), [player](std::shared_ptr<Player> p) {
				return player->GetID() == p->GetID();
			});

	if (it != _players.end()) return Asset::ERROR_ROOM_HAS_BEEN_IN; //已经在房间

	DEBUG("player_id:{} enter room:{} success.", player->GetID(), GetID());

	return Asset::ERROR_SUCCESS;
}

bool Room::Enter(std::shared_ptr<Player> player)
{
	if (TryEnter(player) != Asset::ERROR_SUCCESS) 
	{
		ERROR("player_id:{} cannot enter room:{}", player->GetID(), GetID());
		return false; //进入房间之前都需要做此检查，理论上不会出现
	}
	
	_players.push_back(player); //进入房间
	
	player->SetPosition((Asset::POSITION_TYPE)_players.size()); //设置位置
	
	DEBUG("curr_count:{} curr_enter:{} position:{}", _players.size(), player->GetID(), player->GetPosition());

	SyncRoom(); //同步当前房间内玩家数据
	return true;
}

void Room::OnPlayerLeave(int64_t player_id)
{
	SyncRoom(); //同步当前房间内玩家数据
}

std::shared_ptr<Player> Room::GetHoster()
{
	if (_players.size() <= 0) return nullptr;

	return *_players.begin(); //房间里面的一个人就是房主
}

bool Room::IsHoster(int64_t player_id)
{
	auto host = GetHoster();
	if (!host) return false;

	return host->GetID() == player_id;
}

std::shared_ptr<Player> Room::GetPlayer(int64_t player_id)
{
	for (auto player : _players)
	{
		if (player->GetID() == player_id) return player;
	}

	return nullptr;
}

void Room::OnPlayerOperate(std::shared_ptr<Player> player, pb::Message* message)
{
	if (!player) return;
	
	auto game_operate = dynamic_cast<Asset::GameOperation*>(message);
	if (!game_operate) return;
			
	BroadCast(game_operate); //广播玩家操作
	
	switch(game_operate->oper_type())
	{
		case Asset::GAME_OPER_TYPE_START: //开始游戏：其实是个准备
		{
			if (!CanStarGame()) return;

			auto game = std::make_shared<Game>();

			game->Init(shared_from_this()); //洗牌

			game->Start(_players); //开始游戏

			_games.push_back(game); //游戏

			OnGameStart();
		}
		break;

		case Asset::GAME_OPER_TYPE_LEAVE: //离开游戏
		{
			Remove(player->GetID()); //玩家退出房间
		}
		break;
		
		case Asset::GAME_OPER_TYPE_KICKOUT: //踢人
		{
			Remove(game_operate->destination_player_id()); //玩家退出房间
		}
		break;
		
		default:
		{
		}
		break;
	}
}
	
int32_t Room::GetRemainCount() 
{ 
	if (!_stuff) return 0;

	return _stuff->options().open_rands() - _games.size(); 
}

bool Room::Remove(int64_t player_id)
{
	for (auto it = _players.begin(); it != _players.end(); ++it)
	{
		if ((*it)->GetID() != player_id) continue;
			
		it->get()->OnLeaveRoom(); //玩家退出房间

		_players.erase(it); //删除玩家

		OnPlayerLeave(player_id); //玩家离开房间
		
		WARN("player:{} leave room.", player_id);

		return true;
	}

	return false;
}

void Room::OnGameStart()
{
	Asset::GameStart game_start;
	game_start.set_total_rounds(_stuff->options().open_rands());
	game_start.set_current_rounds(_games.size());

	BroadCast(game_start);
}

void Room::GameOver(int64_t player_id)
{
	if (_banker != player_id) 
		_banker_index = (_banker_index + 1) % MAX_PLAYER_COUNT; //下庄
}

void Room::BroadCast(pb::Message* message, int64_t exclude_player_id)
{
	if (!message) return;
			
	DEBUG("房间内广播协议:{}", message->ShortDebugString());

	for (auto player : _players)
	{
		if (!player) 
		{
			ERROR("玩家已经掉线");
			continue; //可能已经释放
		}

		if (exclude_player_id == player->GetID()) continue;

		player->SendProtocol(message);
	}
}
	
void Room::BroadCast(pb::Message& message, int64_t exclude_player_id)
{
	BroadCast(&message, exclude_player_id);
}
	
void Room::SyncRoom()
{
	Asset::RoomInformation message;

	auto redis = std::make_shared<Redis>(); //加载数据库
	
	for (auto player : _players)
	{
		DEBUG("sync room infomation, curr_player_size:{} player_id:{} position:{}", _players.size(), player->GetID(), player->GetPosition());
		auto p = message.mutable_player_list()->Add();
		p->set_position(player->GetPosition());
		p->set_oper_type(player->GetOperState());
		p->mutable_common_prop()->CopyFrom(player->CommonProp());
		p->mutable_wechat()->CopyFrom(player->GetWechat());
	
		for (auto dis_player : _players)
		{
			if (!dis_player || dis_player->GetID() == player->GetID()) continue;

			auto dis_element = p->mutable_dis_list()->Add();
			dis_element->set_position(dis_player->GetPosition());
			dis_element->set_distance(redis->GetDistance(dis_player->GetID(), player->GetID()));
		}
	}

	BroadCast(message);
}

void Room::OnCreated() 
{ 
	auto curr_time = CommonTimerInstance.GetTime();
	SetTime(curr_time + g_const->room_last_time());
}
	
bool Room::CanStarGame()
{
	if (_players.size() != MAX_PLAYER_COUNT) return false;

	for (auto player : _players)
	{
		if (!player->IsReady()) return false; //需要所有玩家都是准备状态
	}

	return true;
}
	
bool Room::IsExpired()
{
	auto curr_time = CommonTimerInstance.GetTime();
	return _expired_time < curr_time;
}
	
/////////////////////////////////////////////////////
//房间通用管理类
/////////////////////////////////////////////////////
std::shared_ptr<Room> RoomManager::Get(int64_t room_id)
{
	auto it = _rooms.find(room_id);
	if (it == _rooms.end()) return nullptr;
	return it->second;
}
	
bool RoomManager::CheckPassword(int64_t room_id, std::string password)
{
	auto room = Get(room_id);
	if (!room) return false;

	return true;
}

int64_t RoomManager::CreateRoom()
{
	std::shared_ptr<Redis> redis = std::make_shared<Redis>();
	int64_t room_id = redis->CreateRoom();
	return room_id;
}
	
std::shared_ptr<Room> RoomManager::CreateRoom(const Asset::Room& room)
{
	auto room_id = room.room_id();

	if (room_id <= 0) room_id = CreateRoom(); //如果没有房间号，则创建

	if (room_id <= 0) return nullptr;

	auto locate_room = std::make_shared<Room>(room);
	locate_room->SetID(room_id);
	locate_room->OnCreated();

	auto success = OnCreateRoom(locate_room); //成功创建房间
	if (!success)
	{
		LOG(ERROR, "Enter room_id:{} callback failed.", room_id);
	}
	return locate_room;
}

bool RoomManager::OnCreateRoom(std::shared_ptr<Room> room)
{
	if (_rooms.find(room->GetID()) != _rooms.end()) 
	{
		ERROR("room:{} has exist.", room->GetID());
		return false;
	}

	_rooms.emplace(room->GetID(), room);
	return true;
}

std::shared_ptr<Room> RoomManager::GetAvailableRoom()
{
	std::lock_guard<std::mutex> lock(_no_password_mutex);

	if (_no_password_rooms.size())
	{
		return _no_password_rooms.begin()->second; //取一个未满的房间
	}

	return nullptr;
}
	
void RoomManager::Update(int32_t diff)
{
	for (auto it = _rooms.begin(); it != _rooms.end(); )
	{
		if (it->second->IsExpired() && it->second->IsEmpty())
		{
			LOG(INFO, "Remove room_id:{} for empty and expired.", it->second->GetID());

			it = _rooms.erase(it);
		}
		else
		{
			++it;
		}
	}
}

}
