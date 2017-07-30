#include <iostream>

#include <spdlog/spdlog.h>
#include <pbjson.hpp>
#include <cpp_redis/cpp_redis>

#include "Player.h"
#include "Timer.h"
#include "Mall.h"
#include "Protocol.h"
#include "CommonUtil.h"
#include "RedisManager.h"
#include "PlayerName.h"
#include "PlayerCommonReward.h"
#include "PlayerCommonLimit.h"

namespace Adoter
{

namespace spd = spdlog;
extern const Asset::CommonConst* g_const;

Player::Player()
{
	//协议默认处理函数
	_method = std::bind(&Player::DefaultMethod, this, std::placeholders::_1);

	//
	//协议处理回调初始化
	//
	//如果没有在此注册，则认为到游戏逻辑服务器进行处理
	//
	AddHandler(Asset::META_TYPE_SHARE_BUY_SOMETHING, std::bind(&Player::CmdBuySomething, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_SIGN, std::bind(&Player::CmdSign, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_COMMON_PROPERTY, std::bind(&Player::CmdGetCommonProperty, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_SAY_HI, std::bind(&Player::CmdSayHi, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_GAME_SETTING, std::bind(&Player::CmdGameSetting, this, std::placeholders::_1));
	AddHandler(Asset::META_TYPE_SHARE_ROOM_HISTORY, std::bind(&Player::CmdGetBattleHistory, this, std::placeholders::_1));

	AddHandler(Asset::META_TYPE_C2S_GET_REWARD, std::bind(&Player::CmdGetReward, this, std::placeholders::_1));
}

Player::Player(int64_t player_id, std::shared_ptr<WorldSession> session) : Player()
{
	SetID(player_id);	
	_session = session; //地址拷贝
}

bool Player::Connected() 
{ 
	if (!_session) return false; 
	return _session->IsConnect(); 
}

int32_t Player::Load()
{
	if (_player_id == 0) return 1;

	cpp_redis::future_client client;
	client.connect(ConfigInstance.GetString("Redis_ServerIP", "127.0.0.1"), ConfigInstance.GetInt("Redis_ServerPort", 6379));
	if (!client.is_connected()) return 2;
	
	auto has_auth = client.auth(ConfigInstance.GetString("Redis_Password", "!QAZ%TGB&UJM9ol."));
	if (has_auth.get().ko()) 
	{
		DEBUG_ASSERT(false);
		return 3;
	}

	auto get = client.get("player:" + std::to_string(_player_id));
	cpp_redis::reply reply = get.get();
	client.commit();

	if (!reply.is_string()) 
	{
		DEBUG_ASSERT(false);
		return 4;
	}

	auto success = _stuff.ParseFromString(reply.as_string());
	if (!success)
	{
		DEBUG_ASSERT(false);
		return 5;
	}

	//auto redis = make_unique<Redis>();
	//auto success = redis->GetPlayer(_player_id, _stuff);

	/*

	if (!success) 
	{
		std::string player_name = NameInstance.Get();
		SetName(player_name); //如果没有这个玩家则创建，防止因为创建账号但是没有角色的情况

		redis->SavePlayer(_player_id, _stuff); //如果没有的角色，则创建一个

		LOG(ERROR, "加载玩家数据失败:{}, 重新生成玩家数据", _player_id);
	}
	*/
		
	return 0;
}

//
//数据存储，如果玩家当前在中心服则以中心服为准
//
//如果玩家已经不在中心服，则去游戏逻辑服进行存储
//
//以免数据覆盖
//
int32_t Player::Save(bool force)
{
	PLAYER(_stuff);	//数据日志

	if (!force && !IsDirty()) return 1;

	if (!IsCenterServer()) return 2; 

	auto redis = make_unique<Redis>();
	auto success = redis->SavePlayer(_player_id, _stuff); 
	if (!success) return 3;

	_dirty = false;
	
	DEBUG("玩家:{}保存数据，数据内容:{}", _player_id, _stuff.ShortDebugString());
		
	return 0;
}
	
int32_t Player::Logout(pb::Message* message)
{
	OnLogout();
	
	return 0;
}
	
int32_t Player::OnLogout()
{
	_stuff.set_login_time(0);
	_stuff.set_logout_time(CommonTimerInstance.GetTime());
	
	Save(true);	//存档数据库

	WorldSessionInstance.RemovePlayer(_player_id); //网络会话数据
	PlayerInstance.Erase(_player_id); //玩家管理

	DEBUG("player_id:{} stuff:{} leave game and scene.", _player_id, _stuff.ShortDebugString());

	return 0;
}

int32_t Player::OnEnterGame() 
{
	if (Load())
	{
		LOG(ERROR, "加载数据失败");
		return 1;
	}

	SendPlayer(); //发送数据给玩家
	
	_stuff.set_login_time(CommonTimerInstance.GetTime());
	_stuff.set_logout_time(0);

	SetDirty(); //存盘

	PLAYER(_stuff);	//BI日志

	WorldSessionInstance.AddPlayer(_player_id, _session); //网络会话数据
	PlayerInstance.Emplace(_player_id, shared_from_this()); //玩家管理

	OnLogin();

	return 0;
}
	
int32_t Player::OnLogin()
{
	ActivityInstance.OnPlayerLogin(shared_from_this()); //活动数据

	BattleHistory(); //历史对战表

	DEBUG("玩家:{}登陆加载数据，数据内容:{}", _player_id, _stuff.ShortDebugString());
	return 0;
}

void Player::SetLocalServer(int32_t server_id) 
{ 
	DEBUG("玩家:{} 所在服务器:{} 新服务器:{}", _player_id, _stuff.server_id(), server_id);

	_stuff.set_server_id(server_id); 

	_dirty = true;
}
	
bool Player::IsCenterServer() 
{ 
	return _stuff.server_id() == ConfigInstance.GetInt("ServerID", 1); 
}
	
int64_t Player::ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE changed_type, int64_t count)
{
	if (count <= 0) return 0;

	if (!CheckRoomCard(count)) return 0;

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

void Player::SendProtocol(const pb::Message* message)
{
	SendProtocol(*message);
}

void Player::SendProtocol(const pb::Message& message)
{
	if (!Connected()) return;

	GetSession()->SendProtocol(message);

	//调试
	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return;

	const pb::EnumValueDescriptor* enum_value = message.GetReflection()->GetEnum(message, field);
	if (!enum_value) return;

	DEBUG("send protocol to player_id:{} protocol_name:{} content:{}", _player_id, enum_value->name().c_str(), message.ShortDebugString());
}
	
void Player::SendMeta(const Asset::Meta& meta)
{
	if (!Connected()) return;

	GetSession()->SendMeta(meta);
}
	
bool Player::SendProtocol2GameServer(const pb::Message& message)
{
	auto _gs_session = WorldSessionInstance.GetServerSession(GetLocalServer());
	if (!_gs_session) return false;

	const pb::FieldDescriptor* field = message.GetDescriptor()->FindFieldByName("type_t");
	if (!field) return false;
	
	int type_t = field->default_value_enum()->number();
	if (!Asset::META_TYPE_IsValid(type_t)) return false;	//如果不合法，不检查会宕线
	
	Asset::Meta meta;
	meta.set_type_t((Asset::META_TYPE)type_t);
	meta.set_stuff(message.SerializeAsString());
	meta.set_player_id(_player_id); 

	DEBUG("玩家{}发送协议{}到逻辑服务器", _player_id, message.ShortDebugString());

	_gs_session->SendMeta(meta); 

	return true;
}

bool Player::SendProtocol2GameServer(const pb::Message* message)
{
	auto _gs_session = WorldSessionInstance.GetServerSession(GetLocalServer());
	if (!_gs_session || !message) return false;

	SendProtocol2GameServer(*message); 
	
	return true;
}

//
//玩家心跳周期为10MS
//
//如果该函数返回FALSE则表示掉线
//
bool Player::Update()
{
	++_heart_count; //心跳
	
	if (_heart_count % 100 == 0) //1s
	{
		CommonLimitUpdate(); //通用限制,定时更新
	}
	
	if (_heart_count % 300 == 0) //3s
	{
		if (_dirty) Save(); //触发存盘
	}
	
	if (_heart_count % 10000 == 0) //100s
	{
		SayHi();
	}

	if (_heart_count % 6000 == 0) //1min
	{
		TRACE("heart_count:{} player_id:{}", _heart_count, _player_id);
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
	if (!message) return false;

	DEBUG("当前玩家{}所在服务器:{} 接收协议数据:{}", _player_id, _stuff.server_id(), message->ShortDebugString());
	//
	//如果中心服务器没有协议处理回调，则发往游戏服务器进行处理
	//
	//如果玩家已经在游戏逻辑服务器，则直接发往游戏逻辑服务器，防止数据覆盖
	//
	auto it = _callbacks.find(type_t);
	if (it == _callbacks.end() && IsCenterServer()) //还在中心服
	{
		if (IsCenterServer())
		{
			int64_t server_id = WorldSessionInstance.RandomServer(); //随机一个逻辑服务器
			if (server_id == 0) 
			{
				LOG(ERROR, "玩家:{}未能分配到逻辑服务器", _player_id);
				DEBUG_ASSERT(false);
			}

			SetLocalServer(server_id);
		}
		
		SendProtocol2GameServer(message); //转发给游戏逻辑服务器进行处理
	}
	else if (!IsCenterServer())
	{
		SendProtocol2GameServer(message); //转发给游戏逻辑服务器进行处理
	}
	else
	{
		CallBack& callback = GetMethod(type_t); 
		callback(std::forward<pb::Message*>(message));	
	}

	return true;
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

void Player::SendPlayer()
{
	Asset::PlayerInformation player_info;
	player_info.mutable_player()->CopyFrom(this->_stuff);

	SendProtocol(player_info);
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
	if (!some_thing) return false;

	int64_t mall_id = some_thing->mall_id();
	if (mall_id <= 0) return false;

	auto ret = MallInstance.BuySomething(shared_from_this(), mall_id);
	some_thing->set_result(ret);

	SendProtocol(some_thing); //返回给Client

	return true;
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

int32_t Player::CmdSayHi(pb::Message* message)
{
	auto say_hi = dynamic_cast<const Asset::SayHi*>(message);
	if (!say_hi) return 1;
    
	using namespace std::chrono;

    if (_last_hi_time == steady_clock::time_point())
    {
        _last_hi_time = steady_clock::now();
	}
	else
	{
        steady_clock::time_point now = steady_clock::now();

        steady_clock::duration duration_pass = now - _last_hi_time;

        _last_hi_time = now;

        if (duration_pass > seconds(10))
        {
            ++_over_speed_pings;
			
            int32_t max_allowed = 3;

            if (max_allowed && _over_speed_pings > max_allowed) return 2;
	    }
	    else
		{
			_over_speed_pings = 0;
		}
	}

	if (!_session) return false;

	SayHi();

	return 0;
}
	
void Player::SayHi()
{
	Asset::SayHi message;
	message.set_heart_count(_heart_count);

	SendProtocol(message);

	DEBUG("player_id:{} _heart_count:{}", _player_id, _heart_count);
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
	
int32_t Player::CmdGetBattleHistory(pb::Message* message)
{
	auto battle_history = dynamic_cast<const Asset::BattleHistory*>(message);
	if (!battle_history) return 1;

	int32_t start_index = battle_history->start_index();
	int32_t end_index = battle_history->end_index();

	BattleHistory(start_index, end_index);

	return 0;
}

void Player::BattleHistory(int32_t start_index, int32_t end_index)
{
	if (start_index > end_index || start_index < 0 || end_index < 0) return;

	int32_t historty_count = std::min(_stuff.room_history().size(), 5); //最多显示5条记录
	if (historty_count <= 0) return;
	
	if (end_index - start_index > historty_count) return;

	if (end_index == 0) end_index = _stuff.room_history().size();
	if (start_index == 0) start_index = end_index - historty_count;

	Asset::BattleHistory message;
	message.set_start_index(start_index);
	message.set_end_index(end_index);

	auto redis = make_unique<Redis>();

	for (int32_t i = end_index - 1; i >= start_index; --i)
	{
		if (i < 0 || i >= _stuff.room_history().size()) 
		{
			DEBUG_ASSERT(false);
			continue; //安全检查
		}

		Asset::RoomHistory history;
		auto success = redis->GetRoomHistory(_stuff.room_history(i), history);
		if (success != REDIS_REPLY_STRING) continue;

		auto record = message.mutable_history_list()->Add();
		record->CopyFrom(history);
	}

	if (message.history_list().size()) SendProtocol(message);
}
	
void Player::OnKickOut(Asset::KICK_OUT_REASON reason)
{
	//
	//如果玩家主动退出，数据发送失败
	//
	//如果由于顶号，会优先发给在线玩家
	//
	Asset::KickOut kickout; //提示Client
	kickout.set_player_id(_player_id);
	kickout.set_reason(reason);
	SendProtocol(kickout); 

	Asset::KickOutPlayer kickout_player; //通知游戏逻辑服务器退出
	kickout_player.set_player_id(_player_id);
	kickout_player.set_reason(reason);
	SendProtocol2GameServer(kickout_player); 

	Logout(nullptr);
}
	
void PlayerManager::Emplace(int64_t player_id, std::shared_ptr<Player> player)
{
	if (!player) return;

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

void PlayerManager::Erase(int64_t player_id)
{
	_players.erase(player_id);
}

void PlayerManager::Erase(std::shared_ptr<Player> player)
{
	if (!player) return;

	_players.erase(player->GetID());
}
	
void PlayerManager::BroadCast(const pb::Message& message)
{
	for (auto it = _players.begin(); it != _players.end(); ++it)
	{
		auto player = it->second;
		if (!player) continue;

		player->SendProtocol(message); //发送给Client
	}
}

}
