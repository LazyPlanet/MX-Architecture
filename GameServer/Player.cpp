#include <iostream>

//#include <hiredis.h>

#include <spdlog/spdlog.h>
#include <pbjson.hpp>

#include "Player.h"
#include "Game.h"
#include "Timer.h"
#include "Mall.h"
#include "Protocol.h"
#include "CommonUtil.h"
#include "RedisManager.h"
#include "PlayerCommonReward.h"
#include "PlayerCommonLimit.h"
#include "PlayerMatch.h"

namespace Adoter
{

namespace spd = spdlog;

Player::Player()
{
	//协议默认处理函数
	_method = std::bind(&Player::DefaultMethod, this, std::placeholders::_1);

	//协议处理回调初始化
	AddHandler(Asset::META_TYPE_SHARE_CREATE_ROOM, std::bind(&Player::CmdCreateRoom, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_GAME_OPERATION, std::bind(&Player::CmdGameOperate, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_PAI_OPERATION, std::bind(&Player::CmdPaiOperate, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_BUY_SOMETHING, std::bind(&Player::CmdBuySomething, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_ENTER_ROOM, std::bind(&Player::CmdEnterRoom, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_SIGN, std::bind(&Player::CmdSign, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_RANDOM_SAIZI, std::bind(&Player::CmdSaizi, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_COMMON_PROPERTY, std::bind(&Player::CmdGetCommonProperty, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_SAY_HI, std::bind(&Player::CmdSayHi, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_GAME_SETTING, std::bind(&Player::CmdGameSetting, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_SYSTEM_CHAT, std::bind(&Player::CmdSystemChat, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_RECHARGE, std::bind(&Player::CmdRecharge, this, std::placeholders::_1));

	//AddHandler(Asset::META_TYPE_C2S_LOGIN, std::bind(&Player::CmdLogin, this, std::placeholders::_1));
	//AddHandler(Asset::META_TYPE_C2S_ENTER_GAME, std::bind(&Player::CmdEnterGame, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_C2S_GET_REWARD, std::bind(&Player::CmdGetReward, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_C2S_LOAD_SCENE, std::bind(&Player::CmdLoadScene, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_C2S_GET_ROOM_DATA, std::bind(&Player::CmdGetRoomData, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_C2S_UPDATE_ROOM, std::bind(&Player::CmdUpdateRoom, this, std::placeholders::_1));
	
	//中心服务器协议处理
	AddHandler(Asset::META_TYPE_S2S_KICKOUT_PLAYER, std::bind(&Player::OnKickOut, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_S2S_PLAYER_STATE, std::bind(&Player::OnPlayerStateChanged, this, std::placeholders::_1));
}
	
Player::Player(int64_t player_id) : Player()/*委派构造函数*/
{
	SetID(player_id);	
}

/*
Player::Player(int64_t player_id, std::shared_ptr<WorldSession> session) : Player()
{
	SetID(player_id);	
	//_session = session; //地址拷贝
}
*/

int32_t Player::Load()
{
	//加载数据库
	//auto redis = make_unique<Redis>();
	auto success = RedisInstance.GetPlayer(_player_id, _stuff);
	if (!success) return 1;
		
	//初始化包裹
	//
	//创建角色或者增加包裹会调用一次
	//
	do {
		const pb::EnumDescriptor* enum_desc = Asset::INVENTORY_TYPE_descriptor();
		if (!enum_desc) return 0;

		int32_t curr_inventories_size = _stuff.inventory().inventory_size(); 
		if (curr_inventories_size == enum_desc->value_count() - 1) break; 

		for (int inventory_index = curr_inventories_size; inventory_index < enum_desc->value_count() - 1; ++inventory_index)
		{
			auto inventory = _stuff.mutable_inventory()->mutable_inventory()->Add(); //增加新包裹，且初始化数据
			inventory->set_inventory_type((Asset::INVENTORY_TYPE)(inventory_index + 1));

			const pb::EnumValueDescriptor *enum_value = enum_desc->value(inventory_index);
			if (!enum_value) break;
		}
	} while(false);
	
	return 0;
}

int32_t Player::Save(bool force)
{
	LOG_BI("player", _stuff);

	if (!force && !IsDirty()) return 1;

	auto success = RedisInstance.SavePlayer(_player_id, _stuff);
	if (!success) 
	{
		LOG(ERROR, "保存玩家:{}数据:{}失败", _player_id, _stuff.ShortDebugString());
		return 2;
	}
	
	_dirty = false;

	return 0;
}
	
int32_t Player::OnLogin()
{
	if (Load()) 
	{
		LOG(ERROR, "玩家:{}加载数据失败", _player_id);
		return 1;
	}

	//DEBUG("玩家:{}数据:{}", _player_id, _stuff.ShortDebugString())
	
	PlayerInstance.Emplace(_player_id, shared_from_this()); //玩家管理
	SetLocalServer(ConfigInstance.GetInt("ServerID", 1));

	return 0;
}

int32_t Player::Logout(pb::Message* message)
{
	const auto kick_out = dynamic_cast<const Asset::KickOutPlayer*>(message);
	if (!kick_out) return 1;
	//
	//如果玩家正在进行玩牌，则不允许立即退出
	//
	//(1) 处理非操作玩家退出的状态;
	//
	//(2) 处理操作玩家退出的状态，即刚好轮到该玩家进行操作的时候，玩家逃跑;
	//
	if (_room) 
	{
		if (_game || (_room->HasStarted() && !_room->HasBeenOver() && !_room->HasDisMiss())) //游戏中，或已经开局且尚未对局完成且不是解散，则不让退出房间
		{
			SetOffline(); //玩家状态

			//auto room_id = _room->GetID();
			//ERROR("玩家:{}从房间且牌局内退出游戏:{}", _player_id, room_id); //玩家逃跑

			//_tuoguan_server = true; //服务器托管

			/*
			if (_tuoguan_server && _game->CanPaiOperate(shared_from_this())) //轮到该玩家操作
			{
				Asset::PaiElement pai;

				for (auto it = _cards_inhand.begin(); it != _cards_inhand.end(); ++it)
				{
					if (it->second.size())
					{
						pai.set_card_type((Asset::CARD_TYPE)it->first);
						pai.set_card_value(it->second[0]); //随便选取一个
						break;
					}
				}

				Asset::PaiOperation pai_operation; 
				pai_operation.set_oper_type(Asset::PAI_OPER_TYPE_DAPAI);
				pai_operation.set_position(GetPosition());
				pai_operation.mutable_pai()->CopyFrom(pai);

				CmdPaiOperate(&pai_operation);
			}
			*/

			return 2; //不能退出游戏
		}
		else
		{
			//
			//房主在尚未开局状态，不能因为离线而解散或者退出房间
			//
			if (_room->IsHoster(_player_id) && !_room->HasBeenOver() && !_room->HasDisMiss())
			{
				SetOffline(); //玩家状态

				//_room->KickOutPlayer(); //不做踢人处理

				return 3;
			}
			else
			{
				_room->Remove(_player_id); //退出房间，回调会调用OnLogout接口，从而退出整个游戏逻辑服务器

				return 4;
			}
		}
	}

	OnLogout(Asset::KICK_OUT_REASON_LOGOUT); //否则房主不会退出//直接通知中心服务器退出
	
	return 0;
}
	
int32_t Player::OnLogout(Asset::KICK_OUT_REASON reason)
{
	if (!_game && _room && (!_room->HasStarted() || _room->HasBeenOver() || _room->HasDisMiss())) 
	{
		ResetRoom();
	}
	else if (!_room && _stuff.room_id()) //进入房间后加载场景失败
	{
		auto room = RoomInstance.Get(_stuff.room_id());
		if (room) room->Remove(_player_id);
	}

	_stuff.clear_server_id(); //退出游戏逻辑服务器

	Save(true);	//存档数据库
	PlayerInstance.Remove(_player_id); //删除玩家

	Asset::KickOutPlayer kickout_player; //通知中心服务器退出
	kickout_player.set_player_id(_player_id);
	kickout_player.set_reason(reason);
	SendProtocol(kickout_player);
	
	return 0;
}
	
/*
void Player::OnCreatePlayer(int64_t player_id)
{
	Asset::CreatePlayer create_player;
	create_player.set_player_id(player_id);
	SendProtocol(create_player);
}

int32_t Player::CmdEnterGame(pb::Message* message)
{
	OnEnterGame();
	return 0;
}
*/
	
int64_t Player::ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	if (_stuff.common_prop().room_card_count() - count > 0)
	{
		_stuff.mutable_common_prop()->set_room_card_count(_stuff.common_prop().room_card_count() - count);
	}
	else
	{
		_stuff.mutable_common_prop()->set_room_card_count(0);
	}

	_dirty = true;
	
	SyncCommonProperty();
	
	LOG(INFO, "玩家:{}消耗房卡，原因:{} 数量:{}成功", _player_id, changed_type, count);
	return count;
}

int64_t Player::GainRoomCard(Asset::ROOM_CARD_CHANGED_TYPE changed_type, int64_t count) 
{
	if (count <= 0) return 0;
	
	_stuff.mutable_common_prop()->set_room_card_count(_stuff.common_prop().room_card_count() + count);
	_dirty = true;
	
	SyncCommonProperty();
	
	LOG(INFO, "玩家:{}获得房卡，原因:{} 数量:{}成功", _player_id, changed_type, count);
	return count;
}

bool Player::CheckRoomCard(int64_t count)
{
	int64_t curr_count = _stuff.common_prop().room_card_count();
	return curr_count >= count;
}

int64_t Player::GetRoomCard()
{
	return _stuff.common_prop().room_card_count();
}

int64_t Player::ConsumeHuanledou(Asset::HUANLEDOU_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	if (_stuff.common_prop().huanledou() - count > 0)
	{
		_stuff.mutable_common_prop()->set_huanledou(_stuff.common_prop().huanledou() - count);
	}
	else
	{
		_stuff.mutable_common_prop()->set_huanledou(0);
	}
	
	_dirty = true;
	
	SyncCommonProperty();
	
	LOG(INFO, "玩家:{}消耗欢乐豆，原因:{} 数量:{}成功", _player_id, changed_type, count);
	return count;
}

int64_t Player::GainHuanledou(Asset::HUANLEDOU_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	_stuff.mutable_common_prop()->set_huanledou(_stuff.common_prop().huanledou() + count);
	_dirty = true;
	
	SyncCommonProperty();
	
	LOG(INFO, "玩家:{}获得欢乐豆，原因:{} 数量:{}成功", _player_id, changed_type, count);
	return count;
}

bool Player::CheckHuanledou(int64_t count)
{
	int64_t curr_count = _stuff.common_prop().huanledou();
	return curr_count >= count;
}

int64_t Player::GetHuanledou() 
{ 
	return _stuff.common_prop().huanledou(); 
}

int64_t Player::GetDiamond() 
{ 
	return _stuff.common_prop().diamond(); 
}

int64_t Player::ConsumeDiamond(Asset::DIAMOND_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	if (_stuff.common_prop().diamond() - count > 0)
	{
		_stuff.mutable_common_prop()->set_diamond(_stuff.common_prop().diamond() - count);
	}
	else
	{
		_stuff.mutable_common_prop()->set_diamond(0);
	}
	
	_dirty = true;
	
	SyncCommonProperty();
	
	LOG(INFO, "玩家:{}消耗钻石:{}原因:{}", _player_id, count, changed_type);
	return count;
}

int64_t Player::GainDiamond(Asset::DIAMOND_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	_stuff.mutable_common_prop()->set_diamond(_stuff.common_prop().diamond() + count);
	_dirty = true;

	SyncCommonProperty();
	
	LOG(INFO, "玩家:{}获得钻石:{}原因:{}", _player_id, count, changed_type);
	return count;
}

bool Player::CheckDiamond(int64_t count)
{
	int64_t curr_count = _stuff.common_prop().diamond();
	return curr_count >= count;
}

int32_t Player::OnEnterGame() 
{
	if (Load()) 
	{
		LOG(ERROR, "玩家:{}加载数据失败", _player_id);
		return 1;
	}
	
	//
	//设置玩家所在服务器，每次进入场景均调用此
	//
	//对于MMORPG游戏，可以是任意一个场景或副本ID，此处记录为解决全球唯一服，通过Redis进行进程间通信，获取玩家所在服务器ID.
	//
	SetLocalServer(ConfigInstance.GetInt("ServerID", 1));

	//SendPlayer(); //发送数据给玩家
	
	_stuff.set_login_time(CommonTimerInstance.GetTime());
	_stuff.set_logout_time(0);

	SetDirty(); //存盘

	LOG_BI("player", _stuff);

	//WorldSessionInstance.Emplace(_player_id, _session); //网络会话数据
	PlayerInstance.Emplace(_player_id, shared_from_this()); //玩家管理

	return 0;
}

int32_t Player::CmdLeaveRoom(pb::Message* message)
{
	if (!message) return 1;

	OnLeaveRoom(); //房间处理

	return 0;
}

void Player::SendPlayer()
{
	Asset::PlayerInformation player_info;
	player_info.mutable_player()->CopyFrom(this->_stuff);

	SendProtocol(player_info);
}

int32_t Player::CmdCreateRoom(pb::Message* message)
{
	int32_t result = CreateRoom(message);
	if (result) OnLogout(); //创建失败

	return result;
}

int32_t Player::CreateRoom(pb::Message* message)
{
	Asset::CreateRoom* create_room = dynamic_cast<Asset::CreateRoom*>(message);
	if (!create_room) return 1;
	
	if (_room) 
	{
		auto room = RoomInstance.Get(_room->GetID());

		if (room && room->GetRemainCount() > 0) //房间尚未解散
		{
			SendRoomState();

			return 2;
		}
	}

	//
	//检查是否活动限免房卡
	//
	//否则，检查房卡是否满足要求
	//
	auto activity_id = g_const->room_card_limit_free_activity_id();
	if (ActivityInstance.IsOpen(activity_id))
	{
		WARN("当前活动:{}开启，玩家ID:{}", activity_id, _player_id);
	}
	else
	{
		auto open_rands = create_room->room().options().open_rands(); //局数
		auto pay_type = create_room->room().options().pay_type(); //付费方式

		const Asset::Item_RoomCard* room_card = dynamic_cast<const Asset::Item_RoomCard*>(AssetInstance.Get(g_const->room_card_id()));
		if (!room_card || room_card->rounds() <= 0) return 3;

		int32_t consume_count = open_rands / room_card->rounds(); //待消耗房卡数量

		switch (pay_type)
		{
			case Asset::ROOM_PAY_TYPE_HOSTER:
			{
				if (!CheckRoomCard(consume_count)) 
				{
					AlertMessage(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //房卡不足

					LOG(ERROR, "玩家:{}开房房卡不足，当前房卡数量:{}需要消耗房卡数量:{}，开局数量:{}，单个房卡可以开房数量:{}", _player_id, _stuff.common_prop().room_card_count(), consume_count, open_rands, room_card->rounds());
					return 5;
				}
			}
			break;
			
			case Asset::ROOM_PAY_TYPE_AA:
			{
				consume_count = consume_count / MAX_PLAYER_COUNT; //单人付卡数量

				if (!CheckRoomCard(consume_count)) 
				{
					AlertMessage(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //房卡不足
					return 6;
				}
			}
			break;

			default:
			{
				return 7;
			}
			break;
		}
	}

	int64_t room_id = RoomInstance.AllocRoom();
	if (!room_id) return 2;

	create_room->mutable_room()->set_room_id(room_id);
	create_room->mutable_room()->set_room_type(Asset::ROOM_TYPE_FRIEND); //创建房间，其实是好友房
	
	SendProtocol(create_room); 
	
	OnCreateRoom(create_room); //创建房间成功

	LOG(INFO, "玩家:{} 创建房间:{} 成功", _player_id, room_id);

	return 0;
}
	
void Player::OnRoomRemoved()
{
	ResetRoom(); //房间非法
}

void Player::OnCreateRoom(Asset::CreateRoom* create_room)
{
	if (!create_room) return; //理论不会如此

	Asset::Room asset_room;
	asset_room.CopyFrom(create_room->room());

	auto room = std::make_shared<Room>(asset_room);
	room->OnCreated(shared_from_this());

	RoomInstance.OnCreateRoom(room); //房间管理
}

int32_t Player::CmdGameOperate(pb::Message* message)
{
	auto game_operate = dynamic_cast<Asset::GameOperation*>(message);
	if (!game_operate) return 1;
	
	game_operate->set_source_player_id(_player_id); //设置当前操作玩家

	switch(game_operate->oper_type())
	{
		case Asset::GAME_OPER_TYPE_NULL: 
		case Asset::GAME_OPER_TYPE_START: //开始游戏：相当于准备
		{
			_player_prop.set_game_oper_state(game_operate->oper_type());
		}
		break;

		case Asset::GAME_OPER_TYPE_LEAVE: //离开游戏：相当于退出房间
		{
			if (_game) return 0;

			if (!_room) 
			{
				OnLeaveRoom();
				return 0; //如果玩家不在房间，也不存在后面的逻辑
			}
		}
		break;

		case Asset::GAME_OPER_TYPE_DISMISS_AGREE: //解散
		case Asset::GAME_OPER_TYPE_DISMISS_DISAGREE: //不解散
		{
			_player_prop.set_game_oper_state(game_operate->oper_type());

			if (!_room || _room->HasBeenOver()) 
			{
				OnLeaveRoom(); //防止玩家不在房间内进行解散操作,出现这种情况原因是C<->S状态不一致
				return 0;
			}
		}
		break;

		case Asset::GAME_OPER_TYPE_KICKOUT: //踢人
		{
			if (!_room->IsHoster(_player_id)) //不是房主，不能踢人
			{
				AlertMessage(Asset::ERROR_ROOM_NO_PERMISSION); //没有权限
				return 3;
			}
		}
		break;

		default:
		{
			 _player_prop.clear_game_oper_state(); //错误状态
		}
		break;
	}

	if (!_room) return 4;

	_room->OnPlayerOperate(shared_from_this(), message); //广播给其他玩家

	return 0;
}
	
void Player::SetStreakWins(int32_t count) 
{ 
	int32_t curr_count = _stuff.common_prop().streak_wins();
	if (count <= curr_count) return;

	_stuff.mutable_common_prop()->set_streak_wins(count); 
	_dirty = true; 
}
	
void Player::OnGameStart()
{
	AddTotalRounds(); //总对战局数
	if (_room && _room->IsFriend()) AddFriendRoomRounds(); //好友房对战局数

	ClearCards();  //游戏数据
}

int32_t Player::CmdPaiOperate(pb::Message* message)
{
	std::lock_guard<std::mutex> lock(_card_lock);

	Asset::PaiOperation* pai_operate = dynamic_cast<Asset::PaiOperation*>(message);
	if (!pai_operate) return 1; 
	
	if (!_room || !_game) return 2; //还没加入房间或者还没开始游戏

	if (!pai_operate->position()) pai_operate->set_position(GetPosition()); //设置玩家座位
			
	auto debug_string = pai_operate->ShortDebugString();
	const auto& pai = pai_operate->pai(); 
	
	//进行操作
	switch (pai_operate->oper_type())
	{
		case Asset::PAI_OPER_TYPE_DAPAI: //打牌
		{
			if (!ShouldDaPai()) 
			{
				PrintPai();

				LOG(ERROR, "玩家:{}在房间:{}第:{}局中不能打牌，当前牌数量:{} 无法进行操作:{}", _player_id, _room->GetID(), _game->GetID(), GetCardCount(), debug_string);
				return 3;
			}

			auto& pais = _cards_inhand[pai.card_type()]; //获取该类型的牌
			
			auto it = std::find(pais.begin(), pais.end(), pai.card_value()); //查找第一个满足条件的牌即可
			if (it == pais.end()) 
			{
				LOG(ERROR, "玩家:{}在房间:{}第:{}局不能打牌，无法找到牌:{}", _player_id, _room->GetID(), _game->GetID(), debug_string);
				return 4; //没有这张牌
			}

			pais.erase(it); //打出牌

			Add2CardsPool(pai);
		}
		break;
		
		case Asset::PAI_OPER_TYPE_CHIPAI: //吃牌
		{
			//检查玩家是否真的有这些牌
			for (const auto& pai : pai_operate->pais()) 
			{
				const auto& pais = _cards_inhand[pai.card_type()];

				auto it = std::find(pais.begin(), pais.end(), pai.card_value());
				if (it == pais.end()) 
				{
					LOG(ERROR, "玩家:{}在房间:{}第:{}局不能吃牌，尚未找到牌数据，类型:{} 值:{} 不满足条件:{}", _player_id, _room->GetID(), _game->GetID(), pai.card_type(), pai.card_value(), debug_string);
					return 5; //没有这张牌
				}

				//if (pais[pai.card_index()] != pai.card_value()) return 6; //Server<->Client 不一致，暂时不做检查
			}
		}
		break;
		
		case Asset::PAI_OPER_TYPE_PENGPAI: //碰牌
		{
			bool ret = CheckPengPai(pai);
			if (!ret) 
			{
				LOG(ERROR, "玩家:{}在房间:{}第:{}局不能碰牌，不满足条件:{}", _player_id, _room->GetID(), _game->GetID(), debug_string);
				return 7;
			}
		}
		break;
		
		case Asset::PAI_OPER_TYPE_GANGPAI: //明杠：简单牌数量检查
		{
			auto it = _cards_inhand.find(pai.card_type());
			if (it == _cards_inhand.end()) 
			{
				LOG(ERROR, "玩家:{}在房间:{}第:{}局不能明杠，没找到牌数据:{}", _player_id, _room->GetID(), _game->GetID(), debug_string);
				return 8;
			}

			int32_t count = std::count(it->second.begin(), it->second.end(), pai.card_value());

			if (count == 1)
			{
				bool has_peng = false;

				for (auto cards : _cards_outhand)
				{
					if (cards.second.size() == 0) continue;

					if (cards.second.size() % 3 != 0) return 9;

					for (size_t i = 0; i < cards.second.size(); i = i + 3)
					{
						auto card_value = cards.second.at(i);
						if (pai.card_value() != card_value) continue;

						if ((card_value == cards.second.at(i + 1)) && (card_value == cards.second.at(i + 2))) 
						{
							has_peng = true;
							break;
						}
					}
				}

				if (!has_peng) 
				{
					LOG(ERROR, "玩家:{}在房间:{}第:{}局不能明杠，不满足条件:{}", _player_id, _room->GetID(), _game->GetID(), debug_string);
					return 10;
				}
			}
			else if (count == 3)
			{
				//理论上可以杠牌
			}
		}
		break;

		case Asset::PAI_OPER_TYPE_ANGANGPAI: //暗杠：简单牌数量检查
		{
			auto it = _cards_inhand.find(pai.card_type());
			if (it == _cards_inhand.end()) return 11;

			int32_t count = std::count(it->second.begin(), it->second.end(), pai.card_value());
			if (count != 4)
			{
				LOG(ERROR, "玩家:{}在房间:{}第:{}局不能暗杠，不满足条件:{}", _player_id, _room->GetID(), _game->GetID(), debug_string);
				return 12;
			}
		}
		break;
		
		case Asset::PAI_OPER_TYPE_XUANFENG_FENG: //旋风杠
		{
			/*
			if (_oper_count >= 2) 
			{
				return 5;
			}
			*/

			--_oper_count;

			OnGangFengPai();
		}
		break;
		
		case Asset::PAI_OPER_TYPE_XUANFENG_JIAN: //旋风杠
		{
			/*
			if (_oper_count >= 2) 
			{
				return 6;
			}
			*/
			
			--_oper_count;
			
			OnGangJianPai();
		}
		break;
		
		case Asset::PAI_OPER_TYPE_TINGPAI: //听牌
		{
			const auto& pai = pai_operate->pai();

			auto& pais = _cards_inhand[pai.card_type()]; //获取该类型的牌

			auto it = std::find(pais.begin(), pais.end(), pai.card_value()); //查找第一个满足条件的牌即可
			if (it == pais.end()) 
			{
				LOG(ERROR, "玩家:{}在房间:{}第:{}局不能听牌:{}, 原因:没找到牌", _player_id, _room->GetID(), _game->GetID(), pai.ShortDebugString());
				return 13; //没有这张牌
			}

			if (!CanTingPai(pai)) 
			{
				LOG(ERROR, "玩家:{}在房间:{}第:{}局不能听牌:{}, 原因:不满足牌型", _player_id, _room->GetID(), _game->GetID(), pai.ShortDebugString());
				return 14; //不能听牌
			}

			pais.erase(it); //删除牌

			Add2CardsPool(pai);

			if (OnTingPai()) 
			{
				_last_oper_type = _oper_type; //记录上次牌操作
				_oper_type = pai_operate->oper_type(); 

				_game->AddPlayerOperation(*pai_operate);  //回放记录
				_game->BroadCast(message); //操作
				_game->Add2CardsPool(pai); //牌池

				return 0; //进宝
			}
		}
		break;

		case Asset::PAI_OPER_TYPE_CANCEL:
		{
			return 0;
		}
		break;

		default:
		{
			//return 0; //下面还要进行操作
		}
		break;
	}

	++_oper_count;

	//
	//处理杠流泪场景，须在_game->OnPaiOperate下进行判断
	//
	//玩家抓到杠之后，进行打牌，记录上次牌状态
	//
	_last_oper_type = _oper_type; //记录上次牌操作
	_oper_type = pai_operate->oper_type(); 
	
	_game->OnPaiOperate(shared_from_this(), message);

	return 0;
}
	
int32_t Player::CmdSign(pb::Message* message)
{
	Asset::Sign* sign = dynamic_cast<Asset::Sign*>(message);
	if (!sign) return 1; 

	auto curr_t = CommonTimerInstance.GetTime(); //当前时间

	auto it = std::find_if(_stuff.sign_time().rbegin(), _stuff.sign_time().rend(), [curr_t](const int32_t& time) {
			return CommonTimerInstance.IsSameDay(curr_t, time); 
	});

	if (it == _stuff.sign_time().rend()) 
	{
		_stuff.mutable_sign_time()->Add(curr_t); //记录签到时间
		sign->set_success(true); //默认失败
	}

	//发奖
	auto asset_message = AssetInstance.Get(g_const->daily_sign_id());
	if (!asset_message) return 2;

	auto asset_sign = dynamic_cast<Asset::DailySign*>(asset_message);
	if (!asset_sign) return 3;

	auto common_limit_id = asset_sign->common_limit_id();
	if (!IsCommonLimit(common_limit_id)) DeliverReward(asset_sign->common_reward_id()); //正式发奖
	AddCommonLimit(common_limit_id); //今日已领取

	SendProtocol(sign);
	return 0;
}

int32_t Player::CmdGetCommonProperty(pb::Message* message)
{
	Asset::SyncCommonProperty* common_prop = dynamic_cast<Asset::SyncCommonProperty*>(message);
	if (!common_prop) return 1; 

	SyncCommonProperty(Asset::SyncCommonProperty_SYNC_REASON_TYPE_SYNC_REASON_TYPE_GET);
	return 0;
}
	
void Player::SyncCommonProperty(Asset::SyncCommonProperty_SYNC_REASON_TYPE reason)
{
	Asset::SyncCommonProperty common_prop;
	
	common_prop.set_reason_type(reason);
	common_prop.set_player_id(_player_id);
	common_prop.mutable_common_prop()->CopyFrom(_stuff.common_prop());

	SendProtocol(common_prop);
}

int32_t Player::CmdEnterRoom(pb::Message* message) 
{
	int32_t result = EnterRoom(message);
	if (result) OnLogout(); //进入失败

	return result;
}

int32_t Player::EnterRoom(pb::Message* message) 
{
	Asset::EnterRoom* enter_room = dynamic_cast<Asset::EnterRoom*>(message);
	if (!enter_room) return Asset::ERROR_INNER; 

	//
	//房间重入检查
	//
	do 
	{
		if (_room) 
		{
			auto room_id = _room->GetID();

			auto client_room_id = enter_room->room().room_id();
			if (room_id != client_room_id)
			{
				LOG(ERROR, "玩家:{}重入房间错误，客户端记录:{}和服务器记录:{}不是一个，以当前客户端为准", _player_id, client_room_id, room_id);
				room_id = client_room_id;
			}
				
			auto locate_room = RoomInstance.Get(room_id);
			if (!locate_room)
			{
				enter_room->set_error_code(Asset::ERROR_ROOM_NOT_FOUNT); //是否可以进入场景//房间
				SendProtocol(message);
				
				AlertMessage(Asset::ERROR_ROOM_NOT_FOUNT, Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE_MESSAGE_BOX); //不能加入，错误提示

				ResetRoom(); //房间非法
				SetOffline(false); //恢复在线状态

				return Asset::ERROR_ROOM_NOT_FOUNT;
			}

			if (_room->GetID() == enter_room->room().room_id()) //重入房间
			{
				enter_room->mutable_room()->CopyFrom(_room->Get());
				enter_room->set_error_code(Asset::ERROR_SUCCESS); //是否可以进入场景//房间
				SendProtocol(message);

				return Asset::ERROR_SUCCESS;
			}
			else
			{
				locate_room = RoomInstance.Get(_room->GetID());
				if (!locate_room || locate_room->HasDisMiss() || locate_room->HasBeenOver()) //已经结束或者解散
				{
					ResetRoom(); //房间非法
					break; //房间已经不存在
				}

				SendRoomState();

				//return Asset::ERROR_ROOM_HAS_BEEN_IN;
				return Asset::ERROR_SUCCESS;
			}
		}
	} while (false);

	//
	//房间正常进入
	//

	ClearCards();

	Asset::ROOM_TYPE room_type = enter_room->room().room_type();

	auto check = [this, room_type]()->Asset::ERROR_CODE {

		const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_ROOM);

		auto it = std::find_if(messages.begin(), messages.end(), [room_type](pb::Message* message){
			auto room_limit = dynamic_cast<Asset::RoomLimit*>(message);
			if (!room_limit) return false;

			return room_type == room_limit->room_type();
		});

		if (it == messages.end()) return Asset::ERROR_ROOM_TYPE_NOT_FOUND;
		
		auto room_limit = dynamic_cast<Asset::RoomLimit*>(*it);
		if (!room_limit) return Asset::ERROR_ROOM_TYPE_NOT_FOUND;

		int64_t beans_count = GetHuanledou();

		int32_t min_limit = room_limit->min_limit();
		if (min_limit >= 0 && beans_count < min_limit) return Asset::ERROR_ROOM_BEANS_MIN_LIMIT;

		int32_t max_limit = room_limit->max_limit();
		if (max_limit >= 0 && beans_count > max_limit) return Asset::ERROR_ROOM_BEANS_MAX_LIMIT;

		return Asset::ERROR_SUCCESS;
	};

	switch (room_type)
	{
		case Asset::ROOM_TYPE_FRIEND: //好友房
		{
			auto room_id = enter_room->room().room_id(); 

			auto locate_room = RoomInstance.Get(room_id);

			if (!locate_room) 
			{
				enter_room->set_error_code(Asset::ERROR_ROOM_NOT_FOUNT); //是否可以进入场景//房间
			}
			else
			{
				if (locate_room->GetOptions().pay_type() == Asset::ROOM_PAY_TYPE_AA && !ActivityInstance.IsOpen(g_const->room_card_limit_free_activity_id())) //AA付卡
				{
					const Asset::Item_RoomCard* room_card = dynamic_cast<const Asset::Item_RoomCard*>(AssetInstance.Get(g_const->room_card_id()));
					if (!room_card || room_card->rounds() <= 0) return Asset::ERROR_INNER;

					int32_t consume_count = locate_room->GetOpenRands() / room_card->rounds() / MAX_PLAYER_COUNT;
					if (consume_count <= 0 || !CheckRoomCard(consume_count))
					{
						//AlertMessage(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //房卡不足

						enter_room->set_error_code(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //房卡不足
						SendProtocol(enter_room);
						return Asset::ERROR_ROOM_CARD_NOT_ENOUGH;
					}
				}

				auto enter_status = locate_room->TryEnter(shared_from_this()); //玩家进入房间
				enter_room->mutable_room()->CopyFrom(locate_room->Get());
				enter_room->set_error_code(enter_status); //是否可以进入场景//房间

				if (enter_status == Asset::ERROR_SUCCESS || enter_status == Asset::ERROR_ROOM_HAS_BEEN_IN) 
				{
					enter_room->set_error_code(Asset::ERROR_SUCCESS);
					bool success = locate_room->Enter(shared_from_this()); //玩家进入房间

					if (success) OnEnterSuccess(room_id);
				}
			}
			
			if (enter_room->error_code() != Asset::ERROR_SUCCESS) 
				AlertMessage(enter_room->error_code(), Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE_MESSAGE_BOX); //不能加入，错误提示

			SendProtocol(enter_room);
		}
		break;

		case Asset::ROOM_TYPE_XINSHOU:
		case Asset::ROOM_TYPE_GAOSHOU:
		case Asset::ROOM_TYPE_DASHI:
		{
			auto result = check();

			if (result != Asset::ERROR_SUCCESS) //不允许进入
			{
				AlertMessage(result);
				return result;
			}

			//进入匹配
			MatchInstance.Join(shared_from_this(), message);
		}
		break;

		default:
		{
			return Asset::ERROR_INNER; //非法
		}
		break;
	}

	return 0;
}
	
int32_t Player::GetLocalRoomID() 
{ 
	if (!_room) return 0; 

	return _room->GetID();
}

void Player::OnEnterSuccess(int64_t room_id)
{
	_stuff.set_room_id(room_id); //防止玩家进入房间后尚未加载场景，掉线

	SetDirty();
}

bool Player::HandleMessage(const Asset::MsgItem& item)
{
	switch (item.type())
	{
	}

	return true;
}

void Player::SendMessage(Asset::MsgItem& item)
{
	DispatcherInstance.SendMessage(item);
}	

void Player::SendProtocol(const pb::Message* message)
{
	if (!message) return;

	SendProtocol(*message);
}

void Player::SendProtocol(const pb::Message& message)
{
	if (!g_center_session) 
	{
		LOG(ERROR, "玩家{}尚未建立连接，目前所在服务器:{}", _player_id, _stuff.server_id());
		return; //尚未建立网络连接
	}

	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::META_TYPE_IsValid(type_t)) return;	//如果不合法，不检查会宕线
	
	Asset::Meta meta;
	meta.set_type_t((Asset::META_TYPE)type_t);
	meta.set_stuff(message.SerializeAsString());
	meta.set_player_id(_player_id);

	std::string content = meta.SerializeAsString();
	if (content.empty()) return;

	g_center_session->AsyncSendMessage(content);

	//DEBUG("玩家:{} 发送协议，类型:{} 内容:{}", _player_id, type_t,  message.ShortDebugString());
}

void Player::Send2Roomers(pb::Message& message, int64_t exclude_player_id) 
{
	if (!_room) return;
	_room->BroadCast(message, exclude_player_id);
}

void Player::Send2Roomers(pb::Message* message, int64_t exclude_player_id)
{
	if (!_room) return;
	_room->BroadCast(message, exclude_player_id);
}

//
//玩家心跳周期为1s
//
//如果该函数返回FALSE则表示掉线
//
bool Player::Update()
{
	++_heart_count; //心跳
	
	if (_heart_count % 3 == 0) //3s
	{
		CommonLimitUpdate(); //通用限制,定时更新
	
		if (_dirty) Save(); //触发存盘
	}
	
	if (_heart_count % 5 == 0) //5s
	{
		OnlineCheck(); //逻辑服务器不进行心跳检查，只进行断线逻辑检查
	}

	//if (_heart_count % 60 == 0) //1min
	return true;
}
	
int32_t Player::DefaultMethod(pb::Message* message)
{
	if (!message) return 0;

	const pb::FieldDescriptor* field = message->GetDescriptor()->FindFieldByName("type_t");
	if (!field) 
	{
		ERROR("玩家:{}尚未存在协议:{}的协议类型", _player_id, message->ShortDebugString());
		return 1;
	}
		
	WARN("玩家:{}尚未存在协议:{}的处理回调", _player_id, message->ShortDebugString());

	return 0;
}

bool Player::HandleProtocol(int32_t type_t, pb::Message* message)
{
	SetOffline(false); //玩家在线

	_pings_count = 0;
	_hi_time = CommonTimerInstance.GetTime();

	CallBack& callback = GetMethod(type_t); 
	callback(std::forward<pb::Message*>(message));	
	return true;
}

bool Player::GainItem(int64_t global_item_id, int32_t count)
{
	/*
	pb::Message* asset_item = AssetInstance.Get(global_item_id); //此处取出来的必然为合法ITEM.
	if (!asset_item) return false;

	Item* item = new Item(asset_item);
	GainItem(item, count);
	*/
	const auto message = AssetInstance.Get(global_item_id); //例如：Asset::Item_Potion
	if (!message) return false;

	auto message_item = message->New(); 
	message_item->CopyFrom(*message);

	if (!message_item) return false; //内存不足
	
	const pb::FieldDescriptor* prop_field = message_item->GetDescriptor()->FindFieldByName("item_common_prop"); //物品公共属性变量
	if (!prop_field) return false; //不是物品

	try {
		const pb::Message& const_item_common_prop = message_item->GetReflection()->GetMessage(*message_item, prop_field);

		pb::Message& item_common_prop = const_cast<pb::Message&>(const_item_common_prop);
		auto& common_prop = dynamic_cast<Asset::Item_CommonProp&>(item_common_prop);

		auto inventory_type = common_prop.inventory(); //物品默认进包

		auto it_inventory = std::find_if(_stuff.mutable_inventory()->mutable_inventory()->begin(), _stuff.mutable_inventory()->mutable_inventory()->end(), 
				[inventory_type](const Asset::Inventory_Element& element){
			return inventory_type == element.inventory_type();		
		});

		if (it_inventory == _stuff.mutable_inventory()->mutable_inventory()->end()) return false; //数据错误

		auto inventory_items = it_inventory->mutable_items(); //包裹中物品数据

		if (!inventory_items) return false; //数据错误

		const pb::FieldDescriptor* type_field = message_item->GetDescriptor()->FindFieldByName("type_t");
		if (!type_field) return false; //数据错误

		auto type_t = type_field->default_value_enum()->number();

		auto it_item = inventory_items->begin(); //查找包裹中该物品数据
		for ( ; it_item != inventory_items->end(); ++it_item)
		{
			if (type_t == it_item->type_t())
			{
				auto item = message_item->New();
				item->ParseFromString(it_item->stuff()); //解析存盘数据

				const FieldDescriptor* item_prop_field = item->GetDescriptor()->FindFieldByName("item_common_prop");
				if (!item_prop_field) continue;

				const Message& item_prop_message = item->GetReflection()->GetMessage(*item, item_prop_field);
				prop_field = item_prop_message.GetDescriptor()->FindFieldByName("common_prop");
				if (!prop_field) continue;

				const Message& prop_message = item_prop_message.GetReflection()->GetMessage(item_prop_message, prop_field);
				const FieldDescriptor* global_id_field = prop_message.GetDescriptor()->FindFieldByName("global_id");
				if (!global_id_field) continue;

				auto global_id = prop_message.GetReflection()->GetInt64(prop_message, global_id_field);
				if (global_id == global_item_id) break; //TODO:不限制堆叠
			}
		}
		
		if (it_item == inventory_items->end()) //没有该类型物品
		{
			auto message_string = message_item->SerializeAsString();

			auto item_toadd = inventory_items->Add();
			item_toadd->set_type_t((Adoter::Asset::ASSET_TYPE)type_t);
			common_prop.set_count(count); //Asset::Item_CommonProp
			item_toadd->set_stuff(message_string);
		}
		else
		{
			auto message_string = message_item->SerializeAsString();

			common_prop.set_count(common_prop.count() + count); //Asset::Item_CommonProp
			it_item->set_stuff(message_string);
		}
	}
	catch (std::exception& e)
	{
		LOG(ERR, "const_cast or dynamic_cast exception:{}", e.what());	
		return false;
	}
	return true;
}

bool Player::GainItem(Item* item, int32_t count)
{
	if (!item || count <= 0) return false;

	Asset::Item_CommonProp& common_prop = item->GetCommonProp(); 
	common_prop.set_count(common_prop.count() + count); //数量

	if (!PushBackItem(common_prop.inventory(), item)) return false;
	return true;
}
	
bool Player::PushBackItem(Asset::INVENTORY_TYPE inventory_type, Item* item)
{
	if (!item) return false;

	const pb::EnumDescriptor* enum_desc = Asset::INVENTORY_TYPE_descriptor();
	if (!enum_desc) return false;

	Asset::Inventory_Element* inventory = _stuff.mutable_inventory()->mutable_inventory(inventory_type); 
	if (!inventory) return false;

	auto item_toadd = inventory->mutable_items()->Add();
	item_toadd->CopyFrom(item->GetCommonProp()); //Asset::Item_CommonProp数据

	return true;
}

void Player::BroadCastCommonProp(Asset::MSG_TYPE type)       
{
	Asset::MsgItem item; //消息数据
	item.set_type(type);
	item.set_sender(_player_id);
	this->BroadCast(item); //通知给房间玩家
}

void Player::OnLeaveRoom(Asset::GAME_OPER_TYPE reason)
{
	//
	//房间数据初始化
	//
	ResetRoom();
	
	//
	//游戏数据
	//
	ClearCards();  

	//
	//逻辑服务器的退出房间，则退出
	//
	OnLogout(Asset::KICK_OUT_REASON_LOGOUT);

	//
	//房间状态同步
	//
	Asset::RoomState room_state;
	room_state.set_room_id(0);
	room_state.set_oper_type(reason);
	SendProtocol(room_state);
}
	
void Player::BroadCast(Asset::MsgItem& item) 
{
	if (!_room) return;
	
}	
	
void Player::ResetRoom() 
{ 
	if (_room) _room.reset(); //刷新房间信息

	_stuff.clear_room_id(); //状态初始化
	_player_prop.clear_voice_member_id(); //房间语音数据
	_dirty = true;
}

void Player::AlertMessage(Asset::ERROR_CODE error_code, Asset::ERROR_TYPE error_type/*= Asset::ERROR_TYPE_NORMAL*/, 
		Asset::ERROR_SHOW_TYPE error_show_type/* = Asset::ERROR_SHOW_TYPE_NORMAL*/)
{
	Asset::AlertMessage message;
	message.set_error_type(error_type);
	message.set_error_show_type(error_show_type);
	message.set_error_code(error_code);

	SendProtocol(message);
}

bool Player::AddCommonLimit(int64_t global_id)
{
	if (global_id <= 0) return false;
	return CommonLimitInstance.AddCommonLimit(shared_from_this(), global_id);
}
	
bool Player::IsCommonLimit(int64_t global_id)
{
	if (global_id <= 0) return false;
	return CommonLimitInstance.IsCommonLimit(shared_from_this(), global_id);
}

bool Player::CommonLimitUpdate()
{
	bool updated = CommonLimitInstance.Update(shared_from_this());
	if (updated) SyncCommonLimit();

	return updated;
}

void Player::SyncCommonLimit()
{
	Asset::SyncCommonLimit proto;
	proto.mutable_common_limit()->CopyFrom(_stuff.common_limit());

	SendProtocol(proto);
}

Asset::ERROR_CODE Player::DeliverReward(int64_t global_id)
{
	auto delivered = CommonRewardInstance.DeliverReward(shared_from_this(), global_id);
	if (delivered == Asset::ERROR_SUCCESS) SyncCommonReward(global_id);
	
	return delivered;
}

void Player::SyncCommonReward(int64_t common_reward_id)
{
	Asset::SyncCommonReward proto;
	proto.set_common_reward_id(common_reward_id);

	SendProtocol(proto);
}

int32_t Player::CmdGetReward(pb::Message* message)
{
	Asset::GetReward* get_reward = dynamic_cast<Asset::GetReward*>(message);
	if (!get_reward) return 1;

	int64_t reward_id = get_reward->reward_id();
	if (reward_id <= 0) return 2;

	switch (get_reward->reason())
	{
		case Asset::GetReward_GET_REWARD_REASON_GET_REWARD_REASON_DAILY_BONUS: //每日登陆奖励
		{
			int64_t daily_bonus_id = g_const->daily_bonus_id();
			if (reward_id != daily_bonus_id) return 3; //Client和Server看到的数据不一致
			
			auto message = AssetInstance.Get(daily_bonus_id);
			if (!message) return 4;

			auto bonus = dynamic_cast<Asset::DailyBonus*>(message);
			if (!bonus) return 5;

			auto ret = DeliverReward(bonus->common_reward_id()); //发奖
			AlertMessage(ret);
		}
		break;
		
		case Asset::GetReward_GET_REWARD_REASON_GET_REWARD_REASON_DAILY_ALLOWANCE: //每日补助奖励
		{
			int64_t daily_allowance_id = g_const->daily_allowance_id();
			if (reward_id != daily_allowance_id) return 3; //Client和Server看到的数据不一致
			
			auto message = AssetInstance.Get(daily_allowance_id);
			if (!message) return 4;

			auto allowance = dynamic_cast<Asset::DailyAllowance*>(message);
			if (!allowance) return 5;

			int32_t huanledou_below = allowance->huanledou_below(); 
			if (huanledou_below > 0 && huanledou_below < GetHuanledou())
			{
				AlertMessage(Asset::ERROR_HUANLEDOU_LIMIT); //欢乐豆数量不满足
				return 8;
			}

			auto ret = DeliverReward(allowance->common_reward_id()); //发奖
			AlertMessage(ret);
		}
		break;

		default:
		{

		}
		break;
	}

	return 0;
}

int32_t Player::CmdBuySomething(pb::Message* message)
{
	auto some_thing = dynamic_cast<Asset::BuySomething*>(message);
	if (!some_thing) return 1;

	int64_t mall_id = some_thing->mall_id();
	if (mall_id <= 0) return 2;

	auto ret = MallInstance.BuySomething(shared_from_this(), mall_id);
	some_thing->set_result(ret);
	SendProtocol(some_thing); //返回给Client

	if (ret) AlertMessage(ret, Asset::ERROR_TYPE_NORMAL, Asset::ERROR_SHOW_TYPE_MESSAGE_BOX);

	return 0;
}
	
int32_t Player::CmdGetRoomData(pb::Message* message)
{
	auto get_data = dynamic_cast<Asset::GetRoomData*>(message);
	if (!get_data) return 1;

	if (get_data->reason() == Asset::ROOM_SYNC_TYPE_QUERY)
	{
		auto room = RoomInstance.Get(get_data->room_id());
		if (!room)
		{
			AlertMessage(Asset::ERROR_ROOM_QUERY_NOT_FORBID);
			return 2;
		}

		Asset::RoomInformation room_information;
		room_information.set_sync_type(Asset::ROOM_SYNC_TYPE_QUERY); //外服查询房间信息
				
		const auto players = room->GetPlayers();
		for (const auto player : players)
		{
			if (!player) continue;

			auto p = room_information.mutable_player_list()->Add();
			p->set_position(player->GetPosition());
			p->set_player_id(player->GetID());
			p->set_oper_type(player->GetOperState());
			p->mutable_common_prop()->CopyFrom(player->CommonProp());
			p->mutable_wechat()->CopyFrom(player->GetWechat());
			p->set_ip_address(player->GetIpAddress());
			p->set_voice_member_id(player->GetVoiceMemberID());
		}

		Asset::RoomQueryResult proto;
		proto.set_room_id(room->GetID());
		proto.set_create_time(room->GetCreateTime());
		proto.mutable_options()->CopyFrom(room->GetOptions());
		proto.mutable_information()->CopyFrom(room_information);

		SendProtocol(proto);
	
		OnLogout(Asset::KICK_OUT_REASON_LOGOUT); //查询之后退出
	}
	else
	{
		DEBUG("玩家:{}由于房间:{}内断线，获取数据:{}", _player_id, message->ShortDebugString(), _stuff.ShortDebugString())

		if (!_room || _room->HasDisMiss() || _room->GetID() != get_data->room_id() || _stuff.room_id() == 0) { SendRoomState(); } //估计房间已经解散
		else { _room->OnReEnter(shared_from_this()); } //再次进入
	}

	return 0;
}

int32_t Player::CmdUpdateRoom(pb::Message* message)
{
	auto update_data = dynamic_cast<Asset::UpdateRoom*>(message);
	if (!update_data) return 1;
	
	if (!_room || !_room->IsVoiceOpen()) return 2;

	if (update_data->voice_member_id() == GetVoiceMemberID()) return 3; //尚未发生变化

	_player_prop.set_voice_member_id(update_data->voice_member_id());

	_room->SyncRoom();
	
	return 0;
}


int32_t Player::CmdLoadScene(pb::Message* message)
{
	Asset::LoadScene* load_scene = dynamic_cast<Asset::LoadScene*>(message);
	if (!load_scene) return 1;

	switch (load_scene->load_type())
	{
		case Asset::LOAD_SCENE_TYPE_START: //加载开始
		{
			_player_prop.set_load_type(Asset::LOAD_SCENE_TYPE_START);
			_player_prop.set_room_id(load_scene->scene_id()); //进入房间ID
		}
		break;
		
		case Asset::LOAD_SCENE_TYPE_SUCCESS: //加载成功
		{
			if (_player_prop.load_type() != Asset::LOAD_SCENE_TYPE_START) return 2;

			auto room_id = _player_prop.room_id();
			
			auto locate_room = RoomInstance.Get(room_id);
			if (!locate_room) return 3; //非法的房间 

			bool is_reenter = (_room == nullptr ? false : room_id == _room->GetID());
			
			SetRoom(locate_room);
				
			_player_prop.clear_load_type(); 
			_player_prop.clear_room_id(); 
	
			if (_stuff.room_id() > 0 && _stuff.room_id() != room_id)
			{
				LOG(ERROR, "玩家:{}加载房间:{}和保存的房间:{}不一致", _player_id, room_id, _stuff.room_id());
			
				_stuff.set_room_id(room_id); 
						
				SetDirty();
			}
				
			OnEnterScene(is_reenter); //进入房间//场景回调
			
			DEBUG("玩家:{} 进入房间:{}成功.", _player_id, room_id);
		}
		break;
		
		default:
		{

		}
		break;
	}

	return 0;
}

void Player::OnEnterScene(bool is_reenter)
{
	SendPlayer(); //发送数据给Client
	
	if (!is_reenter) ClearCards(); //第一次进房间初始化牌局状态

	if (_room) 
	{
		_room->SyncRoom(); //同步当前房间内玩家数据

		if (is_reenter) _room->OnReEnter(shared_from_this()); //房间重入
	}

	DEBUG("玩家:{} 进入房间, 是否重入:{}", _player_id, is_reenter);
}

int32_t Player::CmdLuckyPlate(pb::Message* message)
{
	auto lucky_plate = dynamic_cast<Asset::PlayerLuckyPlate*>(message);
	if (!lucky_plate) return 1;

	auto asset_message = AssetInstance.Get(lucky_plate->plate_id());
	if (!asset_message) return 2;
	
	auto asset_lucky_plate = dynamic_cast<Asset::LuckyPlate*>(asset_message);
	if (!asset_lucky_plate) return 3;

	auto index = CommonUtil::RandomByWeight(asset_lucky_plate->plates().begin(), asset_lucky_plate->plates().end(), 
		[](const Asset::LuckyPlate_Plate& ele){
			return ele.weight();
	});

	if (index < 0 || index >= asset_lucky_plate->plates().size() || index >= asset_lucky_plate->plates_count()) return 4;

	auto common_reward_id = asset_lucky_plate->plates(index).common_reward_id();
	DeliverReward(common_reward_id); //发奖

	lucky_plate->set_result(index + 1); //Client从1开始
	SendProtocol(lucky_plate);

	return 0;
}

int32_t Player::CmdSaizi(pb::Message* message)
{
	auto saizi = dynamic_cast<Asset::RandomSaizi*>(message);
	if (!saizi) return 1;

	int32_t result = CommonUtil::Random(1, 6);
	saizi->mutable_random_result()->Add(result);

	SendProtocol(saizi);
	return 0;
}
	
/*
bool Player::AddGameRecord(const Asset::GameRecord& record)
{
	if (!_room) return false;

	int64_t room_id = _room->GetID();

	Asset::RoomHistory* room_history = nullptr;

	for (int32_t i = _stuff.room_history().size() - 1; i >= 0; --i)
	{
		if (_stuff.room_history(i).room_id() == room_id)
		{
			room_history = _stuff.mutable_room_history(i);
			break;
		}
	}

	if (!room_history)
	{
		room_history = _stuff.mutable_room_history()->Add();
		room_history->set_room_id(_room->GetID());
		room_history->set_create_time(CommonTimerInstance.GetTime()); //创建时间
		room_history->mutable_options()->CopyFrom(_room->GetOptions());
	}

	auto list = room_history->mutable_list()->Add();
	list->CopyFrom(record);

	SetDirty();

	DEBUG("player_id:{} add game record:{}", _player_id, record.ShortDebugString());

	return true;
}
*/
	
bool Player::AddRoomRecord(int64_t room_id) 
{ 
	Asset::BattleList message;
	for (auto id : _stuff.room_history()) message.mutable_room_list()->Add(id);
	message.mutable_room_list()->Add(room_id);
	SendProtocol(message);

	auto it = std::find(_stuff.room_history().begin(), _stuff.room_history().end(), room_id);
	if (it == _stuff.room_history().end()) 
	{
		_stuff.mutable_room_history()->Add(room_id); 
		_dirty = true; 
		
		DEBUG("玩家:{}增加房间:{}历史战绩", _player_id, room_id);
	}
	
	OnGameOver(); 

	return true;
}

const std::string Player::GetNickName()
{
	auto wechat = GetWechat();

	auto name = wechat.nickname();

	//
	//如果微信名为空，则用基本属性数据
	//
	if (name.empty()) name = GetName();

	return name;
}

const std::string Player::GetHeadImag()
{
	auto wechat = GetWechat();

	return wechat.headimgurl();
}
	
const std::string Player::GetIpAddress()
{
	//if (!_user.has_client_info() || !_user.client_info().has_client_ip())
	//{
		//auto redis = make_unique<Redis>();
		RedisInstance.GetUser(_stuff.account(), _user);
	//}

	return _user.client_info().ip_address();
}

/////////////////////////////////////////////////////
//游戏逻辑定义
/////////////////////////////////////////////////////
std::vector<Asset::PAI_OPER_TYPE> Player::CheckPai(const Asset::PaiElement& pai, int64_t source_player_id)
{
	std::lock_guard<std::mutex> lock(_card_lock);

	std::vector<Asset::PAI_OPER_TYPE> rtn_check;

	if (CheckHuPai(pai, false, false)) 
	{
		DEBUG("玩家:{} 可以胡来自玩家:{} 牌数据:{}", _player_id, source_player_id, pai.ShortDebugString());
		rtn_check.push_back(Asset::PAI_OPER_TYPE_HUPAI);
	}
	if (CheckGangPai(pai, source_player_id)) 
	{
		DEBUG("玩家:{} 可以杠来自玩家:{} 牌数据:{}", _player_id, source_player_id, pai.ShortDebugString());
		rtn_check.push_back(Asset::PAI_OPER_TYPE_GANGPAI);
	}
	if (CheckPengPai(pai)) 
	{
		DEBUG("玩家:{} 可以碰来自玩家:{} 牌数据:{}", _player_id, source_player_id, pai.ShortDebugString());
		rtn_check.push_back(Asset::PAI_OPER_TYPE_PENGPAI);
	}
	if (CheckChiPai(pai)) 
	{
		DEBUG("玩家:{} 可以吃来自玩家:{} 牌数据:{}", _player_id, source_player_id, pai.ShortDebugString());
		rtn_check.push_back(Asset::PAI_OPER_TYPE_CHIPAI);
	}
		
	return rtn_check;
}
	
bool Player::HasPai(const Asset::PaiElement& pai)
{
	if (pai.card_type() <= 0 || pai.card_value() == 0) return false;

	auto type_it = _cards_inhand.find(pai.card_type());
	if (type_it == _cards_inhand.end()) return false;

	auto value_it = std::find(type_it->second.begin(), type_it->second.end(), pai.card_value());
	if (value_it == type_it->second.end()) return false;

	return true;
}

//假定牌是排序过的, 且胡牌规则为 n*AAA+m*ABC+DD
//
//用 A + 点数 表示 万子(A1 表示 1万, 依此类推)
//用 B + 点数 表示 筒子(B1 表示 1筒, 依此类推)
//用 C + 点数 表示 索子(C1 表示 1索, 依此类推)
//
//字只有东西南北中发白:假定用D1-D7表示吧.
//
//算法逻辑: 首张牌用于对子，顺子, 或者三张.
//接下来递归判断剩下牌型是否能和, 注意对子只能用一次.
//
//下面的算法是可以直接判断是否牌型是否和牌的，不局限于14张牌(3n+2即可)

bool Player::CanHuPai(std::vector<Card_t>& cards, bool use_pair)
{
	int32_t size = cards.size();

	if (size <= 2) 
	{
		if (size == 1) return false;	

		if (size == 0) return true;

		if (cards[0] == cards[1])
		{
			Asset::PaiElement zhang;
			zhang.set_card_type((Asset::CARD_TYPE)cards[0]._type);
			zhang.set_card_value(cards[0]._value);

			AddZhang(zhang); //对儿牌

			return true;
		}
		else
		{
			return false;
		}
	}

	bool pair = false; //对子，如果要胡牌，必须只有一个对子
	bool straight = false; //顺子//一套副

	if (!use_pair)
	{
		std::vector<Card_t> sub_cards(cards.begin() + 2, cards.end());
		pair = (cards[0] == cards[1]) && CanHuPai(sub_cards, true);

		if (pair)
		{
			Asset::PaiElement zhang;
			zhang.set_card_type((Asset::CARD_TYPE)cards[0]._type);
			zhang.set_card_value(cards[0]._value);

			AddZhang(zhang); //对儿牌
		}
	}

	//这里有个判断, 如果只剩两张牌而又不是对子肯定不算和牌,跳出是防止下面数组越界。
	//
	//首张牌用以三张, 剩下的牌是否能和牌。
	
	std::vector<Card_t> sub_cards(cards.begin() + 3, cards.end());
	bool trips = (cards[0] == cards[1]) && (cards[1] == cards[2]) && CanHuPai(sub_cards, use_pair); //刻:三个一样的牌

	int32_t card_value = cards[0]._value, card_type = cards[0]._type;

	if (card_value <= 7 && card_type != Asset::CARD_TYPE_FENG && card_type != Asset::CARD_TYPE_JIAN)
	{
		//顺子的第一张牌
		auto first = cards[0];
		//顺子的第二张牌
		auto second = cards[0] + 1;
		//顺子的第三张牌
		auto third = cards[0] + 2;
		//玩家是否真的有这两张牌
		if (std::find(cards.begin(), cards.end(), second) != cards.end() && std::find(cards.begin(), cards.end(), third) != cards.end())
		{
			//去掉用以顺子的三张牌后是否能和牌
			auto it_first = std::find(cards.begin(), cards.end(), first);
			cards.erase(it_first); //删除
			auto it_second = std::find(cards.begin(), cards.end(), second); //由于前面已经删除了元素，索引已经发生了变化，需要重新查找
			cards.erase(it_second); //删除
			auto it_third = std::find(cards.begin(), cards.end(), third); //由于前面已经删除了元素，索引已经发生了变化，需要重新查找
			cards.erase(it_third); //删除

			//顺子
			straight = CanHuPai(cards, use_pair);

			if (straight)
			{
				Asset::ShunZi shunzi;

				auto pai = shunzi.mutable_pai()->Add();
				pai->set_card_type((Asset::CARD_TYPE)first._type);
				pai->set_card_value(first._value);
				
				pai = shunzi.mutable_pai()->Add();
				pai->set_card_type((Asset::CARD_TYPE)second._type);
				pai->set_card_value(second._value);
				
				pai = shunzi.mutable_pai()->Add();
				pai->set_card_type((Asset::CARD_TYPE)third._type);
				pai->set_card_value(third._value);

				AddShunZi(shunzi);
			}
		}
	}
	
	_hu_result.push_back(std::make_tuple(pair, trips, straight));
	
	return pair || trips || straight; //一对、刻或者顺子
}

void Player::AddZhang(const Asset::PaiElement& zhang)
{
	if (!_game || !_room) return;

	_zhang = zhang; //对儿

	//DEBUG("玩家:{} 在房间:{} 局数:{} 中胡牌对儿:{}", _player_id, _room->GetID(), _game->GetID(), _zhang.ShortDebugString());
}
	
void Player::AddShunZi(const Asset::ShunZi& shunzi)
{
	if (!_game || !_room) return;

	_shunzis.push_back(shunzi);

	//DEBUG("玩家:{} 在房间:{} 局数:{} 中胡牌顺子数量:{}，本次增加:{}", _player_id, _room->GetID(), _game->GetID(), _shunzis.size(), shunzi.ShortDebugString());
}
	
bool Player::CheckBaoHu(const Asset::PaiElement& pai/*宝牌数据*/)
{
	if (!_game || !_room) return false;

	if (!IsTingPai()) return false; //没有听牌显然不能胡宝牌

	auto baopai = _game->GetBaoPai();
	if (pai.card_type() != baopai.card_type() || pai.card_value() != baopai.card_value())  return false; //不是宝牌
	
	if (!_room->HasAnbao() && !_room->HasBaopai()) return false;

	if (_cards_hu.size() == 0) return false; //尚不能胡牌

	bool deleted = false; //理论上宝牌玩家都会自摸在手里

	auto it = std::find(_cards_inhand[pai.card_type()].begin(), _cards_inhand[pai.card_type()].end(), pai.card_value());
	if (it != _cards_inhand[pai.card_type()].end()) //宝牌已经在墙内
	{
		_cards_inhand[pai.card_type()].erase(it);
		deleted = true;
	}

	//
	//宝牌多种胡法的多种番型，求解最大番型
	//
	const auto fan_asset = _room->GetFan();

	auto get_multiple = [&](const int32_t fan_type)->int32_t {
		auto it = std::find_if(fan_asset->fans().begin(), fan_asset->fans().end(), [fan_type](const Asset::RoomFan_FanElement& element){
			return fan_type == element.fan_type();
		});
		if (it == fan_asset->fans().end()) return 0;
		return pow(2, it->multiple());
	};

	auto max_fan_score = 1;
	auto max_fan_card = _cards_hu[0];
	auto hupai = CheckHuPai(max_fan_card, false);
	auto max_fan_list = _fan_list;
	for (const auto fan : _fan_list) max_fan_score *= get_multiple(fan); //番数

	for (auto card : _cards_hu) //可以胡的牌
	{
		if (card.card_type() == max_fan_card.card_type() && card.card_value() == max_fan_card.card_value()) continue; //减少后续检查

		hupai = CheckHuPai(card, false); //能否胡牌，理论上一定可以胡牌//杠后可能换听，现有的胡牌可能已经不胡了//再次检查
		if (!hupai) continue;
		
		int32_t score_base = 1;
		for (const auto fan : _fan_list) score_base *= get_multiple(fan); //番数

		if (score_base >= max_fan_score)
		{
			max_fan_score = score_base;
			max_fan_card = card;
			max_fan_list = _fan_list;
		}
	}
	
	hupai = CheckHuPai(max_fan_card, false); //基础番型

	if (deleted) _cards_inhand[pai.card_type()].push_back(pai.card_value());

	return hupai;
}

bool Player::HasYaoJiu()
{
	return HasYaoJiu(_cards_inhand, _cards_outhand, _minggang, _angang, _jiangang, _fenggang);
}

bool Player::HasYaoJiu(const std::map<int32_t, std::vector<int32_t>>& cards_inhand, //玩家手里的牌
		const std::map<int32_t, std::vector<int32_t>>& cards_outhand, //玩家墙外牌
		const std::vector<Asset::PaiElement>& minggang, //明杠
		const std::vector<Asset::PaiElement>& angang, //暗杠
		int32_t jiangang, //旋风杠，本质是明杠
		int32_t fenggang) //旋风杠，本质是暗杠
{
	auto cards = cards_inhand;
	
	for (auto crds : cards_outhand) 
		cards[crds.first].insert(cards[crds.first].end(), crds.second.begin(), crds.second.end());

	for (auto crds : cards)
	{
		if (crds.second.size() == 0) continue;

		if (crds.first == Asset::CARD_TYPE_WANZI || crds.first == Asset::CARD_TYPE_BINGZI || crds.first == Asset::CARD_TYPE_TIAOZI)
		{
			if (std::find(crds.second.begin(), crds.second.end(), 1) != crds.second.end() || 
					(std::find(crds.second.begin(), crds.second.end(), 9) != crds.second.end())) return true;
		}
		
		if (crds.first == Asset::CARD_TYPE_FENG || crds.first == Asset::CARD_TYPE_JIAN) return true;
	}
	
	for (auto gang : minggang)
	{
		if (gang.card_value() == 1 || gang.card_value() == 9 || 
				gang.card_type() == Asset::CARD_TYPE_FENG || gang.card_type() == Asset::CARD_TYPE_JIAN) return true;
	}
	
	for (auto gang : angang)
	{
		if (gang.card_value() == 1 || gang.card_value() == 9 ||
				gang.card_type() == Asset::CARD_TYPE_FENG || gang.card_type() == Asset::CARD_TYPE_JIAN) return true;
	}

	if (jiangang > 0 || fenggang > 0) return true;

	return false;
}
	
bool Player::CheckHuPai(const std::map<int32_t, std::vector<int32_t>>& cards_inhand, //玩家手里的牌
		const std::map<int32_t, std::vector<int32_t>>& cards_outhand, //玩家墙外牌
		const std::vector<Asset::PaiElement>& minggang, //明杠
		const std::vector<Asset::PaiElement>& angang, //暗杠
		int32_t jiangang, //旋风杠，本质是明杠
		int32_t fenggang, //旋风杠，本质是暗杠
		const Asset::PaiElement& pai, //胡牌
		bool check_zimo, //是否自摸
		bool calculate) //是否结算
{
	if (!_room || !_game) return false;
	
	if (calculate) _fan_list.clear(); //番型数据

	_hu_result.clear(); //胡牌数据
	_shunzis.clear(); //顺子数据

	auto cards = cards_inhand;

	for (auto crds : cards_outhand) //复制牌外牌
		cards[crds.first].insert(cards[crds.first].end(), crds.second.begin(), crds.second.end());

	if (!check_zimo) cards[pai.card_type()].push_back(pai.card_value()); //放入可以操作的牌//牌数量检查
	
	for (auto& card : cards)
		std::sort(card.second.begin(), card.second.end(), [](int x, int y){ return x < y; }); //由小到大，排序
	
	bool zhanlihu = false, jiahu = false, bianhu = false, duanmen = false, yise = false, piao = false; //积分

	//
	//1.是否可以缺门、清一色
	//
	{
		int32_t has_count = 0; //万饼条数量

		auto gang_list = minggang;
		gang_list.insert(gang_list.end(), angang.begin(), angang.end());

		//是否含有万子
		auto it_wanzi = std::find_if(gang_list.begin(), gang_list.end(), [](const Asset::PaiElement& pai){
					return pai.card_type() == Asset::CARD_TYPE_WANZI;
				});
		if (cards[Asset::CARD_TYPE_WANZI].size() > 0 || it_wanzi != gang_list.end()) ++has_count;

		//是否含有饼子
		auto it_bingzi = std::find_if(gang_list.begin(), gang_list.end(), [](const Asset::PaiElement& pai){
					return pai.card_type() == Asset::CARD_TYPE_BINGZI;
				});
		if (cards[Asset::CARD_TYPE_BINGZI].size() > 0 || it_bingzi != gang_list.end()) ++has_count; 
		
		//是否含有条子
		auto it_tiaozi = std::find_if(gang_list.begin(), gang_list.end(), [](const Asset::PaiElement& pai){
					return pai.card_type() == Asset::CARD_TYPE_TIAOZI;
				});
		if (cards[Asset::CARD_TYPE_TIAOZI].size() > 0 || it_tiaozi != gang_list.end()) ++has_count; 

		if (!_room->HasDuanMen()) //不可以缺门
		{
			if (has_count < 3) //少于三门显然不行，检查是否清一色
			{
				if (_room->HasQingYiSe()) //可以清一色
				{
					if (has_count == 2) { return false; } //不可缺门
					else { yise = true;} //<= 1//是清一色
				}
				else //断门还不可以清一色
				{
					return false;
				}
			}
		}
		else //可以断门
		{
			if (has_count < 3) duanmen = true;

			if (_room->HasQingYiSe()) //可以清一色
			{
				if (has_count <= 1) 
				{
					yise = true;
					duanmen = false; //清一色不算断门
				}
			}
		}
	}

	//
	//2.是否可以站立胡
	//
	{
		if (_room->HasZhanLi()) //可以站立胡
		{
			if (cards_outhand.size() == 0 && minggang.size() == 0) zhanlihu = true;
		}
		else
		{
			if (cards_outhand.size() == 0 && minggang.size() == 0) return false; //没开门
		}
	}
	
	//
	//3.是否有幺九
	//
	bool has_yao = false;

	for (auto crds : cards) //不同牌类别的牌
	{
		if (crds.second.size() == 0) continue;

		if (crds.first == Asset::CARD_TYPE_WANZI || crds.first == Asset::CARD_TYPE_BINGZI || crds.first == Asset::CARD_TYPE_TIAOZI)
		{
			if (std::find(crds.second.begin(), crds.second.end(), 1) != crds.second.end() || (std::find(crds.second.begin(), crds.second.end(), 9) != crds.second.end())) 
			{
				has_yao = true;
				break;
			}
		}
		
		if (crds.first == Asset::CARD_TYPE_FENG || crds.first == Asset::CARD_TYPE_JIAN) 
		{
			has_yao = true;
			break;
		}
	}

	for (auto gang : minggang)
	{
		if (gang.card_value() == 1 || gang.card_value() == 9 || 
				gang.card_type() == Asset::CARD_TYPE_FENG || gang.card_type() == Asset::CARD_TYPE_JIAN) has_yao = true;
	}
	
	for (auto gang : angang)
	{
		if (gang.card_value() == 1 || gang.card_value() == 9 ||
				gang.card_type() == Asset::CARD_TYPE_FENG || gang.card_type() == Asset::CARD_TYPE_JIAN) has_yao = true;
	}

	if (jiangang > 0 || fenggang > 0) has_yao = true;

	if (!has_yao) return false; //没有幺九不能胡牌

	//
	//4.是否可以满足胡牌的要求
	//
	std::vector<Card_t> card_list;
	for (auto crds : cards) //不同牌类别的牌
	{
		for (auto value : crds.second)
			card_list.push_back(Card_t(crds.first, value));
	}

	bool can_hu = CanHuPai(card_list); //胡牌牌型检查
	if (!can_hu) return false;
	
	//
	//5.是否含有刻
	//
	bool has_ke = false; //胡牌时至少有一刻子或杠或有中发白其中一对
			
	if (HasKeOutHand()) has_ke = true;
	
	int32_t shunzi_count = 0, ke_count = 0; 

	for (auto r : _hu_result)
	{
		 bool is_shunzi = std::get<2>(r);
		 if (is_shunzi) ++shunzi_count;
				 
		 bool is_ke = std::get<1>(r);
		 if (is_ke) ++ke_count;
	}

	if (!has_ke && (/*cards[Asset::CARD_TYPE_FENG].size() //朝阳：两个风牌不顶横 || */cards[Asset::CARD_TYPE_JIAN].size())) has_ke = true;
	
	if (!has_ke && (jiangang > 0 || fenggang > 0 || minggang.size() > 0 || angang.size() > 0)) has_ke = true;
	
	if (!has_ke && ke_count == 0) return false;

	//
	//依赖牌型检查
	//
	if (_room->IsJianPing() && !_room->HasZhang28() && Is28Zhang()) return false; //建平玩法：28不能作掌
	
	//6.飘胡检查，严重依赖上面的刻的数量
	//
	if (shunzi_count == 0) 
	{
		piao = true; 

		if (!IsPiao()) piao = false; //尝试处理：玩家吃了三套副一样的，如[7 7 7 8 8 8 9 9 9]牌型
	}
	
	//
	//7.特殊情况检查
	//
	//防止出现玩家已经碰了6饼，但是牌内含有2、3、4、5、6万，7、8饼
	//
	//收到1、4、7万胡牌的情况
	//
	//这里对牌内牌进行胡牌检查
	//
	{
		//7-1.胡牌检查
		//
		auto cards_inhand_check = cards_inhand;
		if (!check_zimo && pai.card_type() && pai.card_value()) cards_inhand_check[pai.card_type()].push_back(pai.card_value()); //放入可以操作的牌
			
		for (auto& card : cards_inhand_check)
			std::sort(card.second.begin(), card.second.end(), [](int x, int y){ return x < y; }); //由小到大，排序
	
		std::vector<Card_t> card_list;
		for (auto crds : cards_inhand_check) //胡牌逻辑检查
		{
			for (auto value : crds.second)
				card_list.push_back(Card_t(crds.first, value));
		}

		_hu_result.clear();
		_shunzis.clear();

		bool can_hu = CanHuPai(card_list);	
		if (!can_hu) return false;

		//7-2.刻数据检查
		//
		if (!has_ke)
		{
			int32_t ke_count = 0; //刻数最多12张
			for (auto r : _hu_result)
			{
				 bool is_ke = std::get<1>(r);
				 if (is_ke) ++ke_count;
			}
			if (ke_count) has_ke = true; //牌内不能构成则没有刻
		}

		if (_shunzis.size() == 0 && !HasChiPaiOutHand()) piao = true; //牌内没有顺子，处理牌型[3 3 3]，墙外碰[2 2 2 4 4 4]情况
		if (_room->IsJianPing() && !piao && _room->HasMingPiao() && HasPengJianPai()) return false; //建平玩法：中发白其中之一只要碰就算明飘，本局必须胡飘，不勾选则正常
	}

	if (!has_ke) return false;

	//
	//8.建平玩法：四归一不允许胡牌
	//
	do {
		if (!_room->IsJianPing() || !_room->HasSiGuiYi()) break;

		if (!check_zimo && IsSiGuiYi(pai)) return false;
		if (check_zimo && IsSiGuiYi()) return false;

	} while(false);
	
	//
	//9.建平玩法：一边高不允许胡牌
	//
	do {
		if (!_room->IsJianPing() || !_room->HasYiBianGao()) break;

		auto cards_inhand_check = cards_inhand;
		
		for (const auto& shunzi : _shunzis) //牌内顺子_shunzis
		{
			if (shunzi.pai().size() != 3) continue;

			auto count = std::count_if(_shunzis.begin(), _shunzis.end(), [shunzi](const Asset::ShunZi& shunzi_element){
						if (shunzi_element.pai().size() != 3) return false;

						for (int32_t i = 0; i < 3; ++i)
						{
							if (shunzi.pai(i).card_type() != shunzi_element.pai(i).card_type()) return false;
							if (shunzi.pai(i).card_value() != shunzi_element.pai(i).card_value()) return false;
						}

						return true;
					});
			if (count > 1) //牌型[2 2 2 7 7 7 8 9]满足条件但不是一边高
			{
				bool has_pai = true;

				for (const auto& pai : shunzi.pai()) //检查手里是否真的含有这些数量的牌
				{
					auto card_count = std::count_if(cards_inhand_check[pai.card_type()].begin(), cards_inhand_check[pai.card_type()].end(), [pai](int32_t card_value){
								return card_value == pai.card_value();
							});
					if (card_count != count) 
					{
						has_pai = false;
						break;
					}
				}

				if (has_pai) return false; //确实含有这些牌数据
			}
		}

	} while(false);

	if (!calculate) return true;
	
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// 番数数据
//
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	
	//
	//1.是否夹胡
	//
	do {
		if (!_room->HasJiaHu()) break; //不支持夹胡

		auto cards_inhand_check = _cards_inhand;
			
		for (auto& card : cards_inhand_check)
			std::sort(card.second.begin(), card.second.end(), [](int x, int y){ return x < y; }); //由小到大，排序

		if (check_zimo && CheckValidCard(_zhuapai))
		{
			auto it = std::find(cards_inhand_check[_zhuapai.card_type()].begin(), cards_inhand_check[_zhuapai.card_type()].end(), _zhuapai.card_value());
			if (it == cards_inhand_check[_zhuapai.card_type()].end())
			{
				LOG(ERROR, "玩家:{}未能找到牌数据牌类型:{} 牌值:{}", _player_id, _zhuapai.card_type(), _zhuapai.card_value());
				return false;
			}

			cards_inhand_check[_zhuapai.card_type()].erase(it);
		}
	
		std::vector<Card_t> card_list;
		for (auto crds : cards_inhand_check) //玩家牌内胡牌逻辑检查
		{
			for (auto value : crds.second)
				card_list.push_back(Card_t(crds.first, value));
		}

		if (pai.card_value() == 3)
		{
			auto card_list3 = card_list;

			Card_t card_t2(pai.card_type(), pai.card_value() - 1);
			auto it2 = std::find(card_list3.begin(), card_list3.end(), card_t2);

			Card_t card_t1(pai.card_type(), pai.card_value() - 2);
			auto it1 = std::find(card_list3.begin(), card_list3.end(), card_t1);

			if (it1 != card_list3.end() && it2 != card_list3.end())
			{
				it2 = std::find(card_list3.begin(), card_list3.end(), card_t2);
				card_list3.erase(it2);
				it1 = std::find(card_list3.begin(), card_list3.end(), card_t1);
				card_list3.erase(it1);
				
				bianhu = CanHuPai(card_list3);	
			}
		}
		else if (pai.card_value() == 7)
		{
			auto card_list7 = card_list;

			Card_t card_t8(pai.card_type(), pai.card_value() + 1);
			auto it8 = std::find(card_list7.begin(), card_list7.end(), card_t8);

			Card_t card_t9(pai.card_type(), pai.card_value() + 2);
			auto it9 = std::find(card_list7.begin(), card_list7.end(), card_t9);
			
			if (it8 != card_list7.end() && it9 != card_list7.end())
			{
				it8 = std::find(card_list7.begin(), card_list7.end(), card_t8);
				card_list7.erase(it8);
				it9 = std::find(card_list7.begin(), card_list7.end(), card_t9);
				card_list7.erase(it9);
				
				bianhu = CanHuPai(card_list7);	
			}
		}
		//
		//防止牌型6 7 8 9 9情况,边胡和夹胡判断后,删除了8 9牌值,后面夹胡不成立情况
		//
		if (!bianhu)
		{
			Card_t card_t_small(pai.card_type(), pai.card_value() - 1);
			auto it_small = std::find(card_list.begin(), card_list.end(), card_t_small);

			Card_t card_t_big(pai.card_type(), pai.card_value() + 1);
			auto it_big = std::find(card_list.begin(), card_list.end(), card_t_big);
			
			if (it_small != card_list.end() && it_big != card_list.end())
			{
				it_small = std::find(card_list.begin(), card_list.end(), card_t_small);
				card_list.erase(it_small);
				it_big = std::find(card_list.begin(), card_list.end(), card_t_big);
				card_list.erase(it_big);
				
				jiahu = CanHuPai(card_list); //夹胡
			}
		}

	} while(false);

	if (bianhu) _fan_list.emplace(Asset::FAN_TYPE_JIA_HU_NORMAL); //边胡
	if (jiahu) _fan_list.emplace(Asset::FAN_TYPE_JIA_HU_NORMAL); //夹胡
	if (zhanlihu) _fan_list.emplace(Asset::FAN_TYPE_ZHAN_LI); //站立胡
	if (duanmen) _fan_list.emplace(Asset::FAN_TYPE_DUAN_MEN);//断门
	if (yise) _fan_list.emplace(Asset::FAN_TYPE_QING_YI_SE); //清一色
	if (piao) _fan_list.emplace(Asset::FAN_TYPE_PIAO_HU); //飘胡
	if (_game->IsLiuJu()) _fan_list.emplace(Asset::FAN_TYPE_HAI_DI_LAO); //海底捞月

	if (_room->IsJianPing() && IsDanDiao(pai, check_zimo)) _fan_list.emplace(Asset::FAN_TYPE_JIA_HU_NORMAL); //单调//单粘//夹胡
	if (_room->IsJianPing() && !HasYaoJiu()) _fan_list.emplace(Asset::FAN_TYPE_JIA_HU_NORMAL); //缺19的时候死胡19
	if (_room->IsJianPing() && !_room->HasZhang28() && Is28Pai(pai) && IsDuiDao(pai, check_zimo)) _fan_list.emplace(Asset::FAN_TYPE_JIA_HU_NORMAL); //对儿倒其一为28的情况且不能28作掌儿
	
	if (IsMingPiao()) 
	{
		_fan_list.emplace(Asset::FAN_TYPE_MING_PIAO); //明飘
		if (_room->IsJianPing()) 
		{
			_fan_list.erase(Asset::FAN_TYPE_PIAO_HU); //建平玩法：明飘不带飘胡番数
			_fan_list.erase(Asset::FAN_TYPE_JIA_HU_NORMAL); //建平玩法：明飘不带夹胡番数
		}
	}

	return true;
}

bool Player::CheckZiMo(bool calculate)
{
	if (!CheckHuCardsInhand()) return false; //胡牌数量检查

	Asset::PaiElement pai;
	return CheckZiMo(pai, calculate);
}

bool Player::CheckZiMo(const Asset::PaiElement& pai, bool calculate)
{
	if (!_room || !_game) return false;

	if (_tuoguan_server) return false;

	return CheckHuPai(pai, true, calculate);
}
	
bool Player::CheckHuPai(const Asset::PaiElement& pai, bool check_zimo, bool calculate)
{
	if (!_room || !_game) return false;

	auto cards_inhand = _cards_inhand; //玩家手里牌
	auto cards_outhand = _cards_outhand; //玩家墙外牌
	auto minggang = _minggang; //明杠
	auto angang = _angang; //暗杠
	auto jiangang = _jiangang; //旋风杠，本质是明杠
	auto fenggang = _fenggang; //旋风杠，本质是暗杠

	return CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai, check_zimo, calculate);
}
	
bool Player::IsMingPiao()
{
	auto curr_count = GetCardCount();

	return curr_count == 1 || curr_count == 2;
}

bool Player::HasKeOutHand()
{
	for (const auto& cards : _cards_outhand)
	{
		if (cards.second.size() == 0) continue;
		if (cards.second.size() % 3 != 0) return false;

		for (size_t i = 0; i < cards.second.size(); i = i + 3)
		{
			auto card_value = cards.second.at(i);
			if ((card_value == cards.second.at(i + 1)) && (card_value == cards.second.at(i + 2))) return true; 
		}
	}

	return false;
}

bool Player::HasChiPaiOutHand()
{
	for (const auto& cards : _cards_outhand)
	{
		if (cards.second.size() == 0) continue;
		if (cards.second.size() % 3 != 0) return false;

		for (size_t i = 0; i < cards.second.size(); i = i + 3)
		{
			auto card_value = cards.second.at(i);
			if ((card_value != cards.second.at(i + 1)) || (card_value != cards.second.at(i + 2))) return true; 
		}
	}

	return false;
}

bool Player::HasPengJianPai()

{
	const auto& cards_outhand = _cards_outhand;

	for (auto cards : cards_outhand)
	{
		if (cards.second.size() == 0) continue;
		if (cards.first == Asset::CARD_TYPE_JIAN) return true; //箭牌没法吃，只能是碰牌
	}

	return false;
}

bool Player::IsPiao()
{
	const auto& cards_outhand = _cards_outhand;

	for (auto cards : cards_outhand)
	{
		if (cards.second.size() == 0) continue;
		if (cards.second.size() % 3 != 0) return false;

		for (size_t i = 0; i < cards.second.size(); i = i + 3)
		{
			auto card_value = cards.second.at(i);
			if ((card_value != cards.second.at(i + 1)) || (card_value != cards.second.at(i + 2))) return false; //外面是否碰了3张
		}
	}

	return true;
}
	
//
//是否四归一 
//
//胡牌时，非杠牌可以组成4张相同牌
//
//比如牌里面有一万、二万、二万、二万、二万、三万，
//
//凑成一万、二万、三万一顺，二万、二万、二万一刻，这时候就是四归一了
//
bool Player::IsSiGuiYi()
{
	auto cards = _cards_inhand;
	for (const auto& crds : _cards_outhand) 
		cards[crds.first].insert(cards[crds.first].end(), crds.second.begin(), crds.second.end());

	for (const auto& crds : cards)
	{
		for (auto card_value : crds.second)
		{
			auto count = std::count(crds.second.begin(), crds.second.end(), card_value);
			if (count == 4) return true;
		}
	}

	return false;
}

bool Player::IsSiGuiYi(const Asset::PaiElement& pai)
{
	auto cards = _cards_inhand;
	for (const auto& crds : _cards_outhand) 
		cards[crds.first].insert(cards[crds.first].end(), crds.second.begin(), crds.second.end());

	cards[pai.card_type()].push_back(pai.card_value());

	for (const auto& crds : cards)
	{
		for (auto card_value : crds.second)
		{
			auto count = std::count(crds.second.begin(), crds.second.end(), card_value);
			if (count == 4) return true;
		}
	}

	return false;
}

//
//是否单粘
//
//胡牌只能是这一张
//
bool Player::IsDanDiao(const Asset::PaiElement& pai, bool check_zimo) 
{ 
	if (_cards_hu.size() == 1) return true;

	auto cards_inhand = _cards_inhand; //玩家手里牌
	auto cards_outhand = _cards_outhand; //玩家墙外牌
	auto minggang = _minggang; //明杠
	auto angang = _angang; //暗杠
	auto jiangang = _jiangang; //旋风杠，本质是明杠
	auto fenggang = _fenggang; //旋风杠，本质是暗杠

	if (check_zimo) 
	{
		auto it = std::find_if(cards_inhand[pai.card_type()].begin(), cards_inhand[pai.card_type()].end(), [pai](int32_t card_value){
					return pai.card_value() == card_value;
				});
		if (it == cards_inhand[pai.card_type()].end()) return false;

		cards_inhand[pai.card_type()].erase(it); //如果是自摸胡牌，则删除检查其他牌是否能胡
	}

	const auto& cards = GameInstance.GetPais();

	for (const auto& card : cards)
	{
		if (card.card_type() == pai.card_type() && card.card_value() == pai.card_value()) continue;

		auto can_hupai = CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, card, false, false);
		if (can_hupai) return false;
	}

	return true;
} 

//
//是否对倒//胡牌两对
//
//方法：掌儿作为胡牌牌值，pai作为掌儿，检查是否依然能够胡牌
//
bool Player::IsDuiDao(const Asset::PaiElement& pai, bool check_zimo) 
{ 
	auto cards_inhand_check = _cards_inhand;
	if (!check_zimo && pai.card_type() && pai.card_value()) cards_inhand_check[pai.card_type()].push_back(pai.card_value()); //放入可以操作的牌
		
	for (auto& card : cards_inhand_check)
		std::sort(card.second.begin(), card.second.end(), [](int x, int y){ return x < y; }); //由小到大，排序

	std::vector<Card_t> card_list;
	for (auto crds : cards_inhand_check) //胡牌逻辑检查
	{
		for (auto value : crds.second)
			card_list.push_back(Card_t(crds.first, value));
	}

	bool can_hu = CanHuPai(card_list); //生成掌儿，赋值_zhang	
	if (!can_hu) return false;
		
	cards_inhand_check = _cards_inhand; //再次检查胡牌，获取掌儿

	if (check_zimo)
	{
		auto it = std::find_if(cards_inhand_check[pai.card_type()].begin(), cards_inhand_check[pai.card_type()].end(), [pai](int32_t value){
					return pai.card_value() == value;
				});
		if (it == cards_inhand_check[pai.card_type()].end()) return false;

		cards_inhand_check[pai.card_type()].erase(it); //删除这张牌
	}

	cards_inhand_check[_zhang.card_type()].push_back(_zhang.card_value()); //放入掌儿，如果还能胡牌，且掌儿是胡牌牌值

	for (auto& card : cards_inhand_check)
		std::sort(card.second.begin(), card.second.end(), [](int x, int y){ return x < y; }); //由小到大，排序

	card_list.clear();

	for (auto crds : cards_inhand_check) //胡牌逻辑检查
	{
		for (auto value : crds.second)
			card_list.push_back(Card_t(crds.first, value));
	}

	can_hu = CanHuPai(card_list); //生成掌儿，赋值_zhang
	if (!can_hu) return false;

	if (pai.card_type() != _zhang.card_type() || pai.card_value() != _zhang.card_value()) return false;

	return true;
} 
	
//
//1.杠->打牌->点炮 流泪
//
//2.杠->胡牌 杠上开
//
//检查的都是上一把是否是杠操作
//
bool Player::IsGangOperation()
{
	if (_last_oper_type == Asset::PAI_OPER_TYPE_GANGPAI || _last_oper_type == Asset::PAI_OPER_TYPE_ANGANGPAI 
			|| _last_oper_type == Asset::PAI_OPER_TYPE_XUANFENG_FENG) 
		return true;
	return false;
}
	
//
//朝阳特殊玩法
//
//如果不是明飘，不能手把一，即单调一张牌
//
bool Player::CheckMingPiao(const Asset::PAI_OPER_TYPE& oper_type)
{
	if (!_room || !_game) return false;

	auto curr_count = GetCardCount();
	
	if (Asset::PAI_OPER_TYPE_ANGANGPAI == oper_type)
	{
		if (curr_count > 5) return true; //当前超过5张牌，暗杠不可能手把一
	}
	else
	{
		if (curr_count > 4) return true; //当前超过4张牌，显然不可能手把一
	}
	

	switch (oper_type)
	{
		case Asset::PAI_OPER_TYPE_CHIPAI: //吃牌
		{
			return false;
		}
		break;
		
		case Asset::PAI_OPER_TYPE_PENGPAI: //碰牌
		case Asset::PAI_OPER_TYPE_GANGPAI: //明杠牌
		case Asset::PAI_OPER_TYPE_ANGANGPAI: //暗杠牌(唯一可能是5张牌)
		{
			//
			//牌外必须都是碰的3张一样
			//
			for (auto cards : _cards_outhand)
			{
				for (auto it = cards.second.begin(); it != cards.second.end(); it += 3)
				{
					auto first_value = *it;
					auto second_value = *(it + 1);
					auto third_value = *(it + 2);
					
					if (first_value != second_value || first_value != third_value || second_value != third_value) return false;
				}
			}
		}
		break;

		default:
		{
			return true;
		}
		break;
	}

	return true;
}

bool Player::CheckChiPai(const Asset::PaiElement& pai)
{
	if (!_room || !_game) return false;

	if (_has_ting || _tuoguan_server) return false; //已经听牌，不再提示

	if (ShouldDaPai()) return false;

	if (!CheckMingPiao(Asset::PAI_OPER_TYPE_CHIPAI)) return false; //明飘检查

	if (_room->HasMingPiao() && HasPengJianPai()) return false; //建平玩法：碰完必须明飘，吃了就不让碰了

	auto it = _cards_inhand.find(pai.card_type());
	if (it == _cards_inhand.end()) return false;

	if (pai.card_type() != Asset::CARD_TYPE_WANZI && pai.card_type() != Asset::CARD_TYPE_BINGZI &&
			pai.card_type() != Asset::CARD_TYPE_TIAOZI) return false; //万子牌、饼子牌和条子牌才能吃牌

	int32_t card_value = pai.card_value();

	//吃牌总共有有三种方式:
	//
	//比如上家出4万，可以吃的条件是：2/3; 5/6; 3/5 三种方法.
	
	if (std::find(it->second.begin(), it->second.end(), card_value - 1) != it->second.end() && 
		std::find(it->second.begin(), it->second.end(), card_value - 2) != it->second.end()) return true; 
	
	if (std::find(it->second.begin(), it->second.end(), card_value + 1) != it->second.end() && 
		std::find(it->second.begin(), it->second.end(), card_value + 2) != it->second.end()) return true; 

	if (std::find(it->second.begin(), it->second.end(), card_value - 1) != it->second.end() && 
		std::find(it->second.begin(), it->second.end(), card_value + 1) != it->second.end()) return true; 

	return false;
}

void Player::OnChiPai(const Asset::PaiElement& pai, pb::Message* message)
{
	PrintPai(); //打印玩家当前手里的牌数据

	if (!CheckChiPai(pai) || !message) 
	{
		LOG(ERROR, "玩家:{}不能吃牌，原因:没有牌能满足吃牌，类型:{} 牌值:{}", _player_id, pai.card_type(), pai.card_value());
		return;
	}

	Asset::PaiOperation* pai_operate = dynamic_cast<Asset::PaiOperation*>(message);
	if (!pai_operate) return;
	
	std::vector<Asset::PaiElement> cards;
	cards.push_back(pai);
		
	auto pais = *pai_operate;

	if (pai_operate->pais().size() != 2) 
	{
		LOG(ERROR, "玩家:{}不能吃牌，原因:吃牌需要2张，类型:{} 牌值:{}, 数据:{}", _player_id, pai.card_type(), pai.card_value(), pais.ShortDebugString());
		return; 
	}
	
	auto it = _cards_inhand.find(pai.card_type());
	if (it == _cards_inhand.end()) return;
		
	for (const auto& p : pai_operate->pais())	
	{
		if (pai.card_type() != p.card_type()) return; //牌类型不一致

		cards.push_back(p);
	}

	std::sort(cards.begin(), cards.end(), [](const Asset::PaiElement& rv, const Asset::PaiElement& lv) {
			return rv.card_value() < lv.card_value();
		});

	if (cards[1].card_value() - cards[0].card_value() != 1 || cards[2].card_value() - cards[1].card_value() != 1) 
	{
		LOG(ERROR, "玩家:{}不能吃牌，原因:不是顺子，类型:{} 牌值:{}, 数据:{}", _player_id, pai.card_type(), pai.card_value(), pais.ShortDebugString());
		return; //不是顺子
	}

	auto first = std::find(it->second.begin(), it->second.end(), pai_operate->pais(0).card_value());
	if (first == it->second.end()) 
	{
		DEBUG_ASSERT(false);
		return; //理论上不会出现
	}
	
	it->second.erase(first); //删除

	auto second = std::find(it->second.begin(), it->second.end(), pai_operate->pais(1).card_value());
	if (second == it->second.end()) 
	{
		DEBUG_ASSERT(false);
		return; //理论上不会出现
	}

	it->second.erase(second); //删除

	for (const auto& card : cards)
	{
		_cards_outhand[card.card_type()].push_back(card.card_value());

		if (card.card_type() != pai.card_type() || card.card_value() != pai.card_value()) _game->Add2CardsPool(card.card_type(), card.card_value()); //加入牌池
	}

	SynchronizePai();
}

bool Player::CheckPengPai(const Asset::PaiElement& pai)
{
	if (!_room || !_game) return false;

	if (_has_ting || _tuoguan_server) return false; //已经听牌，不再提示
	
	if (ShouldDaPai()) return false;
	
	if (!CheckMingPiao(Asset::PAI_OPER_TYPE_PENGPAI)) return false; //明飘检查

	if (_room->HasMingPiao() && pai.card_type() == Asset::CARD_TYPE_JIAN && HasChiPaiOutHand()) return false; //建平玩法：碰完必须明飘，吃了就不让碰了

	auto it = _cards_inhand.find(pai.card_type());
	if (it == _cards_inhand.end()) return false;

	int32_t card_value = pai.card_value();
	int32_t count = std::count_if(it->second.begin(), it->second.end(), [card_value](int32_t value) { return card_value == value; });

	if (count < 2) return false;
	
	return true;
}

void Player::OnPengPai(const Asset::PaiElement& pai)
{
	if (!_game || !_room) return;
	
	PrintPai(); //打印玩家当前手里的牌数据

	if (!CheckPengPai(pai)) 
	{
		LOG(ERROR, "玩家:{}无法碰牌:{}", _player_id, pai.ShortDebugString());
		return;
	}
	
	auto it = _cards_inhand.find(pai.card_type());
	if (it == _cards_inhand.end()) return; //理论上不会如此
	
	for (int i = 0; i < 2; ++i)
	{
		auto iit = std::find(it->second.begin(), it->second.end(), pai.card_value()); //从玩家手里删除
		if (iit == it->second.end()) return;

		it->second.erase(iit);
	}

	for (int i = 0; i < 3; ++i) _cards_outhand[pai.card_type()].push_back(pai.card_value());
	for (int i = 0; i < 2; ++i) _game->Add2CardsPool(pai); //加入牌池
		
	SynchronizePai();
}

bool Player::CheckGangPai(const Asset::PaiElement& pai, int64_t source_player_id)
{
	if (!_room || !_game) return false;

	if (_tuoguan_server) return false;
	
	//if (ShouldDaPai()) return false;

	auto cards_inhand = _cards_inhand; //玩家手里牌
	auto cards_outhand = _cards_outhand; //玩家墙外牌
	auto minggang = _minggang; //明杠
	auto angang = _angang; //暗杠
	auto jiangang = _jiangang; //旋风杠，本质是明杠
	auto fenggang = _fenggang; //旋风杠，本质是暗杠

	auto has_gang = false; //是否含有杠牌

	auto it = cards_inhand.find(pai.card_type());
	int32_t card_value = pai.card_value();

	if (it != cards_inhand.end()) 
	{
		int32_t count = std::count(it->second.begin(), it->second.end(), card_value);
		if (count == 3 && CheckMingPiao(Asset::PAI_OPER_TYPE_GANGPAI))
		{
			has_gang = true;  
			minggang.push_back(pai);
		}
		else if (count == 4 && CheckMingPiao(Asset::PAI_OPER_TYPE_ANGANGPAI)) 
		{
			if (_room->IsJianPing() && IsBimen()) return false; //建平玩法：不开门不让扣暗杠

			has_gang = true;  
			angang.push_back(pai);
		}
		else if (count == 1) //手里一张，杠后杠
		{
			source_player_id = _player_id;
		}
	}

	if (!CheckMingPiao(Asset::PAI_OPER_TYPE_GANGPAI)) return false; //明飘检查
	
	if (!has_gang && source_player_id == _player_id) 
	{
		auto it = cards_outhand.find(pai.card_type()); //牌面的牌不做排序,顺序必须3张
		if (it == cards_outhand.end()) return false;

		if (it->second.size() % 3 != 0) return false;

		auto first_it = std::find(it->second.begin(), it->second.end(), card_value);
		if (first_it == it->second.end()) return false;

		auto second_it = ++first_it;
		if (second_it == it->second.end()) return false;

		auto third_it = ++second_it;
		if (third_it == it->second.end()) return false;
		
		if ((card_value == *first_it) && (*first_it == *second_it) && (*second_it == *third_it)) 
		{
			has_gang = true;  //玩家牌面有3张牌
			minggang.push_back(pai);
		}
	}
			
	if (has_gang)
	{
		auto remove_it = std::remove(it->second.begin(), it->second.end(), card_value); //删除杠牌
		if (remove_it != it->second.end()) it->second.erase(remove_it, it->second.end());
	}

	//
	//防止杠后听牌牌型不一致的场景
	//
	if (has_gang && HasTingPai()) 
	{
		has_gang = CanTingPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang); 
	}

	return has_gang;
}

bool Player::CanTingIfGang(const Asset::PaiElement& pai)
{
	if (_tuoguan_server) return false;

	if (!CheckMingPiao(Asset::PAI_OPER_TYPE_GANGPAI)) return false; //明飘检查
	
	auto cards_inhand = _cards_inhand; //玩家手里牌
	auto cards_outhand = _cards_outhand; //玩家墙外牌
	auto minggang = _minggang; //明杠
	auto angang = _angang; //暗杠
	auto jiangang = _jiangang; //旋风杠，本质是明杠
	auto fenggang = _fenggang; //旋风杠，本质是暗杠

	auto it = cards_inhand.find(pai.card_type());
	if (it == cards_inhand.end()) return false;

	int32_t card_value = pai.card_value();

	auto remove_it = std::remove(it->second.begin(), it->second.end(), card_value); //删除杠牌
	it->second.erase(remove_it, it->second.end());

	angang.push_back(pai);
	//
	//防止杠后听牌牌型不一致的场景
	//
	return CanTingPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang); 
}

//
//检查玩家可能的杠牌
//
//比如,玩家碰3个4饼,牌内4 5 6饼套副条件下不杠的情况,每次进行检查
//
bool Player::CheckAllGangPai(::google::protobuf::RepeatedField<Asset::PaiOperationAlert_AlertElement>& gang_list)
{
	if (!_room || !_game) return false;

	if (!CheckMingPiao(Asset::PAI_OPER_TYPE_GANGPAI)) return false; //明飘检查

	//
	//牌内检查
	//
	//有4张牌，即暗杠检查
	//
	for (auto cards : _cards_inhand)
	{
		auto card_type = cards.first;

		for (auto card_value : cards.second)
		{
			auto count = std::count(cards.second.begin(), cards.second.end(), card_value);
			if (count == 4 && CheckMingPiao(Asset::PAI_OPER_TYPE_ANGANGPAI)) 
			{
				if (_room->IsJianPing() && IsBimen()) continue; //建平玩法：不开门不让扣暗杠

				Asset::PaiElement pai;
				pai.set_card_type((Asset::CARD_TYPE)card_type);
				pai.set_card_value(card_value);

				if (HasTingPai() && !CanTingIfGang(pai)) continue; //防止杠后听牌不一致

				auto it = std::find_if(gang_list.begin(), gang_list.end(), [card_type, card_value](const Asset::PaiOperationAlert_AlertElement& element){
					return card_type == element.pai().card_type() && card_value == element.pai().card_value();
				});

				if (it == gang_list.end()) //暗杠
				{
					auto gang = gang_list.Add();
					gang->mutable_pai()->CopyFrom(pai); 
					gang->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_ANGANGPAI);
				}
			}
		}
	}

	//
	//牌外检查
	//
	//牌内有1张牌，牌外有3张碰牌，即明杠检查
	//
	for (auto cards : _cards_outhand)
	{
		if (cards.second.size() == 0) continue;

		if (cards.second.size() % 3 != 0) return false;

		int32_t card_type = cards.first;

		for (size_t i = 0; i < cards.second.size(); i = i + 3)
		{
			auto card_value = cards.second.at(i);

			if ((card_value != cards.second.at(i + 1)) || (card_value != cards.second.at(i + 2))) continue; //外面是否碰了3张

			auto it = _cards_inhand.find(card_type);
			if (it == _cards_inhand.end()) continue;
			
			auto iit = std::find(it->second.begin(), it->second.end(), card_value);
			if (iit == it->second.end()) continue; //手里有一张才满足
			
			Asset::PaiElement pai;
			pai.set_card_type((Asset::CARD_TYPE)card_type);
			pai.set_card_value(card_value);
			pai.set_source_player_id(_player_id);

			if (HasTingPai() && !CanTingIfGang(pai)) continue; //防止杠后听牌不一致
			
			auto gang = gang_list.Add();
			gang->mutable_pai()->CopyFrom(pai); 
			gang->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_GANGPAI);
		}
	}

	return gang_list.size() > 0;
}
	
void Player::OnGangPai(const Asset::PaiElement& pai, int64_t source_player_id)
{
	PrintPai(); 

	if (!CheckGangPai(pai, source_player_id)) 
	{
		LOG(ERROR, "玩家:{} 无法杠牌:{} 牌来自:{}", _player_id, pai.ShortDebugString(), source_player_id);
		return; //理论上不会如此
	}
	
	int32_t card_type = pai.card_type();
	int32_t card_value = pai.card_value();
		
	//
	//1.手里满足杠牌
	//
	auto it = _cards_inhand.find(card_type);
	if (it == _cards_inhand.end()) 
	{
		LOG(ERROR, "玩家:{} 无法杠牌:{} 牌来自:{}", _player_id, pai.ShortDebugString(), source_player_id);
		return; //理论上不会如此
	}
	
	auto count = std::count(it->second.begin(), it->second.end(), card_value); //玩家手里多少张牌

	if (count == 3)
	{
		auto ming_gang = pai;
		ming_gang.set_source_player_id(source_player_id);

		_minggang.push_back(ming_gang); //明杠
	}
	else if (count == 4)
	{
		if (_room->IsJianPing() && IsBimen()) 
		{
			LOG(ERROR, "玩家:{} 尚未开门，无法杠牌:{} 牌来自:{}", _player_id, pai.ShortDebugString(), source_player_id);
			return; //建平玩法：不开门不让扣暗杠
		}
		_angang.push_back(pai); //暗杠
	}
	
	auto remove_it = std::remove(it->second.begin(), it->second.end(), card_value); //从玩家手里删除
	it->second.erase(remove_it, it->second.end());
	
	//
	//2.墙外满足杠牌
	//
	auto iit = _cards_outhand.find(card_type);
	if (iit != _cards_outhand.end()) 
	{
		auto count = std::count(iit->second.begin(), iit->second.end(), card_value); //玩家手里多少张牌

		if (count == 3)
		{
			auto ming_gang = pai;
			ming_gang.set_source_player_id(source_player_id);

			_minggang.push_back(ming_gang);
			
			auto remove_it = std::remove(iit->second.begin(), iit->second.end(), card_value); //从墙外删除
			iit->second.erase(remove_it, iit->second.end());
		}
	}
					
	for (int32_t i = 0; i < count; ++i) _game->Add2CardsPool(pai); //加入牌池
}

void Player::OnBeenQiangGang(const Asset::PaiElement& pai, int64_t source_player_id)
{
	PrintPai(); 

	if (!CheckGangPai(pai, source_player_id)) return; //理论上不会如此
	
	int32_t card_type = pai.card_type();
	int32_t card_value = pai.card_value();

	//手里满足杠牌
	auto it = _cards_inhand.find(card_type);
	if (it == _cards_inhand.end()) return; //理论上不会如此
	
	auto count = std::count(it->second.begin(), it->second.end(), card_value); //玩家手里多少张牌
	
	auto remove_it = std::remove(it->second.begin(), it->second.end(), card_value); //从玩家手里删除
	it->second.erase(remove_it, it->second.end());
	
	//墙外满足杠牌
	auto iit = _cards_outhand.find(card_type);
	if (iit != _cards_outhand.end()) 
	{
		auto count = std::count(iit->second.begin(), iit->second.end(), card_value); //玩家手里多少张牌

		if (count == 3)
		{
			auto remove_it = std::remove(iit->second.begin(), iit->second.end(), card_value); //从墙外删除
			iit->second.erase(remove_it, iit->second.end());
		}
	}
					
	for (int32_t i = 0; i < count; ++i) _game->Add2CardsPool(pai); //加入牌池
}
	
void Player::OnBeenQiangGangWithGivingUp(const Asset::PaiElement& pai, int64_t source_player_id)
{
	auto ming_gang = pai;
	ming_gang.set_source_player_id(source_player_id);

	_minggang.push_back(ming_gang); //明杠
}

bool Player::CheckFengGangPai() 
{ 
	if (!_room || !_game) return false;

	//if (_oper_count >= 1) return false;

	return CheckFengGangPai(_cards_inhand); 
}

bool Player::CheckJianGangPai() 
{ 
	if (!_room || !_game) return false;

	//if (_oper_count >= 1) return false;

	return CheckJianGangPai(_cards_inhand); 
}
	
int32_t Player::CheckXuanFeng()
{
	auto size = _xf_gang.size();
	if (size == 0) return 0;

	//
	//玩家手牌中发白白
	//
	//初始上家打了白板，玩家碰白板后不具备旋风杠条件
	//
	if (!CheckFengGangPai() && !CheckJianGangPai()) 
	{
		_xf_gang.clear();
		return 0; 
	}

	if (!CheckFengGangPai()) //防止已经杠出的牌重新提示
	{
		auto remove_it = std::remove(_xf_gang.begin(), _xf_gang.end(), Asset::PAI_OPER_TYPE_XUANFENG_FENG); //删除杠牌
		if (remove_it != _xf_gang.end()) _xf_gang.erase(remove_it, _xf_gang.end());
	}
	
	if (!CheckJianGangPai()) //防止已经杠出的牌重新提示
	{
		auto remove_it = std::remove(_xf_gang.begin(), _xf_gang.end(), Asset::PAI_OPER_TYPE_XUANFENG_JIAN); //删除杠牌
		if (remove_it != _xf_gang.end()) _xf_gang.erase(remove_it, _xf_gang.end());
	}
	
	size = _xf_gang.size();
	if (size == 0) return 0;

	auto it = _xf_gang.begin();
	auto gang = *it;

	_xf_gang.erase(it);

	return gang;
}

void Player::CheckXuanFengGang()
{
	auto xf_card = _cards_inhand;

	while (CheckFengGangPai(xf_card)) //风牌检查
	{
		_xf_gang.push_back(Asset::PAI_OPER_TYPE_XUANFENG_FENG);

		auto it = xf_card.find(Asset::CARD_TYPE_FENG);
		if (it == xf_card.end()) continue;

		for (auto card_value = 1; card_value <= 4; ++card_value) //东南西北
		{
			auto it_if = std::find(it->second.begin(), it->second.end(), card_value);
			if (it_if != it->second.end()) it->second.erase(it_if); //删除一个
		}
	}
	while (CheckJianGangPai(xf_card)) //箭牌检查
	{
		_xf_gang.push_back(Asset::PAI_OPER_TYPE_XUANFENG_JIAN);

		auto it = xf_card.find(Asset::CARD_TYPE_JIAN);
		if (it == xf_card.end()) continue;

		for (auto card_value = 1; card_value <= 3; ++card_value) //中发白
		{
			auto it_if = std::find(it->second.begin(), it->second.end(), card_value);
			if (it_if != it->second.end()) it->second.erase(it_if); //删除一个
		}
	}
}

//玩家能不能听牌的检查
//
//玩家打出一张牌后，查看玩家再拿到一张牌后可以胡牌

bool Player::CanTingPai(const Asset::PaiElement& pai)
{
	if (!_room || !_game) return false;

	if (!_room->HasAnbao() && !_room->HasBaopai()) return false;
	
	std::vector<Asset::PaiElement> cards_hu;

	auto cards_inhand = _cards_inhand; //玩家手里牌
	auto cards_outhand = _cards_outhand; //玩家墙外牌
	auto minggang = _minggang; //明杠
	auto angang = _angang; //暗杠
	auto jiangang = _jiangang; //旋风杠，本质是明杠
	auto fenggang = _fenggang; //旋风杠，本质是暗杠

	auto find_it = std::find(cards_inhand[pai.card_type()].begin(), cards_inhand[pai.card_type()].end(), pai.card_value());
	if (find_it == cards_inhand[pai.card_type()].end()) 
	{
		DEBUG_ASSERT(false);
		return false; //理论上一定能找到
	}
			
	cards_inhand[pai.card_type()].erase(find_it); //删除这张牌

	/*
	//能否胡万饼条
	for (int card_type = Asset::CARD_TYPE_WANZI; card_type <= Asset::CARD_TYPE_TIAOZI; ++card_type)
	{
		for (int card_value = 1; card_value <= 9; ++card_value)
		{
			Asset::PaiElement pai;
			pai.set_card_type((Asset::CARD_TYPE)card_type);
			pai.set_card_value(card_value);

			if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai)) 
				cards_hu.push_back(pai);
		}
	}
	
	//能否胡风牌
	for (int card_value = 1; card_value <= 4; ++card_value)
	{
		Asset::PaiElement pai;
		pai.set_card_type(Asset::CARD_TYPE_FENG);
		pai.set_card_value(card_value);

		if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai))
			cards_hu.push_back(pai);
	}

	//能否胡箭牌
	for (int card_value = 1; card_value <= 3; ++card_value)
	{
		Asset::PaiElement pai;
		pai.set_card_type(Asset::CARD_TYPE_JIAN);
		pai.set_card_value(card_value);

		if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai)) 
			cards_hu.push_back(pai);
	}
	*/
	const auto& cards = GameInstance.GetPais();

	for (const auto& pai : cards)
	{
		if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai, false, false)) cards_hu.push_back(pai);
	}

	if (_cards_hu.size() > 0 && cards_hu.size() > 0)
	{
		/*
		 * 听牌减少了可以杠
		 *
		for (auto card_hu : _cards_hu)
		{
			auto it = std::find_if(cards_hu.begin(), cards_hu.end(), [&card_hu](const Asset::PaiElement& pai){
					return card_hu.card_type() == pai.card_type() && card_hu.card_value() == pai.card_value();
				});
			if (it == cards_hu.end()) return false; //不能换听
		}
		*/

		for (auto card_hu : cards_hu)
		{
			auto it = std::find_if(_cards_hu.begin(), _cards_hu.end(), [&card_hu](const Asset::PaiElement& pai){
					return card_hu.card_type() == pai.card_type() && card_hu.card_value() == pai.card_value();
				});
			if (it == _cards_hu.end()) return false; //不能换听
		}
	}

	if (cards_hu.size() > 0 && _cards_hu.size() == 0) _cards_hu = cards_hu; //胡牌缓存

	return cards_hu.size() > 0;
}

bool Player::CanTingPai(std::map<int32_t, std::vector<int32_t>> cards_inhand, //玩家手里的牌
		std::map<int32_t, std::vector<int32_t>> cards_outhand, //玩家墙外牌
		std::vector<Asset::PaiElement> minggang, //明杠
		std::vector<Asset::PaiElement> angang, //暗杠
		int32_t jiangang, //旋风杠，本质是明杠
		int32_t fenggang) //旋风杠，本质是暗杠
{
	if (!_room || !_game) return false;

	if (!_room->HasAnbao() && !_room->HasBaopai()) return false;

	std::vector<Asset::PaiElement> cards_hu;

	/*
	//能否胡万饼条
	for (int card_type = Asset::CARD_TYPE_WANZI; card_type <= Asset::CARD_TYPE_TIAOZI; ++card_type)
	{
		for (int card_value = 1; card_value <= 9; ++card_value)
		{
			Asset::PaiElement pai;
			pai.set_card_type((Asset::CARD_TYPE)card_type);
			pai.set_card_value(card_value);

			if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai)) 
				cards_hu.push_back(pai);
		}
	}
	
	//能否胡风牌
	for (int card_value = 1; card_value <= 4; ++card_value)
	{
		Asset::PaiElement pai;
		pai.set_card_type(Asset::CARD_TYPE_FENG);
		pai.set_card_value(card_value);

		if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai))
			cards_hu.push_back(pai);
	}

	//能否胡箭牌
	for (int card_value = 1; card_value <= 3; ++card_value)
	{
		Asset::PaiElement pai;
		pai.set_card_type(Asset::CARD_TYPE_JIAN);
		pai.set_card_value(card_value);

		if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai)) 
			cards_hu.push_back(pai);
	}
	
	*/

	const auto& cards = GameInstance.GetPais();

	for (const auto& pai : cards)
	{
		if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai, false, false)) cards_hu.push_back(pai);
	}

	if (_cards_hu.size() > 0 && cards_hu.size() > 0)
	
	if (_cards_hu.size() > 0 && cards_hu.size() > 0)
	{
		/*
		for (auto card_hu : _cards_hu)
		{
			auto it = std::find_if(cards_hu.begin(), cards_hu.end(), [&card_hu](const Asset::PaiElement& pai){
					return card_hu.card_type() == pai.card_type() && card_hu.card_value() == pai.card_value();
				});
			if (it == cards_hu.end()) return false; //不能换听
		}
		*/
		for (auto card_hu : cards_hu)
		{
			auto it = std::find_if(_cards_hu.begin(), _cards_hu.end(), [&card_hu](const Asset::PaiElement& pai){
					return card_hu.card_type() == pai.card_type() && card_hu.card_value() == pai.card_value();
				});
			if (it == _cards_hu.end()) return false; //不能换听
		}
	}

	if (cards_hu.size() > 0 && _cards_hu.size() == 0) _cards_hu = cards_hu; //胡牌缓存
	
	return cards_hu.size() > 0;
}

//玩家能不能听牌的检查
//
//玩家打出一张牌后，查看玩家再拿到一张牌后可以胡牌
//
//玩家全部牌检查，即玩家抓牌后检查
	
bool Player::CheckTingPai(std::vector<Asset::PaiElement>& pais)
{
	if (!_room || !_game) return false;

	if (_has_ting || _tuoguan_server) return false; //已经听牌，不再提示

	if (!_room->HasAnbao() && !_room->HasBaopai()) return false;

	auto cards_inhand = _cards_inhand; //玩家手里牌
	auto cards_outhand = _cards_outhand; //玩家墙外牌
	auto minggang = _minggang; //明杠
	auto angang = _angang; //暗杠
	auto jiangang = _jiangang; //旋风杠，本质是明杠
	auto fenggang = _fenggang; //旋风杠，本质是暗杠
	
	auto card_list = cards_inhand; //复制当前牌
			
	const auto& cards = GameInstance.GetPais();

	for (auto it = card_list.begin(); it != card_list.end(); ++it)
	{
		for (auto value : it->second)
		{
			auto find_it = std::find(cards_inhand[it->first].begin(), cards_inhand[it->first].end(), value);
			if (find_it == cards_inhand[it->first].end()) continue; //理论上一定能找到
						
			cards_inhand[it->first].erase(find_it); //删除这张牌

			if (!HasYaoJiu(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang)) 
			{
				cards_inhand = card_list; //恢复牌，尝试删除下一张牌
				continue; //朝阳特殊玩法,缺幺九可以胡牌不可以听牌
			}

			//
			//玩家能否胡牌
			//
			for (const auto& pai : cards)
			{
				if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai, false, false)) 
				{
					Asset::PaiElement pai; //即：打出这张后才可能听牌
					pai.set_card_type((Asset::CARD_TYPE)it->first);
					pai.set_card_value(value);

					pais.push_back(pai); //缓存
				}
			}

			/*
			//能否胡万饼条
			for (int card_type = Asset::CARD_TYPE_WANZI; card_type <= Asset::CARD_TYPE_TIAOZI; ++card_type)
			{
				for (int card_value = 1; card_value <= 9; ++card_value)
				{
					Asset::PaiElement pai;
					pai.set_card_type((Asset::CARD_TYPE)card_type);
					pai.set_card_value(card_value);

					if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai))
					{
						Asset::PaiElement pai; //即打出这张后，听牌
						pai.set_card_type((Asset::CARD_TYPE)it->first);
						pai.set_card_value(value);

						pais.push_back(pai); //缓存
					}
				}
			}
			
			//能否胡风牌
			for (int card_value = 1; card_value <= 4; ++card_value)
			{
				Asset::PaiElement pai;
				pai.set_card_type(Asset::CARD_TYPE_FENG);
				pai.set_card_value(card_value);
					
				if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai))
				{
					Asset::PaiElement pai; //即打出这张后，听牌
					pai.set_card_type((Asset::CARD_TYPE)it->first);
					pai.set_card_value(value);

					pais.push_back(pai); //缓存
				}
			}
		
			//能否胡箭牌
			for (int card_value = 1; card_value <= 3; ++card_value)
			{
				Asset::PaiElement pai;
				pai.set_card_type(Asset::CARD_TYPE_JIAN);
				pai.set_card_value(card_value);
					
				if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai))
				{
					Asset::PaiElement pai; //即打出这张后，听牌
					pai.set_card_type((Asset::CARD_TYPE)it->first);
					pai.set_card_value(value);

					pais.push_back(pai); //缓存
				}
			}
			*/
			
			//
			//下一轮检查
			//
			cards_inhand = card_list; //恢复牌，尝试删除下一张牌
		} 
	} 

	return pais.size() > 0;
}

bool Player::CheckFengGangPai(std::map<int32_t/*麻将牌类型*/, std::vector<int32_t>/*牌值*/>& cards)
{
	if (!_room || !_game) return false;

	if (!_room->HasXuanFengGang()) return false; //不支持旋风杠

	auto it = cards.find(Asset::CARD_TYPE_FENG);
	if (it == cards.end()) return false;

	for (int32_t card_value = 1; card_value <= 4; ++card_value) //东南西北
	{
		auto it_if = std::find(it->second.begin(), it->second.end(), card_value);
		if (it_if == it->second.end()) return false;
	}
	return true;
}

void Player::OnGangFengPai()
{
	PrintPai();

	if (!CheckFengGangPai(_cards_inhand)) 
	{
		LOG(ERROR, "玩家:{}不满足风杠条件", _player_id);
		return;
	}
	
	auto it = _cards_inhand.find(Asset::CARD_TYPE_FENG);
	if (it == _cards_inhand.end()) return;

	/*
	try {
		std::unique_lock<std::mutex> lock(_card_lock, std::defer_lock);

		if (lock.try_lock())
		{
			for (int32_t card_value = 1; card_value <= 4; ++card_value) //东南西北
			{
				auto it_if = std::find(it->second.begin(), it->second.end(), card_value);
				if (it_if != it->second.end())  
				{	
					_game->Add2CardsPool(Asset::CARD_TYPE_FENG, card_value);
					it->second.erase(it_if); //删除
				}
			}
		}
		else
		{
			ERROR("player_id:{} try locked failed.", _player_id);
			return;
		}
	}
	catch(const std::system_error& error)
	{
		ERROR("Delete card from player_id:{} error:{}.", _player_id, error.what());
		return;
	}
	*/

	for (int32_t card_value = 1; card_value <= 4; ++card_value) //东南西北
	{
		auto it_if = std::find(it->second.begin(), it->second.end(), card_value);
		if (it_if != it->second.end())  
		{	
			_game->Add2CardsPool(Asset::CARD_TYPE_FENG, card_value);
			it->second.erase(it_if); //删除
		}
	}

	++_fenggang;
	
	auto cards = _game->TailPai(1); //从后楼给玩家取一张牌
	if (cards.size() == 0) return;

	OnFaPai(cards);

	if (cards.size() == 0) return;

	Asset::PaiOperationAlert alert;

	//
	//旋风杠检查
	//
	auto gang = CheckXuanFeng();
	if (gang)
	{
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)gang);
	}
	//
	//杠检查(明杠和暗杠)：发牌中检查
	//
	/*
	RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
	if (CheckAllGangPai(gang_list)) 
	{
		for (auto gang : gang_list) 
		{
			auto pai_perator = alert.mutable_pais()->Add();
			pai_perator->CopyFrom(gang);
		}
	}
	*/
	//
	//自摸检查
	//
	auto zhuapai = GameInstance.GetCard(cards[0]);
	if (CheckZiMo(false) || CheckBaoHu(zhuapai))
	{
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_pai()->CopyFrom(zhuapai);
		pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);

		_game->SetPaiOperation(_player_id, _player_id, zhuapai);
	}
	//
	//听牌检查
	//
	std::vector<Asset::PaiElement> pais;
	if (CheckTingPai(pais))
	{
		for (auto pai : pais) 
		{
			auto pai_perator = alert.mutable_pais()->Add();
			pai_perator->mutable_pai()->CopyFrom(pai);
			pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
		}
	}
	
	if (alert.pais().size()) SendProtocol(alert); //提示Client
}

bool Player::CheckJianGangPai(std::map<int32_t/*麻将牌类型*/, std::vector<int32_t>/*牌值*/>& cards)
{
	if (!_room || !_game) return false;

	if (!_room->HasXuanFengGang()) return false; //不支持旋风杠

	auto it = cards.find(Asset::CARD_TYPE_JIAN);
	if (it == cards.end()) return false;

	for (auto card_value = 1; card_value <= 3; ++card_value) //中发白
	{
		auto it_if = std::find(it->second.begin(), it->second.end(), card_value);
		if (it_if == it->second.end()) return false;
	}
	return true;
}

void Player::OnGangJianPai()
{
	if (!CheckJianGangPai(_cards_inhand)) return;
	
	/*
	try {
		std::unique_lock<std::mutex> lock(_card_lock, std::defer_lock);

		if (lock.try_lock())
		{
			auto it = _cards_inhand.find(Asset::CARD_TYPE_JIAN);
			if (it == _cards_inhand.end()) return;

			for (auto card_value = 1; card_value <= 3; ++card_value) //中发白
			{
				auto it_if = std::find(it->second.begin(), it->second.end(), card_value);
				if (it_if != it->second.end())  
				{
					_game->Add2CardsPool(Asset::CARD_TYPE_JIAN, card_value);
					it->second.erase(it_if); //删除
				}
			}
		}
		else
		{
			ERROR("player_id:{} try locked failed.", _player_id);
			return;
		}
	}
	catch(const std::system_error& error)
	{
		ERROR("Delete card from player_id:{} error:{}.", _player_id, error.what());
		return;
	}
	*/

	auto it = _cards_inhand.find(Asset::CARD_TYPE_JIAN);
	if (it == _cards_inhand.end()) return;

	for (auto card_value = 1; card_value <= 3; ++card_value) //中发白
	{
		auto it_if = std::find(it->second.begin(), it->second.end(), card_value);
		if (it_if != it->second.end())  
		{
			_game->Add2CardsPool(Asset::CARD_TYPE_JIAN, card_value);
			it->second.erase(it_if); //删除
		}
	}

	++_jiangang;
	
	Asset::PaiOperationAlert alert;

	//
	//旋风杠检查
	//
	auto gang = CheckXuanFeng();
	if (gang)
	{
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)gang);
	}
	//
	//杠检查(明杠和暗杠)
	//
	RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
	if (CheckAllGangPai(gang_list)) 
	{
		for (auto gang : gang_list) 
		{
			auto pai_perator = alert.mutable_pais()->Add();
			pai_perator->CopyFrom(gang);
		}
	}
	//
	//听牌检查
	//
	std::vector<Asset::PaiElement> pais;
	if (CheckTingPai(pais))
	{
		for (auto pai : pais) 
		{
			auto pai_perator = alert.mutable_pais()->Add();
			pai_perator->mutable_pai()->CopyFrom(pai);
			pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
		}
	}
	
	if (alert.pais().size()) SendProtocol(alert); //提示Client
}

bool Player::LookAtBaopai()
{
	if (!_room || !_game) return false;

	if (!IsTingPai()) return false; 
	
	//听牌后第一次抓牌
	//
	//产生宝牌或查看宝牌
	if (_game->HasBaopai() && _game->GetRemainBaopai() <= 0) ResetBaopai(); 

	if (_oper_count_tingpai == 1) //听牌后第一次抓牌
	{
		if (!_game->HasBaopai())
		{
			_game->OnTingPai(shared_from_this()); //生成宝牌

			if (LookAtBaopai(true)) return true;
		}
		else
		{
			if (LookAtBaopai(false)) return true;
		}
	}

	return false;
}
	
int32_t Player::OnFaPai(const Asset::PaiElement& pai)
{
	//std::lock_guard<std::mutex> lock(_card_lock);

	if (!_room || !_game) return 1;

	if (pai.card_type() == 0 || pai.card_value() == 0) return 2; //数据有误

	_cards_inhand[pai.card_type()].push_back(pai.card_value()); 

	for (auto& cards : _cards_inhand) //整理牌
	{
		std::sort(cards.second.begin(), cards.second.end(), [](int x, int y){ return x < y; }); //由小到大
	}

	_zhuapai = pai;

	return 0;
}

int32_t Player::OnFaPai(std::vector<int32_t>& cards)
{
	//std::lock_guard<std::mutex> lock(_card_lock);

	if (!_room || !_game) return 1;

	if (!ShouldZhuaPai()) 
	{
		LOG(ERROR, "玩家:{}在房间:{}第:{}局中不能抓牌，当前手中牌数量:{}", _player_id, _room->GetID(), _game->GetID(), GetCardCount());
		return 12;
	}

	if (LookAtBaopai()) return 0; //生成宝牌，进宝检查

	if (false && _player_id == 262560 && _cards_inhand.size() == 0)
	{
		_cards_inhand = {
			{ 1, {2, 2, 7, 7, 7} },
			{ 2, {6, 6, 6} },
			{ 3, {3, 3, 4} },
			//{ 5, { 1, 2, 3 } },
		};
		
		_cards_outhand = {
			//{ 1, { 7, 7, 7, 6, 7, 8} },
			//{ 2, { 3, 4, 5 } },
			{ 4, { 3, 3, 3 } },
		};

		/*
		Asset::PaiElement gang;
		gang.set_card_type((Asset::CARD_TYPE)1);
		gang.set_card_value(3);

		_minggang.push_back(gang);
		*/
	}
	else if (true && _player_id == 262687 && _cards_inhand.size() == 0)
	{
		_cards_inhand = {
			{ 1, {1, 1, 1, 1, 2, 2, 3, 3, 3} },
			{ 2, {1, 1} },
			{ 3, {2, 2} },
			//{ 5, {1, 1} },
		};
		
		_cards_outhand = {
			//{ 1, { 1, 1, 1} },
			//{ 2, { 4, 4, 4} },
			//{ 3, { 8, 8, 8 } },
		};
	}
	else if (false && _player_id == 265892 && _cards_inhand.size() == 0)
	{
		_cards_inhand = {
			{ 1, {1, 1, 7, 8} },
			{ 2, {1, 1, 1, 6, 7, 8} },
			{ 3, {6, 6, 6} },
		};
	}
	else if (false && _player_id == 265888 && _cards_inhand.size() == 0)
	{
		_cards_inhand = {
			{ 1, {1, 2, 6, 8, 9} },
			{ 2, {2, 3, 5, 7, 8} },
			{ 3, {2, 2, 8} },
		};
	}
	else if (false && _player_id == 265873 && _cards_inhand.size() == 0)
	{
		_cards_inhand = {
			{ 1, {1, 2, 6, 8, 9} },
			{ 2, {1, 1, 5, 7, 8} },
			{ 3, {7, 7, 8, 8} },
		};
	}
	else
	{
		for (auto card_index : cards) //发牌到玩家手里
		{
			auto card = GameInstance.GetCard(card_index);
			if (card.card_type() == 0 || card.card_value() == 0) return 2; //数据有误

			_cards_inhand[card.card_type()].push_back(card.card_value()); //插入玩家手牌
		}
	}

	//for (auto& cards : _cards_inhand) //整理牌
	//{
	//	std::sort(cards.second.begin(), cards.second.end(), [](int x, int y){ return x < y; }); //由小到大
	//}

	for (auto& cards : _cards_inhand) //整理牌
		std::sort(cards.second.begin(), cards.second.end(), [](int x, int y){ return x < y; }); //由小到大

	Asset::PaiNotify notify; //玩家牌数据发给Client
	notify.set_player_id(_player_id); //目标玩家

	if (cards.size() > 1) //开局
	{
		for (auto pai : _cards_inhand)
		{
			auto pais = notify.mutable_pais()->Add();

			pais->set_card_type((Asset::CARD_TYPE)pai.first); //牌类型

			::google::protobuf::RepeatedField<int32_t> cards(pai.second.begin(), pai.second.end());
			pais->mutable_cards()->CopyFrom(cards); //牌值
		}
		
		notify.set_data_type(Asset::PaiNotify_CARDS_DATA_TYPE_CARDS_DATA_TYPE_START); //操作类型：开局

		if (cards.size() == 14) //庄家检查
		{
			_fapai_count = 1; //默认已经发牌

			CheckXuanFengGang(); //庄家起手，旋风杠检查

			Asset::PaiOperationAlert alert; //提示协议

			//
			//是否可以胡牌
			//
			//朝阳麻将不支持天胡
			//
			/*
			if (CheckZiMo())
			{
				auto pai_perator = alert.mutable_pais()->Add();
				pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);

				LOG(INFO, "玩家:{}起手可以胡牌", _player_id);
			}
			*/

			//
			//听牌检查
			//
			std::vector<Asset::PaiElement> ting_list;
			if (CheckTingPai(ting_list))
			{
				for (auto pai : ting_list) 
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_pai()->CopyFrom(pai);
					pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
				}
			}
			
			//
			//明杠和暗杠检查(开始只可能是暗杠)
			//
			::google::protobuf::RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
			if (CheckAllGangPai(gang_list)) 
			{
				for (auto gang : gang_list) 
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->CopyFrom(gang);
				
					_game->SetPaiOperation(_player_id, _player_id, gang.pai(), Asset::PAI_OPER_TYPE_ANGANGPAI); //多个暗杠只提示最后一个
				}
			}
			
			//
			//是否旋风杠
			//
			for (auto gang : _xf_gang)
			{
				auto pai_perator = alert.mutable_pais()->Add();
				pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE(gang));
		
				_game->SetPaiOperation(_player_id, _player_id, Asset::PaiElement(), gang);
			}

			if (alert.pais().size()) SendProtocol(alert); //上来即有旋风杠或者胡牌
		}
	}
	else if (cards.size() == 1)
	{
		auto card = GameInstance.GetCard(cards[0]);
		if (card.card_type() == 0 || card.card_type() == 0) return 11;

		_zhuapai = card;
		++_fapai_count; //发牌数量
		
		//_game->SetCurrPlayerIndexByPlayer(_player_id); //设置游戏当前玩家

		notify.set_data_type(Asset::PaiNotify_CARDS_DATA_TYPE_CARDS_DATA_TYPE_FAPAI); //操作类型:发牌
		notify.mutable_pai()->set_card_type(card.card_type());
		notify.mutable_pai()->set_card_value(card.card_value());

		Asset::PaiOperation pai_operate;
		pai_operate.set_oper_type(Asset::PAI_OPER_TYPE_FAPAI);
		pai_operate.set_position(GetPosition());
		pai_operate.mutable_pai()->CopyFrom(card);
		_game->AddPlayerOperation(pai_operate); //牌局回放

		if (IsTingPai()) ++_oper_count_tingpai; //听牌后发了//抓了多少张牌

		if (_fapai_count == 1) CheckXuanFengGang(); //非庄家旋风杠检查
			
		//
		//如果玩家处于服务器托管状态，则自动出牌
		//
		//可能玩家掉线或者已经逃跑
		//
		/*
		if (HasTuoGuan())
		{
			Asset::PaiOperation pai_operation; 
			pai_operation.set_oper_type(Asset::PAI_OPER_TYPE_DAPAI);
			pai_operation.set_position(GetPosition());
			pai_operation.mutable_pai()->CopyFrom(card);

			CmdPaiOperate(&pai_operation);
		}
		else
		{
			NormalCheckAfterFaPai(card);
		}
		*/
	}
	
	auto remain_count = _game->GetRemainCount();
	notify.set_cards_remain(remain_count); //当前剩余牌数量
	
	SendProtocol(notify); //发牌给玩家

	notify.mutable_pais()->Clear(); notify.mutable_pai()->Clear(); //其他玩家不能知道具体发了什么牌
	Send2Roomers(notify, _player_id); //玩家行为

	return 0;
}
	
//
//看宝的逻辑都一样
//
//只不过第一人要打股子
//
bool Player::LookAtBaopai(bool has_saizi)
{
	if (!_room || !_game) return false;

	auto baopai = _game->GetBaoPai();

	if (baopai.card_type() != _baopai.card_type() || baopai.card_value() != _baopai.card_value())
	{
		Asset::RandomSaizi proto;
		proto.set_has_rand_saizi(has_saizi);
		proto.set_reason_type(Asset::RandomSaizi_REASON_TYPE_REASON_TYPE_TINGPAI);
		proto.mutable_random_result()->Add(_game->GetRandResult());
		proto.mutable_pai()->CopyFrom(baopai);

		if (_room->HasAnbao()) proto.mutable_pai()->Clear(); //暗宝不同步宝牌
		SendProtocol(proto); //通知玩家

		if (has_saizi) _game->OnPlayerLookAtBaopai(_player_id, proto);
	}
	
	_baopai = baopai; //设置宝牌

	if (CheckHuPai(baopai, false, false)) 
	{
		Jinbao(); //进宝

		Asset::PaiOperationAlert alert;
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_pai()->CopyFrom(baopai);
		pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
		SendProtocol(alert); //进宝 
	
		_game->SetZiMoCache(shared_from_this(), baopai); //自摸胡牌缓存

		return true;
	}

	return false;
}

bool Player::OnTingPai()
{
	if (!_game || !_room) return false;

	_has_ting = true;
	_oper_count_tingpai = 1;

	_game->AddTingPlayer(_player_id);
	
	if (_room->HasAnbao()) 
	{
		if (LookAtBaopai()) return true; //暗宝直接看宝
		++_oper_count_tingpai;
	}

	return false;
}

//
//宝牌没有剩余数量则重新进行随机
//
//相当于玩家一直在打股子，选择宝牌
//
void Player::ResetBaopai()
{
	if (!_game || !_has_ting) return;
		
	while(_game->HasBaopai())
	{
		if (_game->GetRemainBaopai() <= 0) 
		{
			int32_t result = CommonUtil::Random(1, 6);

			auto baopai = _game->GetBaoPai(result);

			//DEBUG("玩家{}发起换宝，当前宝牌:{}, 换之后宝牌:{} 打股子:{}", _player_id, _game->GetBaoPai().ShortDebugString(), baopai.ShortDebugString(), result);

			_game->SetBaoPai(baopai);

			if (_game->GetRemainBaopai() > 0) _game->OnRefreshBaopai(_player_id, result);
		}
		else
		{
			return;
		}
	}
}
	
void Player::ResetLookAtBaopai() 
{ 
	if (!_game) return;

	_baopai = _game->GetBaoPai();

	_oper_count_tingpai = 1; 
}
	
void Player::SynchronizePai()
{
	return;

	Asset::PaiNotify notify; /////玩家当前牌数据发给Client

	for (auto pai : _cards_inhand)
	{
		auto pais = notify.mutable_pais()->Add();

		pais->set_card_type((Asset::CARD_TYPE)pai.first); //牌类型

		for (auto value : pai.second)
			std::cout << value << " ";
		std::cout << std::endl;

		::google::protobuf::RepeatedField<int32_t> cards(pai.second.begin(), pai.second.end());
		pais->mutable_cards()->CopyFrom(cards); //牌值
	}
	
	notify.set_data_type(Asset::PaiNotify_CARDS_DATA_TYPE_CARDS_DATA_TYPE_SYNC); //操作类型：同步数据
	
	SendProtocol(notify); //发送
}

void Player::PrintPai()
{
	if (!_room || !_game) return;

	std::stringstream card_value_list;

	for (const auto& pai : _minggang)
	{
		card_value_list << "[明杠]"	<< " card_type:" << pai.card_type() << " card_value:" << pai.card_value();
	}
	
	for (auto pai : _angang)
	{
		card_value_list << "[暗杠]" << " card_type:" << pai.card_type() << " card_value:" << pai.card_value();
	}
	
	for (const auto& pai : _cards_outhand)
	{
		std::stringstream outhand_list;
		for (auto card_value : pai.second) 
			outhand_list << card_value << " ";

		if (outhand_list.str().size()) card_value_list << "[牌外]" << " card_type:" << pai.first << " card_value:" << outhand_list.str();
	}

	for (const auto& pai : _cards_inhand)
	{
		std::stringstream inhand_list;
		for (auto card_value : pai.second) 
			inhand_list << card_value << " ";

		if (inhand_list.str().size()) card_value_list << "[牌内]"	<< " card_type:" << pai.first << " card_value:" << inhand_list.str();
	}
		
	auto room_id = _room->GetID();
	auto curr_count = _room->GetGamesCount();
	auto open_rands = _room->GetOpenRands();

	LOG(INFO, "玩家:{}在房间{}第{}/{}局牌数据:{}", _player_id, room_id, curr_count, open_rands, card_value_list.str());
}
	
Asset::GAME_OPER_TYPE Player::GetOperState() 
{ 
	if (_player_prop.offline()) return Asset::GAME_OPER_TYPE_OFFLINE;

	return _player_prop.game_oper_state(); 
}
	
void Player::SetOffline(bool offline)
{ 
	if (!_room/* || !_game*/) return; //房间状态

	if (offline == _player_prop.offline()) return; //状态尚未发生变化
	
	DEBUG("玩家:{}状态变化:{} 是否离线:{}", _player_id, _player_prop.game_oper_state(), offline);

	_player_prop.set_offline(offline); 

	_room->OnPlayerStateChanged();
}

void Player::ClearCards() 
{
	_fan_list.clear(); //番数
	_cards_inhand.clear(); //清理手里牌
	_cards_outhand.clear(); //清理墙外牌
	_cards_pool.clear(); //牌池
	_cards_hu.clear(); //胡牌
	_hu_result.clear(); //胡牌数据
 
 	_minggang.clear(); //清理杠牌
	_angang.clear(); //清理杠牌

	_jiangang = _fenggang = 0; //清理旋风杠
	
	_oper_count_tingpai = 0;
	_fapai_count = _oper_count = 0; 
	_has_ting = _jinbao = false;
	_tuoguan_server = false;
	_last_oper_type = _oper_type = Asset::PAI_OPER_TYPE_BEGIN; //初始化操作
	_player_prop.clear_game_oper_state(); //准备//离开
	_baopai.Clear();
	_zhuapai.Clear();

	if (_game) _game.reset();
}
	
void Player::OnGameOver()
{
	ClearCards();
	
	if (_tuoguan_server) OnLogout(Asset::KICK_OUT_REASON_LOGOUT);
}

int32_t Player::CmdSayHi(pb::Message* message)
{
	auto say_hi = dynamic_cast<const Asset::SayHi*>(message);
	if (!say_hi) return 1;

	_hi_time = TimerInstance.GetTime();
	
	OnSayHi(); //心跳回复

	return 0;
}

void Player::OnlineCheck()
{
	auto curr_time = TimerInstance.GetTime();
	auto duration_pass = curr_time - _hi_time;

	if (duration_pass <= 0) 
	{
		_pings_count = 0;
		return;
	}

	if (duration_pass > 5)
	{
		++_pings_count;
		
		static int32_t max_allowed = 2;

		if (max_allowed && _pings_count >= max_allowed) 
		{
			SetOffline(); //玩家离线
		}
	}
	else
	{
		SetOffline(false); //玩家上线
		
		_pings_count = 0;
	}
}
	
void Player::OnSayHi()
{
	Asset::SayHi message;
	message.set_heart_count(_heart_count);
	SendProtocol(message);
	
	DEBUG("玩家:{}收到心跳发送心跳", _player_id);
}
	
int32_t Player::CmdGameSetting(pb::Message* message)
{
	auto game_setting = dynamic_cast<const Asset::GameSetting*>(message);
	if (!game_setting) return 1;

	_stuff.mutable_game_setting()->CopyFrom(game_setting->game_setting());

	SendProtocol(game_setting); //设置成功
	
	SetDirty();

	return 0;
}
	
int32_t Player::CmdSystemChat(pb::Message* message)
{
	auto chat = dynamic_cast<Asset::SystemChat*>(message);
	if (!chat) return 1;

	if (!_room) return 2;

	chat->set_position(GetPosition());

	_room->BroadCast(message);

	return 0;
}
	
int32_t Player::OnKickOut(pb::Message* message)
{
	const auto kick_out = dynamic_cast<const Asset::KickOutPlayer*>(message);
	if (!kick_out) return 1;

	DEBUG("玩家:{} 被踢出逻辑服务器:{} 踢出内容:{}", _player_id, _stuff.server_id(), kick_out->ShortDebugString());
	
	Logout(message);

	return 0;
}
	
int32_t Player::OnPlayerStateChanged(pb::Message* message)
{
	const auto state = dynamic_cast<const Asset::PlayerState*>(message);
	if (!state) return 1;

	switch (state->oper_type())
	{
		case Asset::GAME_OPER_TYPE_OFFLINE:
		{
			SetOffline(); //玩家离线
		}
		break;

		default:
		{
		}
		break;
	}

	return 0;
}
	
int32_t Player::GetCardCount()
{
	int32_t total_size = 0;

	for (auto cards : _cards_inhand)
	{
		total_size += cards.second.size();
	}

	return total_size;
}
	
const Asset::WechatUnion Player::GetWechat() 
{ 
	if (!_user.has_wechat())
	{
		//auto redis = make_unique<Redis>();
		RedisInstance.GetUser(_stuff.account(), _user);
	}

	return _user.wechat();
}
	
bool Player::CheckCardsInhand()
{
	auto count = GetCardCount(); //牌数量

	if (count == 13 || count == 10 || count == 7 || count == 4 || count == 1) return true;

	return false;
}

bool Player::ShouldZhuaPai()
{
	return _cards_inhand.size() == 0 || CheckCardsInhand();
}

bool Player::CheckHuCardsInhand()
{
	auto count = GetCardCount(); //牌数量

	if (count == 14 || count == 11 || count == 8 || count == 5 || count == 2) return true;

	return false;
}

bool Player::ShouldDaPai()
{
	return CheckHuCardsInhand();
}
	
void Player::SendRoomState()
{
	Asset::RoomState proto;

	if (_room && !_room->HasDisMiss()) 
	{
		proto.set_room_id(_room->GetID());
	}
	else if (_stuff.room_id() && (_room && !_room->HasDisMiss()))
	{
		auto room = RoomInstance.Get(_stuff.room_id());
		if (room) proto.set_room_id(_stuff.room_id());
	}
	else 
	{
		proto.set_oper_type(Asset::GAME_OPER_TYPE_LEAVE);
	}

	SendProtocol(proto);
}
	
void Player::AddRoomScore(int32_t score)
{
	_stuff.mutable_common_prop()->set_score(_stuff.common_prop().score() + score); 

	if (score > 0)
	{
		_stuff.mutable_common_prop()->set_score_win_rounds(_stuff.common_prop().score_win_rounds() + 1); //获胜
	}
	else
	{
		_stuff.mutable_common_prop()->set_score_win_rounds(_stuff.common_prop().score_win_rounds() - 1);

		if (_stuff.common_prop().score_win_rounds() < 0) _stuff.mutable_common_prop()->set_score_win_rounds(0); //失败
	}

	_dirty = true;
}

//
//当前周期为50MS.
//
//CenterSession::Update()  调用
//
/*
void PlayerManager::Update(int32_t diff)
{
	std::lock_guard<std::mutex> lock(_player_lock);

	++_heart_count;

	if (_heart_count % 20 == 0) //1s
	{
		for (auto it = _players.begin(); it != _players.end();)
		{
			if (!it->second) 
			{
				it = _players.erase(it);
				continue;
			}
			else
			{
				it->second->Update();
				++it;
			}
		}
	}
	
	if (_heart_count % 3600 == 0) //30mins
	{
		int32_t server_id = ConfigInstance.GetInt("ServerID", 1);
		DEBUG("游戏逻辑服务器:{}在线玩家数量:{}", server_id, _players.size());
	}
}
*/

int32_t Player::CmdRecharge(pb::Message* message)
{
	auto user_recharge = dynamic_cast<const Asset::UserRecharge*>(message);
	if (!user_recharge) return 1;
		
	const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_RECHARGE);

	for (auto it = messages.begin(); it != messages.end(); ++it)
	{
		auto recharge = dynamic_cast<Asset::Recharge*>(*it);
		if (!recharge) continue;

		if (user_recharge->product_id() != recharge->product_id()) continue;

		//if (recharge->price_show() != user_recharge->price()) continue; //价格不一致
		GainDiamond(Asset::DIAMOND_CHANGED_TYPE_MALL, recharge->gain_diamond());
		break;
	}

	return 0;
}

bool Player::Is28Zhang()
{
	if (_zhang.card_type() == Asset::CARD_TYPE_FENG || _zhang.card_type() == Asset::CARD_TYPE_JIAN) return false; //东南西北中发白

	if (_zhang.card_value() == 2 || _zhang.card_value() == 8) return true;

	return false;
}
	
bool Player::Is28Pai(const Asset::PaiElement& pai)
{
	if (pai.card_type() == Asset::CARD_TYPE_FENG || pai.card_type() == Asset::CARD_TYPE_JIAN) return false; //东南西北中发白

	if (pai.card_value() == 2 || pai.card_value() == 8) return true;

	return false;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// 玩家管理
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void PlayerManager::Emplace(int64_t player_id, std::shared_ptr<Player> player)
{
	if (!player) return;

	std::lock_guard<std::mutex> lock(_player_lock);

	_players[player_id] = player;
}

std::shared_ptr<Player> PlayerManager::GetPlayer(int64_t player_id)
{
	std::lock_guard<std::mutex> lock(_player_lock);

	return _players[player_id];
}

std::shared_ptr<Player> PlayerManager::Get(int64_t player_id)
{
	return GetPlayer(player_id);
}
	
bool PlayerManager::Has(int64_t player_id)
{
	auto player = GetPlayer(player_id);

	return player != nullptr;
}

void PlayerManager::Remove(int64_t player_id)
{
	std::lock_guard<std::mutex> lock(_player_lock);

	auto player = _players[player_id];
	if (player) player.reset();
	
	_players.erase(player_id);

	if (g_center_session) g_center_session->RemovePlayer(player_id);
}

void PlayerManager::Remove(std::shared_ptr<Player> player)
{
	if (!player) return;

	Remove(player->GetID());
}
	
void PlayerManager::BroadCast(const pb::Message& message)
{
	for (auto it = _players.begin(); it != _players.end(); ++it)
	{
		auto player = it->second;
		if (!player) continue;

		player->SendProtocol(message);
	}
}

}
