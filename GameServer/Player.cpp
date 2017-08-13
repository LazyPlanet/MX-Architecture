#include <iostream>

#include <hiredis.h>

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

	//AddHandler(Asset::META_TYPE_C2S_LOGIN, std::bind(&Player::CmdLogin, this, std::placeholders::_1));
	//AddHandler(Asset::META_TYPE_C2S_ENTER_GAME, std::bind(&Player::CmdEnterGame, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_C2S_GET_REWARD, std::bind(&Player::CmdGetReward, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_C2S_LOAD_SCENE, std::bind(&Player::CmdLoadScene, this, std::placeholders::_1));
	
	//中心服务器协议处理
	AddHandler(Asset::META_TYPE_S2S_KICKOUT_PLAYER, std::bind(&Player::OnKickOut, this, std::placeholders::_1));
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
	auto redis = make_unique<Redis>();
	auto success = redis->GetPlayer(_player_id, _stuff);
	if (!success) return 1;
		
	DEBUG("player_id:{} load info:{}", _player_id, _stuff.ShortDebugString());

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
			TRACE("player_id:{} add inventory:{}", _player_id, enum_value->name());
		}
	} while(false);
	
	return 0;
}

int32_t Player::Save(bool force)
{
	PLAYER(_stuff);	//BI日志

	if (!force && !IsDirty()) return 1;

	auto redis = make_unique<Redis>();
	auto success = redis->SavePlayer(_player_id, _stuff);
	if (!success) 
	{
		DEBUG_ASSERT(false);
		return 2;
	}

	_dirty = false;

	DEBUG("玩家:{}保存数据，数据内容:{}", _player_id, _stuff.ShortDebugString());
		
	return 0;
}
	
int32_t Player::OnLogin()
{
	if (Load()) return 1;

	//ClearCards();
	
	PlayerInstance.Emplace(_player_id, shared_from_this()); //玩家管理
	SetLocalServer(ConfigInstance.GetInt("ServerID", 1));

	return 0;
}

int32_t Player::Logout(pb::Message* message)
{
	//
	//如果玩家正在进行玩牌，则不允许立即退出
	//
	//(1) 处理非操作玩家退出的状态;
	//
	//(2) 处理操作玩家退出的状态，即刚好轮到该玩家进行操作的时候，玩家逃跑;
	//
	if (_room) 
	{
		if (_game && _room->GetRemainCount() > 0) //游戏中，且尚未对局完成，则不让退出房间
		{
			SetOffline(); //玩家状态

			ERROR("player_id:{} logout game when in room:{}", _player_id, _room->GetID()); //玩家逃跑

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

			return 1; //不能退出游戏
		}
		else
		{
			_room->Remove(_player_id); //退出房间
		}
	}

	OnLogout();
	
	return 0;
}
	
int32_t Player::OnLogout()
{
	DEBUG("玩家:{} 退出游戏回调...", _player_id);

	_stuff.set_login_time(0);
	_stuff.set_logout_time(CommonTimerInstance.GetTime());
	
	//ClearCards();

	if (!_game && _room) ResetRoom();
	
	Save(true);	//存档数据库

	PlayerInstance.Remove(_player_id); //玩家管理

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

	if (!CheckRoomCard(count)) 
	{
		LOG(INFO, "消耗玩家:{}房卡，消耗类型:{} 数量:{}失败", _player_id, changed_type, count);
		return 0;
	}

	_stuff.mutable_common_prop()->set_room_card_count(_stuff.common_prop().room_card_count() - count);
	_dirty = true;
	
	SyncCommonProperty();
	
	return count;
}

int64_t Player::GainRoomCard(Asset::ROOM_CARD_CHANGED_TYPE changed_type, int64_t count) 
{
	if (count <= 0) return 0;

	_stuff.mutable_common_prop()->set_room_card_count(_stuff.common_prop().room_card_count() + count);
	_dirty = true;
	
	SyncCommonProperty();
	
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

	if (!CheckHuanledou(count)) return 0;

	_stuff.mutable_common_prop()->set_huanledou(_stuff.common_prop().huanledou() - count);
	_dirty = true;
	
	SyncCommonProperty();
	
	return count;
}

int64_t Player::GainHuanledou(Asset::HUANLEDOU_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	_stuff.mutable_common_prop()->set_huanledou(_stuff.common_prop().huanledou() + count);
	_dirty = true;
	
	SyncCommonProperty();
	
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

	if (!CheckDiamond(count)) return 0;

	_stuff.mutable_common_prop()->set_diamond(_stuff.common_prop().diamond() - count);
	_dirty = true;
	
	SyncCommonProperty();
	
	return count;
}

int64_t Player::GainDiamond(Asset::DIAMOND_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	_stuff.mutable_common_prop()->set_diamond(_stuff.common_prop().diamond() + count);
	_dirty = true;

	SyncCommonProperty();
	
	return count;
}

bool Player::CheckDiamond(int64_t count)
{
	int64_t curr_count = _stuff.common_prop().diamond();
	return curr_count >= count;
}

int32_t Player::OnEnterGame() 
{
	if (Load()) return 1;
	
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

	PLAYER(_stuff);	//BI日志

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
	//
	//调试命令
	//
	//如果不用，请勿忘注释
	//
	//DebugCommand();

	Asset::CreateRoom* create_room = dynamic_cast<Asset::CreateRoom*>(message);
	if (!create_room) return 1;
	
	if (_room) 
	{
		auto room = RoomInstance.Get(_room->GetID());

		if (room) //房间尚未解散
		{
			AlertMessage(Asset::ERROR_ROOM_HAS_BEEN_IN);
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

		const Asset::Item_RoomCard* room_card = dynamic_cast<const Asset::Item_RoomCard*>(AssetInstance.Get(g_const->room_card_id()));
		if (!room_card) return 3;

		int32_t rounds = room_card->rounds(); //该张房卡可以玩多少局麻将
		if (rounds <= 0) return 4;

		int32_t consume_count = open_rands / rounds; //待消耗房卡数量

		if (!CheckRoomCard(consume_count)) 
		{
			AlertMessage(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //房卡不足
			return 5;
		}

		/*
		auto consume_real = ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_OPEN_ROOM, consume_count); //消耗
		if (consume_count != consume_real)
		{
			LOG(ERROR, "Consume room card error, consume_count:{} consume_real:{}", consume_count, consume_real);
			return 6;
		}
		*/
	}

	int64_t room_id = RoomInstance.CreateRoom();
	if (!room_id) return 2;

	create_room->mutable_room()->set_room_id(room_id);
	create_room->mutable_room()->set_room_type(Asset::ROOM_TYPE_FRIEND); //创建房间，其实是好友房
	
	SendProtocol(create_room); 
	
	OnCreateRoom(create_room); //创建房间成功，直接将玩家设置到该房间，TODO:感觉设计有问题，客户端应该走通用进入房间逻辑

	LOG(ACTION, "player_id:{} create room_id:{}", _player_id, room_id);

	return 0;
}

void Player::OnCreateRoom(Asset::CreateRoom* create_room)
{
	if (!create_room) return; //理论不会如此

	Asset::Room asset_room;
	asset_room.CopyFrom(create_room->room());

	_room = std::make_shared<Room>(asset_room);
	_room->OnCreated(shared_from_this());

	RoomInstance.OnCreateRoom(_room); //房间管理
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

	if (!_room) 
	{
		DEBUG_ASSERT(false && "玩家尚未在房间当中");
		return 4;
	}

	_room->OnPlayerOperate(shared_from_this(), message); //广播给其他玩家

	return 0;
}
	
void Player::OnGameStart()
{
	ClearCards();  //游戏数据
	//_player_prop.clear_game_oper_state(); 
}

int32_t Player::CmdPaiOperate(pb::Message* message)
{
	Asset::PaiOperation* pai_operate = dynamic_cast<Asset::PaiOperation*>(message);
	if (!pai_operate) return 1; 

	if (!_room || !_game) 
	{
		DEBUG_ASSERT(false);
		return 2; //还没加入房间或者还没开始游戏
	}

	if (!pai_operate->position()) pai_operate->set_position(GetPosition()); //设置玩家座位
	
	DEBUG("Receive from player_id:{} operation: {}.", _player_id, pai_operate->DebugString());

	PrintPai(); //打印玩家当前手里的牌数据

	//进行操作
	switch (pai_operate->oper_type())
	{
		case Asset::PAI_OPER_TYPE_DAPAI: //打牌
		{
			const auto& pai = pai_operate->pai(); 

			auto& pais = _cards_inhand[pai.card_type()]; //获取该类型的牌
			
			auto it = std::find(pais.begin(), pais.end(), pai.card_value()); //查找第一个满足条件的牌即可
			if (it == pais.end()) 
			{
				DEBUG_ASSERT(false);
				return 3; //没有这张牌
			}

			pais.erase(it); //打出牌

			Add2CardsPool(pai);

			/*
			try {
				std::unique_lock<std::mutex> lock(_card_lock, std::defer_lock);

				if (lock.try_lock()) 
				{
					pais.erase(it); //打出牌

					Add2CardsPool(pai);
				}
				else
				{
					ERROR("player_id:{} try locked failed.", _player_id);
					return 10;
				}
			}
			catch(const std::system_error& error)
			{
				ERROR("Delete card from player_id:{} card_type:{} card_value:{} error.", _player_id, pai.card_type(), pai.card_value(), error.what());
				return 10;
			}
			*/
		}
		break;
		
		case Asset::PAI_OPER_TYPE_CHIPAI: //吃牌
		{
			//检查玩家是否真的有这些牌
			for (const auto& pai : pai_operate->pais()) 
			{
				const auto& pais = _cards_inhand[pai.card_type()];

				auto it = std::find(pais.begin(), pais.end(), pai.card_value());
				if (it == pais.end()) return 4; //没有这张牌

				//if (pais[pai.card_index()] != pai.card_value()) return 4; //Server<->Client 不一致 TODO:暂时不做检查
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
				DEBUG_ASSERT(false);
				return 7; //没有这张牌
			}

			if (!CanTingPai(pai)) 
			{
				DEBUG_ASSERT(false);
				return 8; //不能听牌
			}
			/*
					
			try {
				std::unique_lock<std::mutex> lock(_card_lock, std::defer_lock);
			
				if (lock.try_lock()) 
				{
					pais.erase(it); //删除牌
					Add2CardsPool(pai);
					TRACE("Delete card from player_id:{} card_type:{} card_value:{} for dapai.", _player_id, pai.card_type(), pai.card_value());
				}
				else
				{
					ERROR("player_id:{} try locked failed.", _player_id);
					return 10;
				}
			}
			catch(const std::system_error& error)
			{
				ERROR("Delete card from player_id:{} card_type:{} card_value:{} error.", _player_id, pai.card_type(), pai.card_value(), error.what());
					return 10;
			}
			*/
			
			pais.erase(it); //删除牌

			Add2CardsPool(pai);

			OnTingPai();
		}
		break;

		default:
		{

		}
		break;
	}

	_game->OnPaiOperate(shared_from_this(), message);
	
	++_oper_count;

	//
	//处理杠流泪场景，须在_game->OnPaiOperate下进行判断
	//
	//玩家抓到杠之后，进行打牌，记录上次牌状态
	//
	if (pai_operate->oper_type() != Asset::PAI_OPER_TYPE_HUPAI) 
	{
		_last_oper_type = _oper_type;
		_oper_type = pai_operate->oper_type(); //记录上次牌操作
	}

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
	Asset::EnterRoom* enter_room = dynamic_cast<Asset::EnterRoom*>(message);
	if (!enter_room) return Asset::ERROR_INNER; 

	//
	//房间重入检查
	//
	if (_room) 
	{
		DEBUG("玩家:{}重入房间:{} 数据:{}", _player_id, _room->GetID(), enter_room->ShortDebugString());
			
		auto locate_room = RoomInstance.Get(enter_room->room().room_id());
		if (!locate_room)
		{
			enter_room->set_error_code(Asset::ERROR_ROOM_NOT_FOUNT); //是否可以进入场景//房间
			SendProtocol(message);

			ResetRoom(); //房间已经解散
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
			AlertMessage(Asset::ERROR_ROOM_HAS_BEEN_IN);

			return Asset::ERROR_ROOM_HAS_BEEN_IN;
		}
	}

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
				auto ret = locate_room->TryEnter(shared_from_this()); //玩家进入房间
				enter_room->mutable_room()->CopyFrom(locate_room->Get());
				enter_room->set_error_code(ret); //是否可以进入场景//房间
			}

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
			return 1; //非法
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

bool Player::OnEnterRoom(int64_t room_id)
{
	/*
	_room = RoomInstance.Get(room_id);

	if (!_room) 
	{
		DEBUG_ASSERT(false);
		return false; //非法的房间 
	}

	_room->OnCreated();

	_room->Enter(shared_from_this()); //玩家进入房间
	*/

	return true;
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
		LOG(ERROR, "玩家{}尚未建立连接", _player_id);
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

	DEBUG("玩家:{} 发送协议内容:{}", _player_id, message.ShortDebugString());
}

void Player::Send2Roomers(pb::Message& message, int64_t exclude_player_id) 
{
	if (!_room) 
	{
		DEBUG_ASSERT(false);
		return;
	}
	_room->BroadCast(message, exclude_player_id);
}

void Player::Send2Roomers(pb::Message* message, int64_t exclude_player_id)
{
	if (!_room) 
	{
		DEBUG_ASSERT(false);
		return;
	}
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
	
	if (_heart_count % 5 == 0) //5s
	{
		CommonLimitUpdate(); //通用限制,定时更新
	
		if (_dirty) Save(); //触发存盘
	
		//SayHi(); //逻辑服务器不进行玩家心跳
	}

	if (_heart_count % 60 == 0) //1min
	{
		DEBUG("heart_count:{} player_id:{}", _heart_count, _player_id);
	}
	return true;
}
	
int32_t Player::DefaultMethod(pb::Message* message)
{
	if (!message) return 0;

	const pb::FieldDescriptor* field = message->GetDescriptor()->FindFieldByName("type_t");
	if (!field) 
	{
		std::cout << __func__ << ":Could not found type_t of received message." << std::endl;
		return 1;
	}
	const pb::EnumValueDescriptor* enum_value = message->GetReflection()->GetEnum(*message, field);
	if (!enum_value) return 2;

	const std::string& enum_name = enum_value->name();
	std::cout << __func__ << ":Could not found call back, message type is: " << enum_name.c_str() << std::endl;
	return 0;
}

bool Player::HandleProtocol(int32_t type_t, pb::Message* message)
{
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
			auto item_toadd = inventory_items->Add();
			item_toadd->set_type_t((Adoter::Asset::ASSET_TYPE)type_t);
			common_prop.set_count(count); //Asset::Item_CommonProp
			item_toadd->set_stuff(message_item->SerializeAsString());
		}
		else
		{
			common_prop.set_count(common_prop.count() + count); //Asset::Item_CommonProp
			it_item->set_stuff(message_item->SerializeAsString());
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

void Player::OnLeaveRoom()
{
	_stuff.clear_room_id(); //用于处理玩家断线重入房间

	ResetRoom();
	ClearCards();  //游戏数据
	
	Asset::RoomState room_state;
	room_state.set_oper_type(Asset::GAME_OPER_TYPE_LEAVE);
	SendProtocol(room_state);
}
	
void Player::BroadCast(Asset::MsgItem& item) 
{
	if (!_room) return;
	
}	

void Player::AlertMessage(Asset::ERROR_CODE error_code, Asset::ERROR_TYPE error_type/*= Asset::ERROR_TYPE_NORMAL*/, 
		Asset::ERROR_SHOW_TYPE error_show_type/* = Asset::ERROR_SHOW_TYPE_CHAT*/)
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

bool Player::CmdBuySomething(pb::Message* message)
{
	auto some_thing = dynamic_cast<Asset::BuySomething*>(message);
	if (some_thing) return false;

	int64_t mall_id = some_thing->mall_id();
	if (mall_id <= 0) return false;

	auto ret = MallInstance.BuySomething(shared_from_this(), mall_id);
	some_thing->set_result(ret);

	SendProtocol(some_thing); //返回给Client

	return true;
}

int32_t Player::CmdLoadScene(pb::Message* message)
{
	Asset::LoadScene* load_scene = dynamic_cast<Asset::LoadScene*>(message);
	if (!load_scene) return 1;

	TRACE("player_id:{}, curr_load_type:{} message:{}", _player_id, _player_prop.load_type(), load_scene->ShortDebugString());

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
			if (_player_prop.load_type() != Asset::LOAD_SCENE_TYPE_START) 
			{
				DEBUG_ASSERT(false && "player is not loaded.");
				return 2;
			}

			auto room_id = _player_prop.room_id();
			
			auto locate_room = RoomInstance.Get(room_id);
			if (!locate_room) 
			{
				DEBUG_ASSERT(false);
				return 3; //非法的房间 
			}
			
			auto enter_status = locate_room->TryEnter(shared_from_this()); //玩家进入房间

			if (enter_status != Asset::ERROR_SUCCESS && enter_status != Asset::ERROR_ROOM_HAS_BEEN_IN) 
			{
				ERROR("player_id:{} enter room:{} failed, reason:{}.", _player_id, room_id, enter_status);
				return 4;
			}

			auto is_entered = locate_room->Enter(shared_from_this()); //玩家进入房间
			if (!is_entered)
			{
				DEBUG_ASSERT(false);
				return 5;
			}
			
			SetRoom(locate_room);
				
			DEBUG("player_id:{} enter room:{} success.", _player_id, room_id);
			
			_player_prop.clear_load_type(); 
			_player_prop.clear_room_id(); 
	
			_stuff.set_room_id(room_id);

			SetDirty();
			
			OnEnterScene(room_id); //进入房间//场景回调
		}
		break;
		
		default:
		{

		}
		break;
	}

	return 0;
}

void Player::OnEnterScene(int64_t room_id)
{
	SendPlayer(); //发送数据给Client
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

	DEBUG("当前玩家:{}历史战绩:{}", _player_id, message.ShortDebugString());
	
	_stuff.mutable_room_history()->Add(room_id); 
	_dirty = true; 
	
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

/////////////////////////////////////////////////////
//游戏逻辑定义
/////////////////////////////////////////////////////
std::vector<Asset::PAI_OPER_TYPE> Player::CheckPai(const Asset::PaiElement& pai, int64_t from_player_id)
{
	//DEBUG("{} player_id:{} from_player_id:{} card_type:{} card_value:{}", __func__, _player_id, from_player_id, pai.card_type(), pai.card_value());

	std::vector<Asset::PAI_OPER_TYPE> rtn_check;

	if (CheckHuPai(pai)) 
	{
		DEBUG("玩家{}可以胡牌.", _player_id);
		rtn_check.push_back(Asset::PAI_OPER_TYPE_HUPAI);
	}
	if (CheckGangPai(pai, from_player_id)) 
	{
		DEBUG("玩家{}可以杠牌.", _player_id);
		rtn_check.push_back(Asset::PAI_OPER_TYPE_GANGPAI);
	}
	if (CheckPengPai(pai)) 
	{
		DEBUG("玩家{}可以碰牌.", _player_id);
		rtn_check.push_back(Asset::PAI_OPER_TYPE_PENGPAI);
	}
	if (CheckChiPai(pai)) 
	{
		DEBUG("玩家{}可以吃.", _player_id);
		rtn_check.push_back(Asset::PAI_OPER_TYPE_CHIPAI);
	}
		
	return rtn_check;
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
		if (size == 1) 
		{
			CRITICAL("size==1");
			return false;	
		}

		return size == 0 || cards[0] == cards[1]; 
	}

	bool pair = false/*一对*/, straight/*顺子//一套副*/ = false;

	if (!use_pair)
	{
		std::vector<Card_t> sub_cards(cards.begin() + 2, cards.end());

		pair = (cards[0] == cards[1]) && CanHuPai(sub_cards, true);
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
		}
	}
	
	_hu_result.push_back(std::make_tuple(pair, trips, straight));
	
	return pair || trips || straight; //一对、刻或者顺子
}
	
bool Player::CheckBaoHu(const Asset::PaiElement& pai, bool has_fapai)
{
	if (!_game || !_room) return false;

	if (!IsTingPai()) return false; //没有听牌显然不能胡宝牌

	auto baopai = _game->GetBaoPai();

	if (pai.card_type() != baopai.card_type() || pai.card_value() != baopai.card_value())  return false; //不是宝牌
	
	auto options = _room->GetOptions();
	
	auto it_baohu = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_BAOPAI);
	if (it_baohu == options.extend_type().end()) return false; //不带宝胡

	if (_cards_hu.size() == 0) return false;

	auto card = _cards_hu[0];

	//
	//玩家不可能有两张宝牌
	//
	bool deleted = false;

	auto it = std::find(_cards_inhand[pai.card_type()].begin(), _cards_inhand[pai.card_type()].end(), pai.card_value());
	if (it != _cards_inhand[pai.card_type()].end()) //宝牌已经在墙内
	{
		_cards_inhand[pai.card_type()].erase(it);
		deleted = true;
	}

	bool hupai = CheckHuPai(card, false);

	if (deleted)
	{
		_cards_inhand[pai.card_type()].push_back(pai.card_value());
	}

	return hupai;
}
	
bool Player::CheckHuPai(const std::map<int32_t, std::vector<int32_t>>& cards_inhand, //玩家手里的牌
		const std::map<int32_t, std::vector<int32_t>>& cards_outhand, //玩家墙外牌
		const std::vector<Asset::PaiElement>& minggang, //明杠
		const std::vector<Asset::PaiElement>& angang, //暗杠
		int32_t jiangang, //旋风杠，本质是明杠
		int32_t fenggang, //旋风杠，本质是暗杠
		const Asset::PaiElement& pai) //胡牌
{
	if (!_room || !_game) 
	{
		DEBUG_ASSERT(false);
		return false;
	}

	auto cards = cards_inhand;

	for (auto crds : cards_outhand) //复制牌外牌
		cards[crds.first].insert(cards[crds.first].end(), crds.second.begin(), crds.second.end());

	cards[pai.card_type()].push_back(pai.card_value()); //放入可以操作的牌
	
	for (auto& card : cards)
		std::sort(card.second.begin(), card.second.end(), [](int x, int y){ return x < y; }); //由小到大，排序

	////////////////////////////////////////////////////////////////////////////是否可以胡牌的前置检查
	const auto& options = _room->GetOptions();

	////////是否可以缺门、清一色
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
		if (cards[Asset::CARD_TYPE_TIAOZI].size() > 0 || it_bingzi != gang_list.end()) ++has_count; 

		auto it_duanmen = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_DUANMEN);
		if (it_duanmen == options.extend_type().end()) //不可以缺门
		{
			if (has_count < 3) //少于三门显然不行，检查是否清一色
			{
				auto it_yise = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_QIYISE);
				if (it_yise != options.extend_type().end()) //可以清一色
				{
					if (has_count == 2) 
					{
						//TRACE("player_id:{} card_type:{} card_value:{} reason:缺门.", _player_id, pai.card_type(), pai.card_value());
						return false; //不可缺门
					}
				}
				else //断门还不可以清一色
				{
					//TRACE("player_id:{} card_type:{} card_value:{} reason:断门还不可以清一色.", _player_id, pai.card_type(), pai.card_value());
					return false;
				}
			}
		}
	}

	////////是否可以站立胡
	{
		auto it_zhanli = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_ZHANLIHU);
		if (it_zhanli == options.extend_type().end()) //不可以站立胡牌
		{
			if (cards_outhand.size() == 0 && minggang.size() == 0) 
			{
				//TRACE("player_id:{} card_type:{} card_value:{} reason:不可以站立胡牌，且没开门.", _player_id, pai.card_type(), pai.card_value());
				return false; //没开门
			}
		}
	}
	
	////////是否有幺九
	bool has_yao = false;

	for (auto crds : cards) //不同牌类别的牌
	{
		if (crds.second.size() == 0) continue;

		if (crds.first == Asset::CARD_TYPE_WANZI || crds.first == Asset::CARD_TYPE_BINGZI || crds.first == Asset::CARD_TYPE_TIAOZI)
		{
			if (std::find(crds.second.begin(), crds.second.end(), 1) != crds.second.end() || 
					(std::find(crds.second.begin(), crds.second.end(), 9) != crds.second.end())) 
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
	
	for (auto gang : _angang)
	{
		if (gang.card_value() == 1 || gang.card_value() == 9 ||
				gang.card_type() == Asset::CARD_TYPE_FENG || gang.card_type() == Asset::CARD_TYPE_JIAN) has_yao = true;
	}

	if (jiangang > 0 || fenggang > 0) has_yao = true;

	if (!has_yao) 
	{
		//TRACE("player_id:{} card_type:{} card_value:{} reason:没有幺九.", _player_id, pai.card_type(), pai.card_value());
		return false;
	}

	//
	//是否可以满足胡牌的要求
	//
	
	_hu_result.clear();

	std::vector<Card_t> card_list;
	for (auto crds : cards) //不同牌类别的牌
	{
		for (auto value : crds.second)
			card_list.push_back(Card_t(crds.first, value));
	}

	bool can_hu = CanHuPai(card_list);	
	if (!can_hu) 
	{
		//TRACE("player_id:{} card_type:{} card_value:{} reason:牌内无法满足胡牌条件.", _player_id, pai.card_type(), pai.card_value());
		return false;
	}
	
	//
	//防止出现玩家已经碰了6饼，但是牌内含有2、3、4、5、6万，7、8饼
	//
	//收到1、4、7万胡牌的情况
	//
	//这里对牌内牌进行胡牌检查
	//
	{
		auto cards_inhand_check = cards_inhand;
		if (pai.card_type() && pai.card_value()) cards_inhand_check[pai.card_type()].push_back(pai.card_value()); //放入可以操作的牌
			
		for (auto& card : cards_inhand_check)
			std::sort(card.second.begin(), card.second.end(), [](int x, int y){ return x < y; }); //由小到大，排序
	
		std::vector<Card_t> card_list;
		for (auto crds : cards_inhand_check) //胡牌逻辑检查
		{
			for (auto value : crds.second)
				card_list.push_back(Card_t(crds.first, value));
		}
		bool can_hu = CanHuPai(card_list);	
		if (!can_hu) return false;
	}

	//胡牌时至少有一刻子或杠，或有中发白其中一对
	bool has_ke = false;
	int32_t ke_count = 0; //刻数量，一般最多4个，即12张

	for (auto r : _hu_result)
	{
		 bool is_ke = std::get<1>(r);
		 if (is_ke) ++ke_count;
	}

	if (ke_count) has_ke = true;

	if (!has_ke && (/*cards[Asset::CARD_TYPE_FENG].size() //朝阳：两个风牌不顶横 || */cards[Asset::CARD_TYPE_JIAN].size())) has_ke = true;
	
	if (!has_ke && (jiangang > 0 || fenggang > 0 || minggang.size() > 0 || angang.size() > 0)) has_ke = true;
	
	if (!has_ke) return false;

	return true;
}

bool Player::CheckZiMo()
{
	Asset::PaiElement pai;
	return CheckZiMo(pai);
}

bool Player::CheckZiMo(const Asset::PaiElement& pai)
{
	if (_tuoguan_server) return false;

	return CheckHuPai(pai, true);
}
	
bool Player::CheckHuPai(const Asset::PaiElement& pai, bool check_zibo)
{
	if (!_room || !_game)
	{
		DEBUG_ASSERT(false);
		return false;
	}

	PrintPai();

	_fan_list.clear(); //番型清空

	std::map<int32_t/*麻将牌类型*/, std::vector<int32_t>/*牌值*/> cards;

	try {
		std::unique_lock<std::mutex> lock(_card_lock, std::defer_lock);

		if (lock.try_lock()) 
		{
			_hu_result.clear();

			cards = _cards_inhand; //复制当前牌
		}
		else
		{
			ERROR("player_id:{} try locked failed.", _player_id);
			return false;
		}
	}
	catch(const std::system_error& error)
	{
		ERROR("Copy cards from player_id:{} error:{}.", _player_id, error.what());
		return false;
	}

	for (const auto& crds : _cards_outhand) //复制牌外牌
		cards[crds.first].insert(cards[crds.first].end(), crds.second.begin(), crds.second.end());

	//
	//如果非自摸检查，牌已经在玩家手中，没必要再进行插入
	//
	//如果不是自摸检查，则放入手中
	//
	if (!check_zibo && pai.card_type() && pai.card_value()) cards[pai.card_type()].push_back(pai.card_value()); //放入可以操作的牌
	
	for (auto& card : cards)
		std::sort(card.second.begin(), card.second.end(), [](int x, int y){ return x < y; }); //由小到大，排序
			
	bool zhanlihu = false, jiahu = false, bianhu = false, xuanfenggang = false, duanmen = false, yise = false, piao = false; //积分

	//
	//是否可以胡牌的前置检查
	//
	auto options = _room->GetOptions();

	////////是否可以缺门、清一色
	{
		int32_t has_count = 0; //万饼条数量

		auto gang_list = _minggang;
		gang_list.insert(gang_list.end(), _angang.begin(), _angang.end());

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

		auto it_duanmen = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_DUANMEN);
		if (it_duanmen == options.extend_type().end()) //不可以缺门
		{
			if (has_count < 3) //少于三门显然不行，检查是否清一色
			{
				auto it_yise = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_QIYISE);
				if (it_yise != options.extend_type().end()) //可以清一色
				{
					if (has_count == 2) //有两门显然不是清一色
					{
						//DEBUG("player_id:{} card_type:{} card_value:{} reason:缺门也不是清一色.", _player_id, pai.card_type(), pai.card_value());
						return false; //不可缺门
					}
					else // <= 1
					{
						yise = true; //是清一色
					}
				}
				else //断门还不可以清一色
				{
					//DEBUG("player_id:{} card_type:{} card_value:{} reason:缺门.", _player_id, pai.card_type(), pai.card_value());
					return false;
				}
			}
		}
		else //可以断门
		{
			if (has_count < 3) duanmen = true;

			auto it_yise = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_QIYISE);
			if (it_yise != options.extend_type().end()) //可以清一色
			{
				if (has_count <= 1) 
				{
					yise = true;
					duanmen = false; //清一色不算断门
				}
			}
		}
	}
			
	////////是否可以站立胡
	{
		auto it_zhanli = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_ZHANLIHU);
		if (it_zhanli == options.extend_type().end()) //不可以站立胡牌
		{
			if (_cards_outhand.size() == 0 && _minggang.size() == 0) 
			{
				//DEBUG("player_id:{} card_type:{} card_value:{} reason:没开门.", _player_id, pai.card_type(), pai.card_value());
				return false; //没开门
			}
		}
		else
		{
			if (_cards_outhand.size() == 0 && _minggang.size() == 0) zhanlihu = true;
		}
	}
	
	////////是否有幺九
	bool has_yao = false;

	for (auto crds : cards) //不同牌类别的牌
	{
		if (crds.second.size() == 0) continue;

		if (crds.first == Asset::CARD_TYPE_WANZI || crds.first == Asset::CARD_TYPE_BINGZI || crds.first == Asset::CARD_TYPE_TIAOZI)
		{
			if (std::find(crds.second.begin(), crds.second.end(), 1) != crds.second.end() || 
					(std::find(crds.second.begin(), crds.second.end(), 9) != crds.second.end()))
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
	
	for (auto gang : _minggang)
	{
		if (gang.card_value() == 1 || gang.card_value() == 9 || 
				gang.card_type() == Asset::CARD_TYPE_FENG || gang.card_type() == Asset::CARD_TYPE_JIAN) has_yao = true;
	}
	
	for (auto gang : _angang)
	{
		if (gang.card_value() == 1 || gang.card_value() == 9 ||
				gang.card_type() == Asset::CARD_TYPE_FENG || gang.card_type() == Asset::CARD_TYPE_JIAN) has_yao = true;
	}

	if (_jiangang > 0 || _fenggang > 0) 
	{
		has_yao = true;
		xuanfenggang = true;
	}
	
	if (!has_yao) 
	{
		//DEBUG("player_id:{} card_type:{} card_value:{} reason:没幺九.", _player_id, pai.card_type(), pai.card_value());
		return false;
	}
	
	//
	//是否可以宝胡：单独处理
	//

	//
	//是否可以满足胡牌的要求
	//
	
	std::vector<Card_t> card_list;
	for (auto crds : cards) //胡牌逻辑检查
	{
		for (auto value : crds.second)
			card_list.push_back(Card_t(crds.first, value));
	}
	
	bool can_hu = CanHuPai(card_list);	
	if (!can_hu) 
	{
		//DEBUG("player_id:{} card_type:{} card_value:{} reason:牌内无法满足胡牌条件.", _player_id, pai.card_type(), pai.card_value());
		return false;
	}
	
	//
	//胡牌时至少有刻子或杠，或有中发白
	//
	bool has_ke = false;
	int32_t ke_count = 0, shunzi_count = 0; 

	for (auto r : _hu_result)
	{
		 bool is_ke = std::get<1>(r);
		 if (is_ke) ++ke_count;
		 
		 bool is_shunzi = std::get<2>(r);
		 if (is_shunzi) ++shunzi_count;
	}

	if (ke_count) has_ke = true;

	if (!has_ke && (cards[Asset::CARD_TYPE_FENG].size() || cards[Asset::CARD_TYPE_JIAN].size())) has_ke = true;
	
	if (!has_ke && (_jiangang > 0 || _fenggang > 0 || _minggang.size() > 0 || _angang.size() > 0)) has_ke = true;
	
	if (!has_ke) 
	{
		//DEBUG("player_id:{} card_type:{} card_value:{} reason:没有刻.", _player_id, pai.card_type(), pai.card_value());
		return false;
	}

	//
	//飘胡检查，严重依赖上面的刻的数量
	//

	//auto ke_total = ke_count + _jiangang + _fenggang + _minggang.size() + _angang.size();

	if (shunzi_count == 0) 
	{
		piao = true; //未能处理：玩家吃了三套副一样的，如 7 7 7 8 8 8 9 9 9
	}

	//
	//防止出现玩家已经碰了6饼，但是牌内含有2、3、4、5、6万，7、8饼
	//
	//收到1、4、7万胡牌的情况
	//
	//这里对牌内牌进行胡牌检查
	//
	{
		auto cards_inhand_check = _cards_inhand;
		if (!check_zibo && pai.card_type() && pai.card_value()) cards_inhand_check[pai.card_type()].push_back(pai.card_value()); //放入可以操作的牌
			
		for (auto& card : cards_inhand_check)
			std::sort(card.second.begin(), card.second.end(), [](int x, int y){ return x < y; }); //由小到大，排序
	
		std::vector<Card_t> card_list;
		for (auto crds : cards_inhand_check) //玩家牌内胡牌逻辑检查
		{
			for (auto value : crds.second)
				card_list.push_back(Card_t(crds.first, value));
		}
		bool can_hu = CanHuPai(card_list);	
		if (!can_hu) return false;
	}
	
	//是否是夹胡
	{
		auto cards_inhand_check = _cards_inhand;
			
		for (auto& card : cards_inhand_check)
			std::sort(card.second.begin(), card.second.end(), [](int x, int y){ return x < y; }); //由小到大，排序

		if (check_zibo)
		{
			auto it = std::find(cards_inhand_check[pai.card_type()].begin(), cards_inhand_check[pai.card_type()].end(), pai.card_value());
			if (it == cards_inhand_check[pai.card_type()].end())
			{
				DEBUG_ASSERT(false && "未能找到牌");
				return false;
			}

			cards_inhand_check[pai.card_type()].erase(it);
		}
	
		std::vector<Card_t> card_list;
		for (auto crds : cards_inhand_check) //玩家牌内胡牌逻辑检查
		{
			for (auto value : crds.second)
				card_list.push_back(Card_t(crds.first, value));
		}

		if (pai.card_value() == 3)
		{
			Card_t card_t2(pai.card_type(), pai.card_value() - 1);
			auto it2 = std::find(card_list.begin(), card_list.end(), card_t2);

			Card_t card_t1(pai.card_type(), pai.card_value() - 2);
			auto it1 = std::find(card_list.begin(), card_list.end(), card_t1);

			if (it1 != card_list.end() && it2 != card_list.end())
			{
				it2 = std::find(card_list.begin(), card_list.end(), card_t2);
				card_list.erase(it2);
				it1 = std::find(card_list.begin(), card_list.end(), card_t1);
				card_list.erase(it1);
				
				bianhu = CanHuPai(card_list);	
			}
		}
		else if (pai.card_value() == 7)
		{
			Card_t card_t8(pai.card_type(), pai.card_value() + 1);
			auto it8 = std::find(card_list.begin(), card_list.end(), card_t8);

			Card_t card_t9(pai.card_type(), pai.card_value() + 2);
			auto it9 = std::find(card_list.begin(), card_list.end(), card_t9);
			
			if (it8 != card_list.end() && it9 != card_list.end())
			{
				it8 = std::find(card_list.begin(), card_list.end(), card_t8);
				card_list.erase(it8);
				it9 = std::find(card_list.begin(), card_list.end(), card_t9);
				card_list.erase(it9);
				
				bianhu = CanHuPai(card_list);	
			}
		}

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
				
				jiahu = CanHuPai(card_list);	
			}
		}

		auto it = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_JIAHU);

		if (it != options.extend_type().end()) //支持夹胡
		{
			if (bianhu)
			{
				_fan_list.emplace(Asset::FAN_TYPE_JIA_HU_MIDDLE);
			}
			else if (jiahu)
			{
				if (pai.card_value() == 5) 
				{
					_fan_list.emplace(Asset::FAN_TYPE_JIA_HU_HIGHER);
				}
				else
				{
					_fan_list.emplace(Asset::FAN_TYPE_JIA_HU_NORMAL);
				}
			}
		}
	}
	
	if (zhanlihu) //站立胡
	{
		_fan_list.emplace(Asset::FAN_TYPE_ZHAN_LI);
	}
	if (duanmen) //断门
	{
		_fan_list.emplace(Asset::FAN_TYPE_DUAN_MEN);
	}
	if (yise) //清一色
	{
		_fan_list.emplace(Asset::FAN_TYPE_QING_YI_SE);
	}
	if (piao) //飘胡
	{
		_fan_list.emplace(Asset::FAN_TYPE_PIAO_HU);
	}
	if (IsGangOperation()) //杠上开
	{
		_fan_list.emplace(Asset::FAN_TYPE_GANG_SHANG_KAI);
	}
	if (_game->IsLiuJu()) //海底捞月
	{
		_fan_list.emplace(Asset::FAN_TYPE_HAI_DI_LAO);
	}
	if (IsMingPiao()) //明飘
	{
		_fan_list.emplace(Asset::FAN_TYPE_MING_PIAO);
	}
	if (IsDanDiao()) //单调
	{
		_fan_list.emplace(Asset::FAN_TYPE_JIA_HU_NORMAL);
	}
	
	return true;
}
	
bool Player::DebugCheckHuPai(const Asset::PaiElement& pai, bool check_zibo)
{
	_fan_list.clear(); 
	_hu_result.clear();

	auto cards = _cards_inhand; //复制当前牌

	for (const auto& crds : _cards_outhand) //复制牌外牌
		cards[crds.first].insert(cards[crds.first].end(), crds.second.begin(), crds.second.end());

	//
	//如果非自摸检查，牌已经在玩家手中，没必要再进行插入
	//
	//如果不是自摸检查，则放入手中
	//
	if (!check_zibo && pai.card_type() && pai.card_value()) cards[pai.card_type()].push_back(pai.card_value()); //放入可以操作的牌
	
	for (auto& card : cards)
		std::sort(card.second.begin(), card.second.end(), [](int x, int y){ return x < y; }); //由小到大，排序
			
	bool zhanlihu = false, jiahu = false, bianhu = false, xuanfenggang = false, duanmen = false, yise = false, piao = false; //积分

	//
	//是否可以胡牌的前置检查
	//
	Asset::RoomOptions options;
	options.mutable_extend_type()->Add(Asset::ROOM_EXTEND_TYPE_ZHANLIHU);
	options.mutable_extend_type()->Add(Asset::ROOM_EXTEND_TYPE_JIAHU);
	options.mutable_extend_type()->Add(Asset::ROOM_EXTEND_TYPE_XUANFENGGANG);
	options.mutable_extend_type()->Add(Asset::ROOM_EXTEND_TYPE_BAOPAI);
	options.mutable_extend_type()->Add(Asset::ROOM_EXTEND_TYPE_DUANMEN);
	options.mutable_extend_type()->Add(Asset::ROOM_EXTEND_TYPE_QIYISE);
	options.mutable_extend_type()->Add(Asset::ROOM_EXTEND_TYPE_BAOSANJIA);

	////////是否可以缺门、清一色
	{
		int32_t has_count = 0; //万饼条数量

		auto gang_list = _minggang;
		gang_list.insert(gang_list.end(), _angang.begin(), _angang.end());

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
		if (cards[Asset::CARD_TYPE_TIAOZI].size() > 0 || it_bingzi != gang_list.end()) ++has_count; 

		auto it_duanmen = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_DUANMEN);
		if (it_duanmen == options.extend_type().end()) //不可以缺门
		{
			if (has_count < 3) //少于三门显然不行，检查是否清一色
			{
				auto it_yise = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_QIYISE);
				if (it_yise != options.extend_type().end()) //可以清一色
				{
					if (has_count == 2) //有两门显然不是清一色
					{
						ERROR("player_id:{} card_type:{} card_value:{} reason:缺门也不是清一色.", _player_id, pai.card_type(), pai.card_value());
						return false; //不可缺门
					}
					else // <= 1
					{
						yise = true; //是清一色
					}
				}
				else //断门还不可以清一色
				{
					ERROR("player_id:{} card_type:{} card_value:{} reason:缺门.", _player_id, pai.card_type(), pai.card_value());
					return false;
				}
			}
		}
		else //可以断门
		{
			if (has_count < 3) duanmen = true;

			auto it_yise = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_QIYISE);
			if (it_yise != options.extend_type().end()) //可以清一色
			{
				if (has_count <= 1) 
				{
					yise = true;
					duanmen = false; //清一色不算断门
				}
			}
		}
	}
			
	////////是否可以站立胡
	{
		auto it_zhanli = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_ZHANLIHU);
		if (it_zhanli == options.extend_type().end()) //不可以站立胡牌
		{
			if (_cards_outhand.size() == 0 && _minggang.size() == 0) 
			{
				ERROR("player_id:{} card_type:{} card_value:{} reason:没开门.", _player_id, pai.card_type(), pai.card_value());
				return false; //没开门
			}
		}
		else
		{
			if (_cards_outhand.size() == 0 && _minggang.size() == 0) zhanlihu = true;
		}
	}
	
	////////是否有幺九
	bool has_yao = false;

	for (auto crds : cards) //不同牌类别的牌
	{
		if (crds.second.size() == 0) continue;

		if (crds.first == Asset::CARD_TYPE_WANZI || crds.first == Asset::CARD_TYPE_BINGZI || crds.first == Asset::CARD_TYPE_TIAOZI)
		{
			if (std::find(crds.second.begin(), crds.second.end(), 1) != crds.second.end() || 
					(std::find(crds.second.begin(), crds.second.end(), 9) != crds.second.end()))
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
	
	for (auto gang : _minggang)
	{
		if (gang.card_value() == 1 || gang.card_value() == 9 || 
				gang.card_type() == Asset::CARD_TYPE_FENG || gang.card_type() == Asset::CARD_TYPE_JIAN) has_yao = true;
	}
	
	for (auto gang : _angang)
	{
		if (gang.card_value() == 1 || gang.card_value() == 9 ||
				gang.card_type() == Asset::CARD_TYPE_FENG || gang.card_type() == Asset::CARD_TYPE_JIAN) has_yao = true;
	}

	if (_jiangang > 0 || _fenggang > 0) 
	{
		has_yao = true;
		xuanfenggang = true;
	}
	
	if (!has_yao) 
	{
		ERROR("player_id:{} card_type:{} card_value:{} reason:没幺九.", _player_id, pai.card_type(), pai.card_value());
		return false;
	}
	
	//
	//是否可以宝胡：单独处理
	//

	//
	//是否可以满足胡牌的要求
	//
	
	std::vector<Card_t> card_list;
	for (auto crds : cards) //胡牌逻辑检查
	{
		for (auto value : crds.second)
			card_list.push_back(Card_t(crds.first, value));
	}
	
	bool can_hu = CanHuPai(card_list);	
	if (!can_hu) 
	{
		ERROR("player_id:{} card_type:{} card_value:{} reason:牌内无法满足胡牌条件.", _player_id, pai.card_type(), pai.card_value());
		return false;
	}
	
	//
	//胡牌时至少有刻子或杠，或有中发白
	//
	bool has_ke = false;
	int32_t ke_count = 0, shunzi_count = 0; 

	for (auto r : _hu_result)
	{
		 bool is_ke = std::get<1>(r);
		 if (is_ke) ++ke_count;

		 bool is_shunzi = std::get<2>(r);
		 if (is_shunzi) ++shunzi_count;
	}

	if (ke_count) has_ke = true;

	if (!has_ke && (cards[Asset::CARD_TYPE_FENG].size() || cards[Asset::CARD_TYPE_JIAN].size())) has_ke = true;
	
	if (!has_ke && (_jiangang > 0 || _fenggang > 0 || _minggang.size() > 0 || _angang.size() > 0)) has_ke = true;
	
	if (!has_ke) 
	{
		ERROR("player_id:{} card_type:{} card_value:{} reason:没有刻.", _player_id, pai.card_type(), pai.card_value());
		return false;
	}

	//
	//飘胡检查，严重依赖上面的刻的数量
	//

	//auto ke_total = ke_count + _jiangang + _fenggang + _minggang.size() + _angang.size();
		
	if (shunzi_count == 0) 
	{
		piao = true; //未能处理：玩家吃了三套副一样的，如 7 7 7 8 8 8 9 9 9
	}

	//
	//防止出现玩家已经碰了6饼，但是牌内含有2、3、4、5、6万，7、8饼
	//
	//收到1、4、7万胡牌的情况
	//
	//这里对牌内牌进行胡牌检查
	//
	{
		auto cards_inhand_check = _cards_inhand;
		if (!check_zibo && pai.card_type() && pai.card_value()) cards_inhand_check[pai.card_type()].push_back(pai.card_value()); //放入可以操作的牌
			
		for (auto& card : cards_inhand_check)
			std::sort(card.second.begin(), card.second.end(), [](int x, int y){ return x < y; }); //由小到大，排序
	
		std::vector<Card_t> card_list;
		for (auto crds : cards_inhand_check) //玩家牌内胡牌逻辑检查
		{
			for (auto value : crds.second)
				card_list.push_back(Card_t(crds.first, value));
		}
		bool can_hu = CanHuPai(card_list);	
		if (!can_hu) return false;
	}
	
	//是否是夹胡
	{
		auto cards_inhand_check = _cards_inhand;
		//if (!check_zibo && pai.card_type() && pai.card_value()) cards_inhand_check[pai.card_type()].push_back(pai.card_value()); //放入可以操作的牌
			
		for (auto& card : cards_inhand_check)
			std::sort(card.second.begin(), card.second.end(), [](int x, int y){ return x < y; }); //由小到大，排序
		
		if (check_zibo)
		{
			auto it = std::find(cards_inhand_check[pai.card_type()].begin(), cards_inhand_check[pai.card_type()].end(), pai.card_value());
			if (it == cards_inhand_check[pai.card_type()].end())
			{
				DEBUG_ASSERT(false && "未能找到牌");
				return false;
			}

			cards_inhand_check[pai.card_type()].erase(it);
		}
	
		std::vector<Card_t> card_list;
		for (auto crds : cards_inhand_check) //玩家牌内胡牌逻辑检查
		{
			for (auto value : crds.second)
				card_list.push_back(Card_t(crds.first, value));
		}

		if (pai.card_value() == 3)
		{
			Card_t card_t2(pai.card_type(), pai.card_value() - 1);
			auto it2 = std::find(card_list.begin(), card_list.end(), card_t2);

			Card_t card_t1(pai.card_type(), pai.card_value() - 2);
			auto it1 = std::find(card_list.begin(), card_list.end(), card_t1);

			if (it1 != card_list.end() && it2 != card_list.end())
			{
				it2 = std::find(card_list.begin(), card_list.end(), card_t2);
				card_list.erase(it2);
				it1 = std::find(card_list.begin(), card_list.end(), card_t1);
				card_list.erase(it1);
				
				bianhu = CanHuPai(card_list);	
			}
		}
		else if (pai.card_value() == 7)
		{
			Card_t card_t8(pai.card_type(), pai.card_value() + 1);
			auto it8 = std::find(card_list.begin(), card_list.end(), card_t8);

			Card_t card_t9(pai.card_type(), pai.card_value() + 2);
			auto it9 = std::find(card_list.begin(), card_list.end(), card_t9);
			
			if (it8 != card_list.end() && it9 != card_list.end())
			{
				it8 = std::find(card_list.begin(), card_list.end(), card_t8);
				card_list.erase(it8);
				it9 = std::find(card_list.begin(), card_list.end(), card_t9);
				card_list.erase(it9);
				
				bianhu = CanHuPai(card_list);	
			}
		}

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
				
				jiahu = CanHuPai(card_list);	
			}
		}

		auto it = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_JIAHU);

		if (it != options.extend_type().end()) //支持夹胡
		{
			if (bianhu)
			{
				_fan_list.emplace(Asset::FAN_TYPE_JIA_HU_MIDDLE);
			}
			else if (jiahu)
			{
				_fan_list.emplace(Asset::FAN_TYPE_JIA_HU_NORMAL);
			}
		}
	}
	
	if (zhanlihu)
	{
		_fan_list.emplace(Asset::FAN_TYPE_ZHAN_LI);
	}
	if (duanmen) 
	{
		_fan_list.emplace(Asset::FAN_TYPE_DUAN_MEN);
	}
	if (yise) 
	{
		_fan_list.emplace(Asset::FAN_TYPE_QING_YI_SE);
	}
	if (piao) 
	{
		_fan_list.emplace(Asset::FAN_TYPE_PIAO_HU);
	}
	if (IsGangOperation()) //杠上开
	{
		_fan_list.emplace(Asset::FAN_TYPE_GANG_SHANG_KAI);
	}
	/*
	if (_game->IsLiuJu()) //海底捞月
	{
		_fan_list.emplace(Asset::FAN_TYPE_HAI_DI_LAO);
	}
	*/
	if (IsMingPiao()) //明飘
	{
		_fan_list.emplace(Asset::FAN_TYPE_MING_PIAO);
	}
	
	return true;
}
	
bool Player::IsMingPiao()
{
	auto curr_count = GetCardCount();

	DEBUG("当前玩家:{}手里的牌数量:{}", _player_id, curr_count);

	return curr_count == 1 || curr_count == 2;
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

//
//东西南北中发白单粘不算夹胡
//
//只算饼条万
//
//2017-8-9 23:10 功能注释
//	
bool Player::IsDanDiao() 
{ 
	return false;

	if (_cards_hu.size() != 1) return false;

	auto pai = _cards_hu[0];
	if (pai.card_type() == Asset::CARD_TYPE_FENG || pai.card_type() == Asset::CARD_TYPE_JIAN) return false;

	return true;
} 
	
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
	auto curr_count = GetCardCount();

	if (curr_count > 4) return true; //当前超过4张牌，显然不可能手把一

	switch (oper_type)
	{
		case Asset::PAI_OPER_TYPE_CHIPAI: //吃牌
		{
			return false;
		}
		break;
		
		case Asset::PAI_OPER_TYPE_PENGPAI: //碰牌
		case Asset::PAI_OPER_TYPE_GANGPAI: //杠牌
		{
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

	if (!CheckMingPiao(Asset::PAI_OPER_TYPE_CHIPAI)) return false; //明飘检查

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
	if (!CheckChiPai(pai) || !message) 
	{
		DEBUG_ASSERT(false);
		return;
	}

	Asset::PaiOperation* pai_operate = dynamic_cast<Asset::PaiOperation*>(message);
	if (!pai_operate) return;
	
	std::vector<Asset::PaiElement> cards;
	cards.push_back(pai);

	if (pai_operate->pais().size() != 2) 
	{
		DEBUG_ASSERT(false);
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
		DEBUG_ASSERT(false);
		return; //不是顺子
	}

	try {
		std::unique_lock<std::mutex> lock(_card_lock, std::defer_lock);

		if (lock.try_lock()) 
		{
			auto first = std::find(it->second.begin(), it->second.end(), pai_operate->pais(0).card_value());
			if (first == it->second.end()) 
			{
				DEBUG_ASSERT(false);
				return; //理论上不会出现
			}
			
			TRACE("delete pai from player_id:{}, card_type:{}, card_value:{}", _player_id, pai_operate->pais(0).card_type(), pai_operate->pais(0).card_value());
			it->second.erase(first); //删除

			auto second = std::find(it->second.begin(), it->second.end(), pai_operate->pais(1).card_value());
			if (second == it->second.end()) 
			{
				DEBUG_ASSERT(false);
				return; //理论上不会出现
			}

			TRACE("delete pai from player_id:{}, card_type:{}, card_value:{}", _player_id, pai_operate->pais(1).card_type(), pai_operate->pais(1).card_value());
			it->second.erase(second); //删除

			for (const auto& card : cards)
			{
				_cards_outhand[card.card_type()].push_back(card.card_value());

				if (card.card_type() != pai.card_type() || card.card_value() != pai.card_value()) _game->Add2CardsPool(card.card_type(), card.card_value());
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
	
	///////////////////////旋风杠检查///////////////////////
	auto xuanfeng_gang = CheckXuanFeng();
	if (xuanfeng_gang)
	{
		Asset::PaiOperationAlert alert;
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xuanfeng_gang);
		SendProtocol(alert); //提示Client
	}

	SynchronizePai();
}

bool Player::CheckPengPai(const Asset::PaiElement& pai)
{
	if (_has_ting || _tuoguan_server) return false; //已经听牌，不再提示
	
	if (!CheckMingPiao(Asset::PAI_OPER_TYPE_PENGPAI)) return false; //明飘检查

	auto it = _cards_inhand.find(pai.card_type());
	if (it == _cards_inhand.end()) return false;

	int32_t card_value = pai.card_value();
	int32_t count = std::count_if(it->second.begin(), it->second.end(), [card_value](int32_t value) { return card_value == value; });

	if (count < 2) return false;
	
	return true;
}

void Player::OnPengPai(const Asset::PaiElement& pai)
{
	if (!CheckPengPai(pai)) 
	{
		DEBUG_ASSERT(false);
		return;
	}
	
	auto it = _cards_inhand.find(pai.card_type());
	if (it == _cards_inhand.end()) return; //理论上不会如此
	
	try {
		std::unique_lock<std::mutex> lock(_card_lock, std::defer_lock);

		if (lock.try_lock()) 
		{
			for (int i = 0; i < 2; ++i)
			{
				auto iit = std::find(it->second.begin(), it->second.end(), pai.card_value()); //从玩家手里删除
				if (iit == it->second.end()) 
				{
					DEBUG_ASSERT(false); //理论上不会如此
					return;
				}

				it->second.erase(iit);
				DEBUG("delete card from player_id:{} card_type:{} card_vale:{}", _player_id, pai.card_type(), pai.card_value());
			}

			for (int i = 0; i < 3; ++i) _cards_outhand[pai.card_type()].push_back(pai.card_value());
			
			for (int i = 0; i < 2; ++i) _game->Add2CardsPool(pai);
		}
		else
		{
			ERROR("player_id:{} try locked failed.", _player_id);
			return;
		}
	}
	catch(const std::system_error& error)
	{
		ERROR("Delete card from player_id:{} card_type:{} card_value:{} error:{}.", _player_id, pai.card_type(), pai.card_value(), error.what());
		return;
	}
	
	///////////////////////旋风杠检查///////////////////////
	auto xuanfeng_gang = CheckXuanFeng();
	if (xuanfeng_gang)
	{
		Asset::PaiOperationAlert alert;
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xuanfeng_gang);
		SendProtocol(alert); //提示Client
	}
	
	SynchronizePai();
}

bool Player::CheckGangPai(const Asset::PaiElement& pai, int64_t from_player_id)
{
	if (_tuoguan_server) return false;

	if (!CheckMingPiao(Asset::PAI_OPER_TYPE_GANGPAI)) return false; //明飘检查

	auto it = _cards_inhand.find(pai.card_type());
	int32_t card_value = pai.card_value();

	if (it != _cards_inhand.end()) 
	{
		int32_t count = std::count(it->second.begin(), it->second.end(), card_value);
		if (count == 3 /*牌是来自其他玩家*/ || count == 4 /*牌是玩家自己抓的*/) return true;  //玩家手里需要有3|4张牌
	}

	if (from_player_id == _player_id) //玩家自己抓牌
	{
		auto it = _cards_outhand.find(pai.card_type()); //牌面的牌不做排序,顺序必须3张

		auto first_it = std::find(it->second.begin(), it->second.end(), card_value);

		if (first_it == it->second.end()) return false;

		auto second_it = ++first_it;
		if (second_it == it->second.end()) return false;

		auto third_it = ++second_it;
		if (third_it == it->second.end()) return false;
		
		if ((*first_it == *second_it) && (*second_it == *third_it)) return true;  //玩家牌面有3张牌
	}
		
	return false;
}

bool Player::CheckAllGangPai(::google::protobuf::RepeatedField<Asset::PaiOperationAlert_AlertElement>& gang_list)
{
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
			if (count == 4) 
			{
				Asset::PaiElement pai;
				pai.set_card_type((Asset::CARD_TYPE)card_type);
				pai.set_card_value(card_value);

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
		DEBUG_ASSERT(cards.second.size() % 3 == 0);

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
			
			//
			//明杠，不能改变现有听牌的牌型
			//
			if (HasTingPai())
			{
				if (_zhuapai.card_type() != card_type || _zhuapai.card_value() != card_value) 
				{
					LOG(ERROR, "玩家:{}杠牌检查，不能改变现有听牌牌型:{} 当前抓牌:{}", _player_id, pai.ShortDebugString(), _zhuapai.ShortDebugString());
					continue;
				}
			}
			auto gang = gang_list.Add();
			gang->mutable_pai()->CopyFrom(pai); 
			gang->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_GANGPAI);
		}
	}

	return gang_list.size() > 0;
}
	
void Player::OnGangPai(const Asset::PaiElement& pai, int64_t from_player_id)
{
	if (!CheckGangPai(pai, from_player_id)) 
	{
		DEBUG_ASSERT(false);
		return;
	}
	
	int32_t card_type = pai.card_type();
	int32_t card_value = pai.card_value();

	//
	//手里满足杠牌
	//
	auto it = _cards_inhand.find(card_type);
	if (it == _cards_inhand.end()) 
	{
		DEBUG_ASSERT(false);
		return; //理论上不会如此
	}
	
	auto count = std::count(it->second.begin(), it->second.end(), card_value); //玩家手里多少张牌

	if (count == 3)
		_minggang.push_back(pai);
	else if (count == 4)
		_angang.push_back(pai);
	
	try {
		std::unique_lock<std::mutex> lock(_card_lock, std::defer_lock);

		if (lock.try_lock()) 
		{
			auto remove_it = std::remove(it->second.begin(), it->second.end(), card_value); //从玩家手里删除
			it->second.erase(remove_it, it->second.end());
			DEBUG("delete card from player_id:{} card_type:{} card_vale:{}", _player_id, card_type, card_value);
		}
		else
		{
			ERROR("player_id:{} try locked failed.", _player_id);
			return;
		}
	}
	catch(const std::system_error& error)
	{
		ERROR("Delete card from player_id:{} card_type:{} card_value:{} error:{}.", _player_id, card_type, card_value, error.what());
		return;
	}
	
	//
	//墙外满足杠牌
	//
	auto iit = _cards_outhand.find(card_type);
	if (iit != _cards_outhand.end()) 
	{
		auto count = std::count(iit->second.begin(), iit->second.end(), card_value); //玩家手里多少张牌

		if (count == 3)
		{
			_minggang.push_back(pai);
			
			auto remove_it = std::remove(iit->second.begin(), iit->second.end(), card_value); //从墙外删除
			iit->second.erase(remove_it, iit->second.end());
		}
	}
	
	auto cards = _game->TailPai(1); //从后楼给玩家取一张牌
	if (cards.size() == 0) return;

	OnFaPai(cards);
	
	Asset::PaiOperationAlert alert;
	std::vector<Asset::PaiElement> pais;

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
	//自摸检查
	//
	auto zhuapai = GameInstance.GetCard(cards[0]);
	if (CheckZiMo() || CheckBaoHu(zhuapai))
	{
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_pai()->CopyFrom(zhuapai);
		pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
	}
	//
	//听牌检查
	//
	else if (CheckTingPai(pais))
	{
		for (auto pai : pais) 
		{
			auto pai_perator = alert.mutable_pais()->Add();
			pai_perator->mutable_pai()->CopyFrom(pai);
			pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
		}
	}
	
	if (alert.pais().size()) SendProtocol(alert); //提示Client
	
	SynchronizePai();
}

bool Player::CheckFengGangPai() 
{ 
	//if (_oper_count >= 1) return false;

	return CheckFengGangPai(_cards_inhand); 
}

bool Player::CheckJianGangPai() 
{ 
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

	auto it = _xf_gang.begin();
	auto gang = *it;

	_xf_gang.erase(it);

	return gang;
}

//玩家能不能听牌的检查
//
//玩家打出一张牌后，查看玩家再拿到一张牌后可以胡牌

bool Player::CanTingPai(const Asset::PaiElement& pai)
{
	if (!_room) return false;

	_cards_hu.clear();

	auto options = _room->GetOptions();
	
	auto it_baohu = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_BAOPAI);
	if (it_baohu == options.extend_type().end()) return false; //不带宝胡，绝对不可能听牌

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

	//能否胡万饼条
	for (int card_type = Asset::CARD_TYPE_WANZI; card_type <= Asset::CARD_TYPE_TIAOZI; ++card_type)
	{
		for (int card_value = 1; card_value <= 9; ++card_value)
		{
			Asset::PaiElement pai;
			pai.set_card_type((Asset::CARD_TYPE)card_type);
			pai.set_card_value(card_value);

			if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai)) 
				_cards_hu.push_back(pai);
		}
	}
	
	//能否胡风牌
	for (int card_value = 1; card_value <= 4; ++card_value)
	{
		Asset::PaiElement pai;
		pai.set_card_type(Asset::CARD_TYPE_FENG);
		pai.set_card_value(card_value);

		if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai))
			_cards_hu.push_back(pai);
	}

	//能否胡箭牌
	for (int card_value = 1; card_value <= 3; ++card_value)
	{
		Asset::PaiElement pai;
		pai.set_card_type(Asset::CARD_TYPE_JIAN);
		pai.set_card_value(card_value);

		if (CheckHuPai(cards_inhand, cards_outhand, minggang, angang, jiangang, fenggang, pai)) 
			_cards_hu.push_back(pai);
	}
	
	return _cards_hu.size() > 0;
}


//玩家能不能听牌的检查
//
//玩家打出一张牌后，查看玩家再拿到一张牌后可以胡牌
//
//玩家全部牌检查，即玩家抓牌后检查
	
bool Player::CheckTingPai(std::vector<Asset::PaiElement>& pais)
{
	if (!_room) return false;

	if (_has_ting || _tuoguan_server) return false; //已经听牌，不再提示

	auto options = _room->GetOptions();
	
	auto it_baohu = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_BAOPAI);
	if (it_baohu == options.extend_type().end()) return false; //不带宝胡，绝对不可能听牌
	
	//std::map<int32_t, std::vector<int32_t>> cards_inhand, cards_outhand; 
	//std::vector<Asset::PaiElement> minggang, angang; 
	//int32_t jiangang = 0, fenggang = 0; 
	
	/*
	try {
		cards_inhand = _cards_inhand; //玩家手里牌
		cards_outhand = _cards_outhand; //玩家墙外牌
		minggang = _minggang; //明杠
		angang = _angang; //暗杠
		jiangang = _jiangang; //旋风杠，本质是明杠
		fenggang = _fenggang; //旋风杠，本质是暗杠
	}
	catch(const std::system_error& error)
	{
		ERROR("player_id:{} try locked failed, error:{}", _player_id, error.what());
		return false;
	}
	*/

	auto cards_inhand = _cards_inhand; //玩家手里牌
	auto cards_outhand = _cards_outhand; //玩家墙外牌
	auto minggang = _minggang; //明杠
	auto angang = _angang; //暗杠
	auto jiangang = _jiangang; //旋风杠，本质是明杠
	auto fenggang = _fenggang; //旋风杠，本质是暗杠
	
	auto card_list = cards_inhand; //复制当前牌

	for (auto it = card_list.begin(); it != card_list.end(); ++it)
	{
		for (auto value : it->second)
		{
			auto find_it = std::find(cards_inhand[it->first].begin(), cards_inhand[it->first].end(), value);
			if (find_it == cards_inhand[it->first].end()) continue; //理论上一定能找到
						
			cards_inhand[it->first].erase(find_it); //删除这张牌

			//
			//玩家能否胡牌
			//

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
	if (!_room) return false;

	auto options = _room->GetOptions();

	auto it_xuanfeng = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_XUANFENGGANG);
	if (it_xuanfeng == options.extend_type().end()) return false; //不支持旋风杠

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
	if (!CheckFengGangPai(_cards_inhand)) 
	{
		DEBUG_ASSERT(false);
		return;
	}
	
	auto it = _cards_inhand.find(Asset::CARD_TYPE_FENG);
	if (it == _cards_inhand.end()) return;

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
	//自摸检查
	//
	auto zhuapai = GameInstance.GetCard(cards[0]);
	if (CheckZiMo() || CheckBaoHu(zhuapai))
	{
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_pai()->CopyFrom(zhuapai);
		pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
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
	if (!_room) return false;

	auto options = _room->GetOptions();

	auto it_xuanfeng = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_XUANFENGGANG);
	if (it_xuanfeng == options.extend_type().end()) return false; //不支持

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
	
	try {
		std::unique_lock<std::mutex> lock(_card_lock, std::defer_lock);

		if (lock.try_lock())
		{
			auto it = _cards_inhand.find(Asset::CARD_TYPE_JIAN);
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

	++_jiangang;
	
	//
	//旋风杠检查
	//
	auto xuanfeng_gang = CheckXuanFeng();
	if (xuanfeng_gang)
	{
		Asset::PaiOperationAlert alert;
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xuanfeng_gang);
		if (alert.pais().size()) SendProtocol(alert); //提示Client
	}
}
	
void Player::PreCheckOnFaPai()
{

}

void Player::NormalCheckAfterFaPai(const Asset::PaiElement& pai)
{
}

int32_t Player::OnFaPai(std::vector<int32_t>& cards)
{
	if (!_room || !_game) 
	{
		ERROR("玩家{}牌局数据为空", _player_id);
		return 1;
	}

	//
	//听牌后第一次抓牌，产生宝牌或查看宝牌
	//
	if (IsTingPai())
	{
		DEBUG("玩家:{}听牌之后，检查宝牌，当前次数:{}", _player_id, _oper_count_tingpai);
			
		if (_game->HasBaopai() && _game->GetRemainBaopai() <= 0) 
		{
			ResetBaopai(); 
		}

		if (_oper_count_tingpai == 1) //听牌后第一次抓牌
		{
			if (!_game->HasBaopai())
			{
				_game->OnTingPai(shared_from_this()); //生成宝牌

				if (LookAtBaopai(true)) return 8;
			}
			else
			{
				if (LookAtBaopai(false)) return 9;
			}
		}
	}

	try {
		std::unique_lock<std::mutex> lock(_card_lock, std::defer_lock);

		if (lock.try_lock())
		{
			for (auto card_index : cards) //发牌到玩家手里
			{
				auto card = GameInstance.GetCard(card_index);
				if (card.card_type() == 0 || card.card_value() == 0) return 2; //数据有误

				_cards_inhand[card.card_type()].push_back(card.card_value()); //插入玩家手牌
			}

			for (auto& cards : _cards_inhand) //整理牌
			{
				std::sort(cards.second.begin(), cards.second.end(), [](int x, int y){ return x < y; }); //由小到大
			}
		}
		else
		{
			ERROR("player_id:{} try locked failed.", _player_id);
			return 3;
		}
	}
	catch(const std::system_error& error)
	{
		ERROR("player get cards, player_id:{} error:{}.", _player_id, error.what());
		return 4;
	}

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

		//
		//旋风杠检查
		//
		//缓存旋风杠给玩家
		//
		auto xf_card = _cards_inhand;

		//风牌检查
		while (CheckFengGangPai(xf_card))
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
		//箭牌检查
		while (CheckJianGangPai(xf_card))
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

		if (cards.size() == 14) //庄家检查
		{
			Asset::PaiOperationAlert alert;

			//是否旋风杠
			for (auto gang : _xf_gang)
			{
				auto pai_perator = alert.mutable_pais()->Add();
				pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE(gang));
			}

			//是否可以胡牌
			if (CheckZiMo())
			{
				auto pai_perator = alert.mutable_pais()->Add();
				pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
			}

			if (alert.pais().size()) SendProtocol(alert); //上来即有旋风杠或者胡牌
			
			_xf_gang.clear();
		}
	}
	else if (cards.size() == 1)
	{
		auto card = GameInstance.GetCard(cards[0]);
		_zhuapai = card;

		if (card.card_type() == 0 || card.card_type() == 0)
		{
			LOG(ERROR, "玩家:{} 抓到非法的牌数据:{}", _player_id, card.ShortDebugString());
			DEBUG_ASSERT(false && "错误的牌数据");
		}

		notify.set_data_type(Asset::PaiNotify_CARDS_DATA_TYPE_CARDS_DATA_TYPE_FAPAI); //操作类型:发牌
		notify.mutable_pai()->set_card_type(card.card_type());
		notify.mutable_pai()->set_card_value(card.card_value());

		if (IsTingPai()) ++_oper_count_tingpai; //听牌后发了//抓了多少张牌
			
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
	if (!_game) return false;

	auto baopai = _game->GetBaoPai();

	if (baopai.card_type() != _baopai.card_type() || baopai.card_value() != _baopai.card_value())
	{
		Asset::RandomSaizi proto;
		proto.set_has_rand_saizi(has_saizi);
		proto.set_reason_type(Asset::RandomSaizi_REASON_TYPE_REASON_TYPE_TINGPAI);
		proto.mutable_random_result()->Add(_game->GetRandResult());
		proto.mutable_pai()->CopyFrom(baopai);
		SendProtocol(proto); //通知Client
	}
	
	_baopai = baopai; //设置宝牌

	if (CheckHuPai(baopai)) 
	{
		Jinbao(); //进宝

		Asset::PaiOperationAlert alert;
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_pai()->CopyFrom(baopai);
		pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
		SendProtocol(alert); //进宝 
	
		DEBUG("玩家:{}看宝牌:{}之后胡牌", _player_id, baopai.ShortDebugString());

		return true;
	}

	return false;
}

void Player::OnTingPai()
{
	if (!_game) return;

	_has_ting = true;
	_oper_count_tingpai = 1;

	_game->AddTingPlayer(_player_id);
}

//
//宝牌没有剩余数量则重新进行随机
//
//相当于玩家一直在打股子，选择宝牌
//
void Player::ResetBaopai()
{
	if (!_has_ting) return;
		
	DEBUG("玩家:{}重置宝牌", _player_id);

	while(_game->HasBaopai())
	{
		if (_game->GetRemainBaopai() <= 0) 
		{
			int32_t result = CommonUtil::Random(1, 6);

			auto baopai = _game->GetBaoPai(result);

			DEBUG("玩家{}发起换宝，当前宝牌:{}, 换之后宝牌:{} 打股子:{}", _player_id, _game->GetBaoPai().ShortDebugString(), baopai.ShortDebugString(), result);

			_game->SetBaoPai(baopai);

			if (_game->GetRemainBaopai() > 0) _game->OnRefreshBaopai(_player_id, result);
		}
		else
		{
			return;
		}
	}
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

		card_value_list << "[牌外]" << " card_type:" << pai.first << " card_value:" << outhand_list.str();
	}

	for (const auto& pai : _cards_inhand)
	{
		std::stringstream inhand_list;
		for (auto card_value : pai.second) 
			inhand_list << card_value << " ";

		card_value_list << "[牌内]"	<< " card_type:" << pai.first << " card_value:" << inhand_list.str();
	}
		
	LOG(INFO, "player_id:{} has cards:{}", _player_id, card_value_list.str());
}

void Player::ClearCards() 
{
	/*
	if (_game) 
	{
		ERROR("玩家:{}清理牌数据失败", _player_id);
		return;
	}
	*/
	
	_fan_list.clear(); //番数
	_cards_inhand.clear(); //清理手里牌
	_cards_outhand.clear(); //清理墙外牌
	_cards_pool.clear(); //牌池
	_cards_hu.clear(); //胡牌
 
 	_minggang.clear(); //清理杠牌
	_angang.clear(); //清理杠牌

	_jiangang = 0; //清理旋风杠
	_fenggang = 0; //清理旋风杠
	
	_oper_count_tingpai = 0;
	_oper_count = 0; 
	_has_ting = false;
	_tuoguan_server = false;
	_jinbao = false;
	_last_oper_type = _oper_type = Asset::PAI_OPER_TYPE_BEGIN; //初始化操作
	_player_prop.clear_game_oper_state(); //准备//离开
	_baopai.Clear();
	_zhuapai.Clear();

	if (_game) _game.reset();
}
	
void Player::OnGameOver()
{
	//if (_game) _game.reset();

	ClearCards();
	
	if (_tuoguan_server) OnLogout();
}

int32_t Player::CmdSayHi(pb::Message* message)
{
	auto say_hi = dynamic_cast<const Asset::SayHi*>(message);
	if (!say_hi) return 1;

	auto hi_time = CommonTimerInstance.GetTime();

	DEBUG("player_id:{} hi_time:{} last_hi_time:{}", _player_id, hi_time, _hi_time);
	return 0;
}
	
void Player::SayHi()
{
	_hi_time = CommonTimerInstance.GetTime();
	
	DEBUG("player_id:{} hi_time:{}", _player_id, _hi_time);

	Asset::SayHi message;
	message.set_heart_count(_heart_count);
	SendProtocol(message);
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
	
	DEBUG("player_id:{} has been kickout for:{}", _player_id, kick_out->reason());

	Logout(nullptr);

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
	
void Player::DebugCommand()
{
	_debug_model = true;

	_cards_outhand = {
		{ 1, { 1, 2, 3} },
	}; 

	_cards_inhand = {
		//{ 1, { 4, 5, 6} },
		{ 2, { 3, 3, 3, 3, 4, 5} },
		{ 3, { 6, 6 } },
		{ 4, { 1, 1, 1} },
	};
	
	/*
	std::vector<Asset::PaiElement> _minggang; //明杠
	std::vector<Asset::PaiElement> _angang; //暗杠
	int32_t _jiangang = 0; //旋风杠，本质是明杠
	int32_t _fenggang = 0; //旋风杠，本质是暗杠
	*/

	Asset::PaiElement pai;
	pai.set_card_type((Asset::CARD_TYPE)2);
	pai.set_card_value(3);

	bool can_hu = DebugCheckHuPai(pai, true);	
	if (can_hu)
	{
		WARN("竟然可以胡牌");
	}
	else
	{
		WARN("不可以胡牌");
	}

	WARN("番型:");
	for (auto fan : _fan_list)
	{
		std::cout << fan << "  ";
	}
	std::cout << std::endl;
}
	
const Asset::WechatUnion Player::GetWechat() 
{ 
	Asset::User user;
	auto redis = make_unique<Redis>();
	redis->GetUser(_stuff.account(), user);

	return user.wechat();
}
	
bool Player::CheckCardsInhand()
{
	auto count = GetCardCount(); //牌数量

	if (count == 13 || count == 10 || count == 7 || count == 4 || count == 1) return true;

	return false;
}

void PlayerManager::Update(int32_t diff)
{
	++_heart_count;

	if (_heart_count % 20 == 0) //1s
	{
		for (auto it = _players.begin(); it != _players.end();)
		{
			if (!it->second) 
			{
				it = _players.erase(it);
				WARN("删除空指针玩家，当前玩家数量:{}", _players.size());
				continue;
			}
			else
			{
				++it;
			}

			it->second->Update();
		}
	}
}

void PlayerManager::Emplace(int64_t player_id, std::shared_ptr<Player> player)
{
	if (!player) return;

	//if (_players.find(player_id) == _players.end()) _players.emplace(player_id, player);
	_players[player_id] = player;
}

std::shared_ptr<Player> PlayerManager::GetPlayer(int64_t player_id)
{
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
	auto player = _players[player_id];
	if (player) player.reset();
	
	_players.erase(player_id);
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
